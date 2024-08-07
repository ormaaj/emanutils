#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>

static void
usage(void)
{
    static char const msg[] = "Usage: openpidfd fd pid cmd [args]...\n";
    if (fputs(msg, stderr) == EOF)
        perror("fputs");
}

int
main(int const argc, char *const *const argv)
{
    for (int opt; opt = getopt(argc, argv, "+"), opt != -1;) {
        switch (opt) {
        default:
            usage();
            return 2;
        }
    }

    if (argc - optind < 3) {
        usage();
        return 2;
    }

    int fd, pid;
    for (int i = 0; i < 2; ++i) {
        char *const str = argv[optind + i];
        char *endptr;
        errno = 0;
        long const num = strtol(str, &endptr, 10);
        if (errno) {
            perror("strtol");
            return 2;
        }
        if (endptr == str || num < 0 || num > INT_MAX || *endptr) {
            if (fputs("Invalid argument.\n", stderr) == EOF)
                perror("fputs");
            return 2;
        }
        *(int *const[]){ &fd, &pid }[i] = (int)num;
    }

    int const pidfd = syscall(SYS_pidfd_open, (pid_t)pid, 0);
    if (pidfd == -1) {
        perror("pidfd_open");
        return 2;
    }

    if (pidfd != fd) {
        int ret;
        do {
            ret = dup2(pidfd, fd);
        } while (ret == -1 && errno == EINTR);
        if (ret == -1) {
            perror("dup2");
            return 2;
        }
    } else {
        int const flags = fcntl(fd, F_GETFD);
        if (flags == -1) {
            perror("fcntl(F_GETFD)");
            return 2;
        }
        if ((flags & FD_CLOEXEC) &&
            fcntl(fd, F_SETFD, flags & ~FD_CLOEXEC) == -1) {
            perror("fcntl(F_SETFD)");
            return 2;
        }
    }

    (void)execvp(argv[optind + 2], &argv[optind + 2]);
    perror("execvp");
    return 2;
}
