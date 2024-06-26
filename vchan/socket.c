/*
 * The Qubes OS Project, http://www.qubes-os.org
 *
 * Copyright (C) 2020  Paweł Marczewski  <pawel@invisiblethingslab.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

#include <sys/socket.h>
#include <sys/un.h>
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <time.h>

#include "libvchan.h"
#include "libvchan_private.h"

#define CONNECT_DELAY_MS 100

static void run_server(libvchan_t *ctrl, int server_fd);
static void comm_loop(libvchan_t *ctrl, int socket_fd);
static void change_state(libvchan_t *ctrl, int state);

int libvchan__listen(const char *socket_path) {
    int server_fd;

    if (unlink(socket_path) && errno != ENOENT) {
        perror("unlink");
        return -1;
    }

    server_fd = socket(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK, 0);
    if (server_fd < 0) {
        perror("socket");
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);
    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr))) {
        perror("bind");
        close(server_fd);
        return -1;
    }
    if (listen(server_fd, 1)) {
        perror("listen");
        close(server_fd);
        return -1;
    }

    return server_fd;
}

void *libvchan__server(void *arg) {
    sigset_t set;
    sigfillset(&set);
    if (pthread_sigmask(SIG_BLOCK, &set, NULL)) {
        perror("pthread_sigmask");
        return NULL;
    }

    libvchan_t *ctrl = arg;
    run_server(ctrl, ctrl->socket_fd);
    return NULL;
}

void *libvchan__client(void *arg) {
    sigset_t set;
    sigfillset(&set);
    if (pthread_sigmask(SIG_BLOCK, &set, NULL)) {
        perror("pthread_sigmask");
        return NULL;
    }

    libvchan_t *ctrl = arg;
    comm_loop(ctrl, ctrl->socket_fd);
    change_state(ctrl, VCHAN_DISCONNECTED);
    return NULL;
}

int libvchan__connect(const char *socket_path) {

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = CONNECT_DELAY_MS * 1000;

    int socket_fd = socket(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC, 0);
    if (socket_fd < 0) {
        perror("socket");
        return -1;
    }

    while (connect(socket_fd, (struct sockaddr*)&addr, sizeof(addr))) {
        if (errno != ECONNREFUSED && errno != ENOENT) {
            perror("connect");
            close(socket_fd);
            return -1;
        }
        nanosleep(&ts, NULL);
    }

    if (fcntl(socket_fd, F_SETFL, O_NONBLOCK)) {
        perror("fcntl socket");
        close(socket_fd);
        return -1;
    }

    return socket_fd;
}

static void run_server(libvchan_t *ctrl, int server_fd) {
    int done;
    int connected = 0;

    struct pollfd fds[1];
    fds[0].fd = server_fd;
    fds[0].events = POLLIN;
    while (!connected) {
        if (poll(fds, 1, CONNECT_DELAY_MS) < 0 && errno != EINTR) {
            perror("poll server_fd");
            return;
        }
        pthread_mutex_lock(&ctrl->mutex);
        done = ctrl->shutdown;
        pthread_mutex_unlock(&ctrl->mutex);
        if (done)
            return;
        connected = fds[0].revents & POLLIN;
    }

    int socket_fd = -1;
    while (socket_fd < 0) {
        socket_fd = accept(server_fd, NULL, NULL);
        if (socket_fd < 0 && errno != EINTR) {
            perror("accept");
            return;
        }
    }

    if (fcntl(socket_fd, F_SETFL, O_NONBLOCK)) {
        perror("fcntl socket");
        return;
    }

    change_state(ctrl, VCHAN_CONNECTED);
    comm_loop(ctrl, socket_fd);
    change_state(ctrl, VCHAN_DISCONNECTED);

    if (close(socket_fd)) {
        perror("close socket");
    }
}

static void comm_loop(libvchan_t *ctrl, int socket_fd) {
    struct pollfd fds[2];
    fds[0].fd = socket_fd;
    fds[1].fd = ctrl->user_event_pipe[0];
    fds[1].events = POLLIN;
    int done = 0;
    int shutdown = 0;
    while (!done) {
        pthread_mutex_lock(&ctrl->mutex);
        fds[0].events = 0;
        if (ring_available(&ctrl->read_ring) > 0)
            fds[0].events |= POLLIN;
        if (ring_filled(&ctrl->write_ring) > 0)
            fds[0].events |= POLLOUT;
        pthread_mutex_unlock(&ctrl->mutex);

        if (poll(fds, 2, -1) < 0 && errno != EINTR) {
            perror("poll comm_loop");
            return;
        }

        pthread_mutex_lock(&ctrl->mutex);
        shutdown = ctrl->shutdown;

        if (fds[1].revents & POLLIN) {
            libvchan__drain_pipe(ctrl->user_event_pipe[0]);
        }

        int notify = 0;

        // Read from socket into read_ring
        if (fds[0].revents & POLLIN) {
            int size = ring_available(&ctrl->read_ring);
            if (size > 0) {
                int count = read(
                    socket_fd, ring_tail(&ctrl->read_ring), size);
                if (count == 0) {
                    done = 1;
                } else if (count < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                        count = 0;
                    else if (errno == ECONNRESET) {
                        count = 0;
                        done = 1;
                    } else {
                        perror("read from socket");
                        pthread_mutex_unlock(&ctrl->mutex);
                        return;
                    }
                } else
                    notify = 1;
                ring_advance_tail(&ctrl->read_ring, count);
            }
        }

        if (fds[0].revents & POLLOUT) {
            // Write from write_ring into socket
            int size = ring_filled(&ctrl->write_ring);
            if (size > 0) {
                int count = write(
                    socket_fd, ring_head(&ctrl->write_ring), size);
                if (count < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                        count = 0;
                    else if (errno == EPIPE) {
                        count = 0;
                        done = 1;
                    } else {
                        perror("write to socket");
                        pthread_mutex_unlock(&ctrl->mutex);
                        return;
                    }
                } else if (count > 0)
                    notify = 1;
                ring_advance_head(&ctrl->write_ring, count);
            }
        }

        if (notify) {
            uint8_t byte = 0;
            if (write(ctrl->socket_event_pipe[1], &byte, 1) != 1) {
                perror("write");
                pthread_mutex_unlock(&ctrl->mutex);
                return;
            }
        }

        // When shutting down, attempt to flush all data first.
        if (shutdown && ring_filled(&ctrl->write_ring) == 0) {
            done = 1;
        }

        pthread_mutex_unlock(&ctrl->mutex);
    }
}

void change_state(libvchan_t *ctrl, int state) {
    pthread_mutex_lock(&ctrl->mutex);
    ctrl->state = state;
    uint8_t byte = 0;
    if (write(ctrl->socket_event_pipe[1], &byte, 1) != 1)
        perror("write");
    pthread_mutex_unlock(&ctrl->mutex);
}
