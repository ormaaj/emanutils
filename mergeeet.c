#define _GNU_SOURCE /* memrchr */
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <poll.h>
#include <unistd.h>

struct buffer {
    char *buffer;
    size_t size;
    size_t length;
};

static void
usage(void)
{
    static char const message[] =
        "Usage: mergeeet [-D] [-0|-d delimiter|-L] fds...\n";
    if (fputs(message, stderr) == EOF)
        perror("fputs");
}

static int
str2int(char const *const str)
{
    char *endptr;
    errno = 0;
    long const num = strtol(str, &endptr, 10);
    if (errno) {
        perror("strtol");
        return INT_MIN;
    }
    if (endptr == str || num < INT_MIN || num > INT_MAX || *endptr)
        return INT_MIN;
    return (int)num;
}

static ssize_t
retryeintr_read(int const fd, char *const buf, size_t const size)
{
    ssize_t const ret = read(fd, buf, size);
    if (ret == -1 && errno == EINTR)
        return retryeintr_read(fd, buf, size);
    return ret;
}

static int
retryeintr_close(int const fd)
{
    int const ret = close(fd);
    if (ret == -1 && errno == EINTR)
        return retryeintr_close(fd);
    return ret;
}

static bool
fullwrite(int const fd, char const *buf, size_t size)
{
    for (;;) {
        int const nwrite = write(fd, buf, size);
        if (nwrite == -1) {
            if (errno == EINTR)
                continue;
            perror("write");
            return false;
        }
        size -= nwrite;
        buf += nwrite;
        if (!size)
            return true;
    }
}

static bool
buffer_append(struct buffer *const b, char const *const buf,
              size_t const size)
{
    if (!b->buffer) {
        b->size = ((size - 1) | (4096 - 1)) + 1;
        b->buffer = malloc(b->size);
        if (!b->buffer) {
            perror("malloc");
            return false;
        }
        (void)memcpy(b->buffer, buf, size);
        b->length = size;
        return true;
    }
    if (b->size - b->length < size) {
        size_t const newsz = ((b->length + size - 1) | (4096 - 1)) + 1;
        char *const newbuf = realloc(b->buffer, newsz);
        if (!newbuf) {
            perror("realloc");
            return false;
        }
        b->buffer = newbuf;
        b->size = newsz;
    }
    memcpy(&b->buffer[b->length], buf, size);
    b->length += size;
    return true;
}

static void
buffer_clear(struct buffer *const b)
{
    free(b->buffer);
    b->buffer = NULL;
    b->size = b->length = 0;
}

static bool
buffer_flush(int fd, struct buffer *const b)
{
    bool const ret = fullwrite(fd, b->buffer, b->length);
    buffer_clear(b);
    return ret;
}

static int
comparpollfd(void const *const a, void const *const b)
{
    int const fda = ((struct pollfd const *)a)->fd;
    int const fdb = ((struct pollfd const *)b)->fd;
    return (fda > fdb) - (fda < fdb);
}

