#include "interrupt.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <poll.h>

#ifdef __linux__
#include <sys/eventfd.h>
#define HAVE_EVENTFD 1
#endif

struct Interrupt {
    int fd;              /* File descriptor for select/poll */
    int write_fd;        /* File descriptor for writing (same as fd for eventfd) */
    bool is_eventfd;     /* Whether we're using eventfd or pipe */
};

Interrupt* interrupt_create(void) {
    Interrupt* intr = calloc(1, sizeof(Interrupt));
    if (!intr) {
        perror("malloc");
        return NULL;
    }

#ifdef HAVE_EVENTFD
    /* Try eventfd first */
    intr->fd = eventfd(0, EFD_CLOEXEC);
    if (intr->fd != -1) {
        intr->write_fd = intr->fd;
        intr->is_eventfd = true;
        return intr;
    }
    /* If eventfd fails with ENOENT, fall back to pipe */
    if (errno != ENOENT) {
        perror("eventfd");
        free(intr);
        return NULL;
    }
#endif

    /* Fall back to pipe */
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        perror("pipe");
        free(intr);
        return NULL;
    }

    /* Set non-blocking mode for read end */
    int flags = fcntl(pipefd[0], F_GETFL, 0);
    if (flags == -1 || fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl");
        close(pipefd[0]);
        close(pipefd[1]);
        free(intr);
        return NULL;
    }

    intr->fd = pipefd[0];
    intr->write_fd = pipefd[1];
    intr->is_eventfd = false;
    return intr;
}

void interrupt_destroy(Interrupt* intr) {
    if (!intr) return;

    if (intr->is_eventfd) {
        close(intr->fd);
    } else {
        close(intr->fd);
        close(intr->write_fd);
    }
    free(intr);
}

int interrupt_get_fd(const Interrupt* intr) {
    return intr ? intr->fd : -1;
}

int interrupt_signal(Interrupt* intr) {
    if (!intr) return -1;

    uint64_t val = 1;
    ssize_t ret = write(intr->write_fd, &val, sizeof(val));
    if (ret != sizeof(val)) {
        perror("write");
        return -1;
    }
    return 0;
}

int interrupt_clear(Interrupt* intr) {
    if (!intr) return -1;

    uint64_t val;
    ssize_t ret = read(intr->fd, &val, sizeof(val));
    if (ret == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;  /* No data available is not an error */
        }
        perror("read");
        return -1;
    }
    return 0;
}

int interrupt_wait(Interrupt* intr) {
    if (!intr) {
        return -1;
    }

    while (1) {
        uint64_t value;
        ssize_t n = read(intr->fd, &value, sizeof(value));
        
        if (n == sizeof(value)) {
            return 0;
        }
        if (n != -1 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            perror("read");
            return -1;
        }

        // Data not available, use poll to wait
        struct pollfd pfd = {
            .fd = intr->fd,
            .events = POLLIN,
            .revents = 0
        };

        int ret = poll(&pfd, 1, -1);  // -1 means wait indefinitely
        if (ret < 0) {
            perror("poll");
            return -1;
        }

        if (ret == 0) {
            // Timeout (shouldn't happen with -1 timeout)
            return -1;
        }

        if (!(pfd.revents & POLLIN)) {
            return -1;
        }
    }
} 