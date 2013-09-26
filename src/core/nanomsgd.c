/*
    Copyright (c) 2011 250bpm s.r.o.  All rights reserved.
    Copyright (c) 2013 GoPivotal, Inc.  All rights reserved.

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"),
    to deal in the Software without restriction, including without limitation
    the rights to use, copy, modify, merge, publish, distribute, sublicense,
    and/or sell copies of the Software, and to permit persons to whom
    the Software is furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included
    in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
    THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
    IN THE SOFTWARE.
*/

#include "../utils/err.h"
#include "../utils/alloc.h"
#include "../utils/clock.h"
#include "../utils/list.h"
#include "../utils/cont.h"

#include <stdlib.h>
#include <unistd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/un.h>
#include <poll.h>

struct tmconn {
    int fd;
    unsigned char name_size;
    unsigned char name [256];
    uint64_t timeout;
    struct nn_list_item item;
};

#define IPCCONN_STATE_NAME 1
#define IPCCONN_STATE_ACTIVE 2
#define IPCCONN_STATE_DEAD 3

struct ipcconn {
    int fd;
    int state;
    unsigned char name_size;
    unsigned char name [256];
    uint64_t timeout;
    struct nn_list_item item;
};

static int send_fd (int s, int fd);

int main ()
{
    /*  TODO: For now this is a constant. Later on it should be
        configurable. */
    const int tmport = 9920;

    /*  The lists of open connections. */
    struct nn_list tmconns;
    size_t tmconns_nbr;
    struct nn_list ipcconns;
    size_t ipcconns_nbr;

    int rc;
    int tmsock;
    int ipcsock;
    int opt;
    int fd;
    struct nn_clock clock;
    struct nn_list_item *it;

    nn_clock_init (&clock);

    /*  We start with no connections open. */
    nn_list_init (&tmconns);
    tmconns_nbr = 0;
    nn_list_init (&ipcconns);
    ipcconns_nbr = 0;

    /*  Start listening on the TCP port. */
    tmsock = socket (AF_INET, SOCK_STREAM, IPPROTO_TCP);
    errno_assert (tmsock >= 0);
    opt = 1;
    rc = setsockopt (tmsock, SOL_SOCKET, SO_REUSEADDR, &opt,
        sizeof (opt));
    errno_assert (rc == 0);
    struct sockaddr_in addr;
    memset (&addr, 0, sizeof (addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons (tmport);
    addr.sin_addr.s_addr = INADDR_ANY;
    rc = bind (tmsock, (struct sockaddr*) &addr, sizeof (addr));
    errno_assert (rc == 0);
    rc = listen (tmsock, 100);
    errno_assert (rc == 0);

    /*  Start listening on the IPC endpoint. */
    {
        char ipcaddr [32];
        struct sockaddr_un unaddr;

        snprintf (ipcaddr, sizeof (ipcaddr), "/tmp/tcpmux-%d.ipc",
            (int) tmport);
        unlink (ipcaddr);
        ipcsock = socket (AF_UNIX, SOCK_STREAM, 0);
        errno_assert (ipcsock >= 0);
        nn_assert (strlen (ipcaddr) < sizeof (unaddr.sun_path));
        unaddr.sun_family = AF_UNIX;
        strcpy (unaddr.sun_path, ipcaddr);
        rc = bind (ipcsock, (struct sockaddr*) &unaddr, sizeof (unaddr));
        errno_assert (rc == 0);
        rc = listen (ipcsock, 100);
        errno_assert (rc == 0);
    }

    while (1) {

        struct pollfd *ps;
        void **pshints;
        int pssz_all;
        int pssz_tms;
        int pssz_ipcs;
        int pos;
        uint64_t now;

        /*  Create the pollset. */
        pssz_all = 2 + tmconns_nbr + ipcconns_nbr;
        ps = nn_alloc (sizeof (struct pollfd) * pssz_all, "pollset");
        alloc_assert (ps);
        pshints = nn_alloc (sizeof (void*) * pssz_all, "pollset hints");
        alloc_assert (pshints);

        /*  Fill in the listening sockets. */
        ps [0].fd = tmsock;
        ps [0].events = POLLIN;
        ps [0].revents = 0;
        pshints [0] = NULL;
        ps [1].fd = ipcsock;
        ps [1].events = POLLIN;
        ps [1].revents = 0;
        pshints [1] = NULL;
        pos = 2;

        /*  Fill in all TCPMUX sockets. */
        for (it = nn_list_begin (&tmconns); it != nn_list_end (&tmconns);
              it = nn_list_next (&tmconns, it)) {
            struct tmconn *tm = nn_cont (it, struct tmconn, item);
            nn_assert (pos < pssz_all);
            ps [pos].fd = tm->fd;
            ps [pos].events = POLLIN;
            ps [pos].revents = 0;
            pshints [pos] = tm;
            ++pos;
        }
        pssz_tms = tmconns_nbr;

        /*  Fill in all IPC sockets. */
        for (it = nn_list_begin (&ipcconns); it != nn_list_end (&ipcconns);
              it = nn_list_next (&ipcconns, it)) {
            struct ipcconn *ipc = nn_cont (it, struct ipcconn, item);
            nn_assert (pos < pssz_all);
            ps [pos].fd = ipc->fd;
            ps [pos].events = ipc->state == IPCCONN_STATE_ACTIVE ? 0 : POLLIN;
            ps [pos].revents = 0;
            pshints [pos] = ipc;
            ++pos;
        }
        pssz_ipcs = ipcconns_nbr;

        /*  Wait for an event. */
        rc = poll (&ps [0], pssz_all, 1000);
        errno_assert (rc >= 0);

        /*  Get the current time (to be used later). */
        now = nn_clock_now (&clock);

        /*  If there is a new TCPMUX connection, accept it. */
        if (ps [0].revents & POLLIN) {
            fd = accept (tmsock, NULL, NULL);
            if (!(fd < 0 && errno == ECONNABORTED)) {
                struct tmconn *tm;
                errno_assert (fd >= 0);
                tm = nn_alloc (sizeof (struct tmconn), "TCPMUX connection");
                alloc_assert (tm);
                tm->fd = fd;
                tm->name_size = 0;
                tm->timeout = nn_clock_now (&clock) + 1000000;
                nn_list_item_init (&tm->item);
                nn_list_insert (&tmconns, &tm->item, nn_list_end (&tmconns));
                ++tmconns_nbr;
            }
        }

        /*  If there is a new IPC connection, accept it. */
        if (ps [1].revents & POLLIN) {
            fd = accept (ipcsock, NULL, NULL);
            if (!(fd < 0 && errno == ECONNABORTED)) {
                struct ipcconn *ipc;
                errno_assert (fd >= 0);
                ipc = nn_alloc (sizeof (struct ipcconn), "IPC connection");
                alloc_assert (ipc);
                ipc->fd = fd;
                ipc->state = IPCCONN_STATE_NAME;
                ipc->name_size = 0;
                ipc->timeout = nn_clock_now (&clock) + 1000;
                nn_list_item_init (&ipc->item);
                nn_list_insert (&ipcconns, &ipc->item, nn_list_end (&ipcconns));
                ++ipcconns_nbr;
            }
        }

        /*  Check individual TCPMUX connections. */
        for (pos = 2; pos != 2 + pssz_tms; ++pos) {
            struct tmconn *tm = (struct tmconn*) pshints [pos];

            /*  If the connection is broken or timed out, close it. */
            if (now > tm->timeout || ps [pos].revents & POLLERR ||
                  ps [pos].revents & POLLHUP) {
tm_end:
                rc = close (tm->fd);
                errno_assert (rc == 0);
                nn_list_erase (&tmconns, &tm->item);
                --tmconns_nbr;
                continue;
            }

            /*  Read the service name. */
            if (ps [pos].revents & POLLIN) {
                char c;
                rc = recv (tm->fd, &c, 1, 0);
                if (rc <= 0)
                    goto tm_end;
                nn_assert (rc == 1);

                /*  Test whether the name ends here. */
                if (c == 0xa) {
                    if (tm->name_size > 0 &&
                          tm->name [tm->name_size - 1] == 0xd) {
                        --tm->name_size;
                        tm->name [tm->name_size] = 0;
printf ("TCPMUX: new connection to service '%s'\n", tm->name);

                        /*  The service name is complete here. Find if there
                            is an active IPC connection providing that
                            service. */
                        for (it = nn_list_begin (&ipcconns);
                              it != nn_list_end (&ipcconns);
                              it = nn_list_next (&ipcconns, it)) {
                            struct ipcconn *ipc = nn_cont (it, struct ipcconn,
                                item);
                            if (ipc->state != IPCCONN_STATE_ACTIVE)
                                continue;
                            if (strcmp (tm->name, ipc->name) == 0) {
                                rc = send_fd (ipc->fd, tm->fd);
                                if (rc < 0)
                                    ipc->state = IPCCONN_STATE_DEAD;
                                rc = close (tm->fd);
                                errno_assert (rc == 0);
                                break;
                            }
                        }

                        /*  If the service was not found,
                            close the connection. */
                        if (it == nn_list_end (&ipcconns)) {
printf ("TCPMUX: service not found\n");
                            goto tm_end;
                        }
                    }
                }

                /*  The service name can have at most 255 characters. */
                if (tm->name_size == 255)
                    goto tm_end;

                /*  Add the new character to the service name. */
                tm->name [tm->name_size] = c;
                ++tm->name_size;
            }
        }

        /*  Check individual IPC connections. */
        for (; pos != pssz_all; ++pos) {
            struct ipcconn *ipc = (struct ipcconn*) pshints [pos];

            /*  If the connection is broken or timed out, close it. */
            if (ipc->state == IPCCONN_STATE_DEAD ||
                  (ipc->state == IPCCONN_STATE_NAME && now > ipc->timeout) ||
                  ps [pos].revents & POLLERR || ps [pos].revents & POLLHUP) {
printf ("TCPMUX: service unregistered (1)\n");
ipc_end:
                rc = close (ipc->fd);
                errno_assert (rc == 0);
                nn_list_erase (&ipcconns, &ipc->item);
                --ipcconns_nbr;
                continue;
            }

            /*  Read the service name. */
            if (ps [pos].revents & POLLIN) {
                char c;
                rc = recv (ipc->fd, &c, 1, 0);
                if (rc <= 0) {
printf ("TCPMUX: service unregistered (2)\n");
                    goto ipc_end;
                }
                nn_assert (rc == 1);

                /*  Test whether the name ends here. */
                if (c == 0xa) {
                    if (ipc->name_size > 0 &&
                          ipc->name [ipc->name_size - 1] == 0xd) {
                        --ipc->name_size;
                        ipc->name [ipc->name_size] = 0;
printf ("TCPMUX: service '%s' registered\n", ipc->name);

                        /*  HELP is a reserved name. */
                        if (strcmp (ipc->name, "HELP") == 0) {
printf ("TCPMUX: service unregistered (3)\n");
                            goto ipc_end;
                        }

                        /*  The service name is complete here. Check for
                            duplicate services. */
                        for (it = nn_list_begin (&ipcconns);
                              it != nn_list_end (&ipcconns);
                              it = nn_list_next (&ipcconns, it)) {
                            struct ipcconn *ipc2 = nn_cont (it,
                                struct ipcconn, item);
                            if (ipc->state != IPCCONN_STATE_ACTIVE)
                                continue;
                            if (strcmp (ipc->name, ipc2->name) == 0) {
printf ("TCPMUX: service unregistered (4)\n");
                                goto ipc_end;
                            }
                        }

                        /*  The name is complete here.
                            Switch into active state. */
                        ipc->state = IPCCONN_STATE_ACTIVE;
                        break;
                    }
                }

                /*  The service name can have at most 255 characters. */
                if (ipc->name_size == 255) {
printf ("TCPMUX: service unregistered (5)\n");
                    goto ipc_end;
                }

                /*  Add the new character to the service name. */
                ipc->name [ipc->name_size] = c;
                ++ipc->name_size;
            }
        }
    }
}

static int send_fd (int s, int fd)
{
    int rc;
    struct iovec iov;
    char c = 0;
    struct msghdr msg;
    char control [sizeof (struct cmsghdr) + 10];
    struct cmsghdr *cmsg;

    /*  Compose the message. We'll send one byte long dummy message
        accompanied with the fd.*/
    iov.iov_base = &c;
    iov.iov_len = 1;
    memset (&msg, 0, sizeof (msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof (control);

    /*  Attach the file descriptor to the message. */
    cmsg = CMSG_FIRSTHDR (&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN (sizeof (fd));
    int *data = (int*) CMSG_DATA (cmsg);
    *data = fd;

    /*  Adjust the size of the control to match the data. */
    msg.msg_controllen = cmsg->cmsg_len;

    /*  Pass the file descriptor to the registered process. */
    rc = sendmsg (s, &msg, 0);
    if (rc < 0)
        return -1;
    nn_assert (rc == 1);
    return 0;
}

