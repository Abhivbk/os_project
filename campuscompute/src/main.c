#include "common.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <semaphore.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

void print_usage(FILE *out) {
    fprintf(out,
        "Usage: campuscompute <command> [args]\n\n"
        "Commands:\n"
        "  help                                   Show this help\n"
        "  sched fcfs  <jobs_file>                FCFS scheduling\n"
        "  sched rr    <jobs_file> <quantum>      Round Robin (quantum >= 1)\n"
        "  sched prio  <jobs_file>                Non-preemptive priority\n"
        "  sched compare <jobs_file> <quantum>    All three on the same job set\n"
        "  logger                                 Producer-consumer logger demo\n"
        "  ipc                                    Shared memory + semaphore demo\n"
        "  banker <state_file>                    Banker's safety check\n"
        "  worker normal                          Fork workers, all exit normally\n"
        "  worker fail                            Fork workers, one simulated failure\n"
        "  limit                                  setrlimit demos\n"
        "  all                                    Run every demo, print summary\n"
        "  cleanup                                Remove stale campuscompute_* IPC objects\n");
}

bool parse_strict_int(const char *str, int *out_val) {
    if (!str || *str == '\0') {
        return false;
    }
    while (isspace((unsigned char)*str)) {
        str++;
    }
    if (*str == '\0') {
        return false;
    }
    errno = 0;
    char *endptr = NULL;
    long val = strtol(str, &endptr, 10);
    if (errno != 0 || endptr == str || *endptr != '\0') {
        return false;
    }
    if (val < INT_MIN || val > INT_MAX) {
        return false;
    }
    *out_val = (int)val;
    return true;
}

int count_campuscompute_ipc(bool remove_them, bool verbose) {
    DIR *dir = opendir("/dev/shm");
    if (!dir) {
        fprintf(stderr, "Error: opendir(/dev/shm) failed: %s\n", strerror(errno));
        return -1;
    }

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        const char *name = ent->d_name;
        bool is_shm = (strncmp(name, "campuscompute_", 14) == 0);
        bool is_sem = (strncmp(name, "sem.campuscompute_", 18) == 0);

        if (!is_shm && !is_sem) {
            continue;
        }
        count++;
        if (remove_them) {
            char fullpath[512];
            snprintf(fullpath, sizeof(fullpath), "/dev/shm/%s", name);
            if (unlink(fullpath) == 0) {
                if (verbose) {
                    printf("Removed /dev/shm/%s\n", name);
                }
            } else {
                fprintf(stderr, "Error: unlink(%s) failed: %s\n",
                        fullpath, strerror(errno));
            }
        }
    }

    if (closedir(dir) != 0) {
        fprintf(stderr, "Error: closedir(/dev/shm) failed: %s\n", strerror(errno));
        return -1;
    }
    if (remove_them && verbose && count == 0) {
        printf("nothing to clean\n");
    }
    return count;
}

