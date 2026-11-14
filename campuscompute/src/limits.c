#include "common.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int demo_nofile(void) {
    printf("=== Demo A: RLIMIT_NOFILE ===\n");
    fflush(stdout);

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "Error: fork() failed: %s\n", strerror(errno));
        return EXIT_RUNTIME;
    }

    if (pid == 0) {
        struct rlimit rl_before, rl_new, rl_after;
        if (getrlimit(RLIMIT_NOFILE, &rl_before) != 0) {
            fprintf(stderr, "Error: getrlimit(RLIMIT_NOFILE): %s\n", strerror(errno));
            _exit(200);
        }
        printf("[CHILD pid=%d] RLIMIT_NOFILE before: soft=%lu hard=%lu\n",
               (int)getpid(), (unsigned long)rl_before.rlim_cur,
               (unsigned long)rl_before.rlim_max);

        rl_new.rlim_cur = 8;
        rl_new.rlim_max = 8;
        if (setrlimit(RLIMIT_NOFILE, &rl_new) != 0) {
            fprintf(stderr, "Error: setrlimit(RLIMIT_NOFILE, 8): %s\n", strerror(errno));
            _exit(200);
        }

        if (getrlimit(RLIMIT_NOFILE, &rl_after) != 0) {
            fprintf(stderr, "Error: getrlimit(RLIMIT_NOFILE): %s\n", strerror(errno));
            _exit(200);
        }
        printf("[CHILD pid=%d] RLIMIT_NOFILE after:  soft=%lu hard=%lu\n",
               (int)getpid(), (unsigned long)rl_after.rlim_cur,
               (unsigned long)rl_after.rlim_max);

        int fds[32];
        int opened = 0;
        int hit_emfile = 0;

        while (opened < 32) {
            int fd = open("/dev/null", O_RDONLY);
            if (fd == -1) {
                const char *ename = (errno == EMFILE) ? "EMFILE" : strerror(errno);
                printf("[CHILD pid=%d] open() failed after %d descriptors with errno=%s (%s)\n",
                       (int)getpid(), opened, ename, strerror(errno));
                hit_emfile = (errno == EMFILE) ? 1 : 0;
                break;
            }
            fds[opened++] = fd;
        }

        for (int i = 0; i < opened; i++) {
            close(fds[i]);
        }

        /* _exit() does not flush stdio, so flush explicitly (matters when piped) */
        fflush(stdout);
        if (!hit_emfile) {
            _exit(201);
        }
        /* Encode opened count in exit status so parent computes explanation dynamically */
        _exit(opened);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) == -1) {
        fprintf(stderr, "Error: waitpid failed: %s\n", strerror(errno));
        return EXIT_RUNTIME;
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) >= 200) {
        fprintf(stderr, "Error: RLIMIT_NOFILE child failed unexpectedly\n");
        return EXIT_RUNTIME;
    }

    int n_opened = WEXITSTATUS(status);
    int limit_val = 8;
    int pre_used = limit_val - n_opened;
    printf("Observed: open() failed with EMFILE after %d descriptors because the limit is %d (%d already used by stdin/stdout/stderr).\n",
           n_opened, limit_val, pre_used);

    return EXIT_OK;
}

static int demo_cpu(void) {
    printf("\n=== Demo B: RLIMIT_CPU ===\n");
    fflush(stdout);

    struct timespec t_start, t_end;
    if (clock_gettime(CLOCK_MONOTONIC, &t_start) != 0) {
        fprintf(stderr, "Error: clock_gettime failed: %s\n", strerror(errno));
        return EXIT_RUNTIME;
    }

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "Error: fork() failed: %s\n", strerror(errno));
        return EXIT_RUNTIME;
    }

    if (pid == 0) {
        struct rlimit core_rl = {0, 0};
        setrlimit(RLIMIT_CORE, &core_rl);

        struct rlimit rl_before, rl_new, rl_after;
        if (getrlimit(RLIMIT_CPU, &rl_before) != 0) {
            fprintf(stderr, "Error: getrlimit(RLIMIT_CPU): %s\n", strerror(errno));
            _exit(2);
        }
        printf("[CHILD pid=%d] RLIMIT_CPU before: soft=%ld hard=%ld (-1 = unlimited)\n",
               (int)getpid(), (long)rl_before.rlim_cur, (long)rl_before.rlim_max);

        rl_new.rlim_cur = 1;
        rl_new.rlim_max = 2;
        if (setrlimit(RLIMIT_CPU, &rl_new) != 0) {
            fprintf(stderr, "Error: setrlimit(RLIMIT_CPU): %s\n", strerror(errno));
            _exit(2);
        }

        if (getrlimit(RLIMIT_CPU, &rl_after) != 0) {
            fprintf(stderr, "Error: getrlimit(RLIMIT_CPU): %s\n", strerror(errno));
            _exit(2);
        }
        printf("[CHILD pid=%d] RLIMIT_CPU after:  soft=%ld hard=%ld (busy-looping...)\n",
               (int)getpid(), (long)rl_after.rlim_cur, (long)rl_after.rlim_max);
        fflush(stdout);

        volatile unsigned long counter = 0;
        for (;;) {
            counter++;
        }
        _exit(0);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) == -1) {
        fprintf(stderr, "Error: waitpid failed: %s\n", strerror(errno));
        return EXIT_RUNTIME;
    }

    if (clock_gettime(CLOCK_MONOTONIC, &t_end) != 0) {
        fprintf(stderr, "Error: clock_gettime failed: %s\n", strerror(errno));
        return EXIT_RUNTIME;
    }

    double elapsed = (double)(t_end.tv_sec - t_start.tv_sec) +
                     (double)(t_end.tv_nsec - t_start.tv_nsec) / 1e9;

    if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        const char *sname = (sig == SIGXCPU) ? "SIGXCPU" :
                            (sig == SIGKILL) ? "SIGKILL" : strsignal(sig);
        printf("[PARENT] child (pid=%d) killed by %s (signal %d), elapsed wall time: %.2fs\n",
               (int)pid, sname, sig, elapsed);
        printf("Observed: kernel sent SIGXCPU after ~1s of CPU time.\n");
        if (sig == SIGXCPU || sig == SIGKILL) {
            return EXIT_OK;
        }
    }

    fprintf(stderr, "Error: RLIMIT_CPU child did not terminate via expected signal\n");
    return EXIT_RUNTIME;
}

int run_limits(void) {
    int rc_a = demo_nofile();
    if (rc_a != EXIT_OK) {
        return rc_a;
    }
    return demo_cpu();
}
