#include "common.h"

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#define NUM_WORKERS 3

static const char *sig_abbrev(int sig) {
    switch (sig) {
        case SIGABRT: return "SIGABRT";
        case SIGSEGV: return "SIGSEGV";
        case SIGTERM: return "SIGTERM";
        case SIGKILL: return "SIGKILL";
        case SIGXCPU: return "SIGXCPU";
        default:      return strsignal(sig);
    }
}

int run_worker(bool fail_mode) {
    printf("=== Worker Demo (%s mode) ===\n", fail_mode ? "fail" : "normal");

    pid_t pids[NUM_WORKERS];
    for (int i = 0; i < NUM_WORKERS; i++) {
        int worker_idx = i + 1;
        fflush(stdout);
        pid_t pid = fork();
        if (pid < 0) {
            fprintf(stderr, "Error: fork() failed: %s\n", strerror(errno));
            return EXIT_RUNTIME;
        }

        if (pid == 0) {
            /* Child worker process */
            printf("[WORKER %d pid=%d] doing work\n", worker_idx, (int)getpid());
            fflush(stdout);
            usleep((useconds_t)(worker_idx * 40000));

            if (fail_mode && worker_idx == 2) {
                printf("[SIMULATED FAILURE] worker 2 exiting with status 3\n");
                fflush(stdout);
                _exit(3);
            }
            if (fail_mode && worker_idx == 3) {
                struct rlimit rl = {0, 0};
                if (setrlimit(RLIMIT_CORE, &rl) != 0) {
                    fprintf(stderr, "Error: setrlimit(RLIMIT_CORE): %s\n", strerror(errno));
                }
                signal(SIGABRT, SIG_DFL);
                printf("[SIMULATED FAILURE] worker 3 raising SIGABRT\n");
                fflush(stdout);
                raise(SIGABRT);
                _exit(1);
            }
            exit(0);
        }

        pids[i] = pid;
    }

    int normal_count = 0;
    int failed_count = 0;
    int status = 0;
    pid_t wpid;

    while ((wpid = waitpid(-1, &status, 0)) > 0) {
        int worker_idx = -1;
        for (int i = 0; i < NUM_WORKERS; i++) {
            if (pids[i] == wpid) {
                worker_idx = i + 1;
                break;
            }
        }

        if (WIFEXITED(status)) {
            int code = WEXITSTATUS(status);
            if (code == 0) {
                normal_count++;
                printf("[PARENT] worker %d (pid=%d) exited with status %d\n",
                       worker_idx, (int)wpid, code);
            } else {
                failed_count++;
                const char *tag = (fail_mode && worker_idx == 2 && code == 3)
                                  ? " (simulated)" : "";
                printf("[PARENT] worker %d (pid=%d) exited with status %d%s\n",
                       worker_idx, (int)wpid, code, tag);
            }
        } else if (WIFSIGNALED(status)) {
            int sig = WTERMSIG(status);
            failed_count++;
            const char *tag = (fail_mode && worker_idx == 3 && sig == SIGABRT)
                              ? " (simulated)" : "";
            printf("[PARENT] worker %d (pid=%d) killed by signal %d (%s)%s\n",
                   worker_idx, (int)wpid, sig, sig_abbrev(sig), tag);
        }
    }

    /* Confirm waitpid returns ECHILD so 0 zombies remain */
    int zombies_left = (wpid == -1 && errno == ECHILD) ? 0 : 1;

    printf("Workers: %d  Normal: %d  Failed: %d  Zombies left: %d\n",
           NUM_WORKERS, normal_count, failed_count, zombies_left);

    if (zombies_left != 0) {
        return EXIT_RUNTIME;
    }
    if (!fail_mode && (normal_count != 3 || failed_count != 0)) {
        return EXIT_RUNTIME;
    }
    if (fail_mode && (normal_count != 1 || failed_count != 2)) {
        return EXIT_RUNTIME;
    }

    return EXIT_OK;
}