static int run_all(void) {
    printf("========================================\n");
    printf("Stage 1: Scheduling Compare\n");
    printf("========================================\n");
    int rc_sched = run_sched_compare_file("tests/jobs.txt", 3);

    printf("\n========================================\n");
    printf("Stage 2: Producer-Consumer Logger\n");
    printf("========================================\n");
    int rc_logger = run_logger();

    printf("\n========================================\n");
    printf("Stage 3: POSIX IPC (Shared Memory + Semaphores)\n");
    printf("========================================\n");
    int rc_ipc = run_ipc();

    printf("\n========================================\n");
    printf("Stage 4: Banker's Algorithm (Safe State)\n");
    printf("========================================\n");
    int safe_flag = -1;
    int rc_bsafe = run_banker("tests/banker_safe.txt", &safe_flag);
    bool bsafe_ok = (rc_bsafe == EXIT_OK && safe_flag == 1);

    printf("\n========================================\n");
    printf("Stage 5: Banker's Algorithm (Unsafe State)\n");
    printf("========================================\n");
    int unsafe_flag = -1;
    int rc_bunsafe = run_banker("tests/banker_unsafe.txt", &unsafe_flag);
    bool bunsafe_ok = (rc_bunsafe == EXIT_OK && unsafe_flag == 0);

    printf("\n========================================\n");
    printf("Stage 6: Worker Processes (Normal)\n");
    printf("========================================\n");
    int rc_wnorm = run_worker(false);

    printf("\n========================================\n");
    printf("Stage 7: Worker Processes (Simulated Failures)\n");
    printf("========================================\n");
    int rc_wfail = run_worker(true);

    printf("\n========================================\n");
    printf("Stage 8: Resource Limits (setrlimit)\n");
    printf("========================================\n");
    int rc_limit = run_limits();

    int leftover = count_campuscompute_ipc(false, false);
    if (leftover < 0) {
        leftover = 999;
    }

    bool all_ok = (rc_sched == EXIT_OK) &&
                  (rc_logger == EXIT_OK) &&
                  (rc_ipc == EXIT_OK) &&
                  bsafe_ok &&
                  bunsafe_ok &&
                  (rc_wnorm == EXIT_OK) &&
                  (rc_wfail == EXIT_OK) &&
                  (rc_limit == EXIT_OK) &&
                  (leftover == 0);

    printf("\n=== CampusCompute Summary ===\n");
    printf("Scheduling ......... %s\n", (rc_sched == EXIT_OK) ? "PASS" : "FAIL");
    printf("Logger ............. %s\n", (rc_logger == EXIT_OK) ? "PASS" : "FAIL");
    printf("IPC ................ %s\n", (rc_ipc == EXIT_OK) ? "PASS" : "FAIL");
    printf("Banker (safe) ...... %s\n", bsafe_ok ? "PASS" : "FAIL");
    printf("Banker (unsafe) .... %s\n",
           bunsafe_ok ? "PASS  (unsafe state correctly detected)" : "FAIL");
    printf("Worker normal ...... %s\n", (rc_wnorm == EXIT_OK) ? "PASS" : "FAIL");
    printf("Worker failure ..... %s\n",
           (rc_wfail == EXIT_OK) ? "PASS  (simulated failures detected)" : "FAIL");
    printf("Resource limits .... %s\n", (rc_limit == EXIT_OK) ? "PASS" : "FAIL");
    printf("Leftover IPC objects: %d\n", leftover);
    printf("Overall: %s\n", all_ok ? "ALL PASS" : "FAILED");

    return all_ok ? EXIT_OK : EXIT_RUNTIME;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Error: missing command\n\n");
        print_usage(stderr);
        return EXIT_USAGE;
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "help") == 0) {
        print_usage(stdout);
        return EXIT_OK;
    }
    if (strcmp(cmd, "sched") == 0) {
        return run_sched(argc - 2, argv + 2);
    }
    if (strcmp(cmd, "logger") == 0) {
        if (argc != 2) {
            fprintf(stderr, "Error: 'logger' takes no arguments\n\n");
            print_usage(stderr);
            return EXIT_USAGE;
        }
        return run_logger();
    }
    if (strcmp(cmd, "ipc") == 0) {
        if (argc != 2) {
            fprintf(stderr, "Error: 'ipc' takes no arguments\n\n");
            print_usage(stderr);
            return EXIT_USAGE;
        }
        return run_ipc();
    }
    if (strcmp(cmd, "banker") == 0) {
        if (argc != 3) {
            fprintf(stderr, "Error: 'banker' requires <state_file>\n\n");
            print_usage(stderr);
            return EXIT_USAGE;
        }
        return run_banker(argv[2], NULL);
    }
    if (strcmp(cmd, "worker") == 0) {
        if (argc != 3) {
            fprintf(stderr, "Error: 'worker' requires 'normal' or 'fail'\n\n");
            print_usage(stderr);
            return EXIT_USAGE;
        }
        if (strcmp(argv[2], "normal") == 0) {
            return run_worker(false);
        }
        if (strcmp(argv[2], "fail") == 0) {
            return run_worker(true);
        }
        fprintf(stderr, "Error: unknown worker mode '%s'\n\n", argv[2]);
        print_usage(stderr);
        return EXIT_USAGE;
    }
    if (strcmp(cmd, "limit") == 0) {
        if (argc != 2) {
            fprintf(stderr, "Error: 'limit' takes no arguments\n\n");
            print_usage(stderr);
            return EXIT_USAGE;
        }
        return run_limits();
    }
    if (strcmp(cmd, "all") == 0) {
        if (argc != 2) {
            fprintf(stderr, "Error: 'all' takes no arguments\n\n");
            print_usage(stderr);
            return EXIT_USAGE;
        }
        return run_all();
    }
    if (strcmp(cmd, "cleanup") == 0) {
        if (argc != 2) {
            fprintf(stderr, "Error: 'cleanup' takes no arguments\n\n");
            print_usage(stderr);
            return EXIT_USAGE;
        }
        int r = count_campuscompute_ipc(true, true);
        return (r < 0) ? EXIT_RUNTIME : EXIT_OK;
    }

    fprintf(stderr, "Error: unknown command '%s'\n\n", cmd);
    print_usage(stderr);
    return EXIT_USAGE;
}