int
main(int const argc, char *const *const argv)
{
    struct buffer *buffers = NULL;
    char delimiter = '\n';
    bool discardpartial = false;
    for (int opt; opt = getopt(argc, argv, "+0d:DL"), opt != -1;) {
        switch (opt) {
        case '0':
            buffers = (void *)1;
            delimiter = '\0';
            break;
        case 'd':
            if (!optarg[0] || !optarg[1]) {
                buffers = (void *)1;
                delimiter = *optarg;
                break;
            }
            if (fputs("Invalid delimiter.\n", stderr) == EOF)
                perror("fputs");
            return 2;
        case 'D':
            discardpartial = true;
            break;
        case 'L':
            buffers = (void *)1;
            delimiter = '\n';
            break;
        default:
            return 2;
        }
    }

    if (argc - optind < 1) {
        usage();
        return 2;
    }

    int exitstatus = 0;
    nfds_t nfds = argc - optind;
    struct pollfd *fds = calloc(nfds, sizeof (struct pollfd));
    if (!fds) {
        perror("calloc");
        return 2;
    }
    if (buffers) {
        buffers = calloc(nfds, sizeof (struct buffer));
        if (!buffers) {
            perror("calloc");
            exitstatus = 2;
            goto done;
        }
    }

    for (nfds_t i = 0; i < nfds; ++i) {
        int const fd = str2int(argv[optind + i]);
        if (fd && fd < 2) {
            if (fputs("Invalid file descriptor.\n", stderr) == EOF)
                perror("fputs");
            exitstatus = 2;
            goto done;
        }
        fds[i].events = POLLIN;
        fds[i].fd = fd;
    }
    qsort(fds, nfds, sizeof *fds, comparpollfd);
    for (nfds_t i = 1; i < nfds; ++i) {
        if (fds[i].fd == fds[i - 1].fd) {
            static char const efmt[] = "Duplicate `%d' not allowed.\n";
            if (fprintf(stderr, efmt, fds[i].fd) == EOF)
                perror("fprintf");
            exitstatus = 2;
            goto done;
        }
    }

    for (nfds_t readablefds = nfds;;) {
        int ret = poll(fds, nfds, -1);
        if (ret < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            perror("poll");
            exitstatus = 2;
            goto done;
        }
        for (nfds_t i = 0; ret && i < nfds; ++i) {
            if (!fds[i].revents)
                continue;
            --ret;
            if (fds[i].revents & POLLIN) {
                char buffer[PIPE_BUF];
                char *buf = buffer;
                int const fd = fds[i].fd;
                ssize_t nread = retryeintr_read(fd, buf, sizeof buffer);
                if (nread == 0)
                    goto pollhup;
                if (nread < 0) {
                    static char const ef[] =
                        "retryeintr_read: fd `%d': %s\n";
                    if (fprintf(stderr, ef, fd, strerror(errno)) == EOF)
                        perror("fprintf");
                    goto ioerror;
                }
                if (!buffers) {
                    if (!fullwrite(STDOUT_FILENO, buf, nread)) {
                        exitstatus = 2;
                        goto done;
                    }
                    continue;
                } else {
                    char *const del = memrchr(buf, delimiter, nread);
                    size_t len = del - buf + 1;
                    if (del) {
                        if (!buffer_flush(STDOUT_FILENO, &buffers[i]) ||
                            !fullwrite(STDOUT_FILENO, buf, len)) {
                            exitstatus = 2;
                            goto done;
                        }
                        if (len == (size_t)nread)
                            continue;
                        buf = &del[1];
                        nread -= len;
                    }
                    if (!buffer_append(&buffers[i], buf, nread)) {
                        exitstatus = 2;
                        goto done;
                    }
                    continue;
                }
            } else if (fds[i].revents & POLLHUP) {
pollhup:
                if (buffers && buffers[i].buffer) {
                    if (discardpartial) {
                        buffer_clear(&buffers[i]);
                    } else {
                        if (!buffer_flush(STDOUT_FILENO, &buffers[i]) ||
                            !fullwrite(STDOUT_FILENO, &delimiter, 1)) {
                            exitstatus = 2;
                            goto done;
                        }
                    }
                }
                if (retryeintr_close(fds[i].fd)) {
                    perror("retryeintr_close");
                    exitstatus = 2;
                }
                fds[i].fd = -1;
                if (!--readablefds)
                    goto done;
            } else if (fds[i].revents & (POLLNVAL | POLLERR)) {
                char const *const efmt = (fds[i].revents & POLLERR)
                    ? "mergeet: fd `%d': I/O error.\n"
                    : "mergeet: fd `%d': not pollable.\n";
                if (fprintf(stderr, efmt, fds[i].fd) == EOF)
                    perror("fprintf");
ioerror:
                exitstatus = 2;
                if (buffers)
                    buffer_clear(&buffers[i]);
                fds[i].fd = -1;
                if (!--readablefds)
                    goto done;
            }
        }
    }

done:
    free(fds);
    if (buffers) {
        for (nfds_t i = 0; i < nfds; ++i)
            free(buffers[i].buffer);
        free(buffers);
    }
    return exitstatus;
}
