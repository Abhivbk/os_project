#include "common.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <semaphore.h>
#include <signal.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define IPC_MSG_COUNT 5

typedef struct {
    int seq;
    int done;
    char msg[128];
} shm_packet_t;

static char g_shm_name[128];
static char g_semdata_name[128];
static char g_semslot_name[128];
static pid_t g_owner_pid = 0;

static void sigint_cleanup_handler(int sig) {
    (void)sig;
    if (g_owner_pid != 0 && getpid() == g_owner_pid) {
        if (g_shm_name[0] != '\0') {
            shm_unlink(g_shm_name);
        }
        if (g_semdata_name[0] != '\0') {
            sem_unlink(g_semdata_name);
        }
        if (g_semslot_name[0] != '\0') {
            sem_unlink(g_semslot_name);
        }
    }
    _exit(130);
}

static int sem_wait_nointr(sem_t *sem) {
    while (sem_wait(sem) == -1) {
        if (errno == EINTR) {
            continue;
        }
        return -1;
    }
    return 0;
}

static void list_dev_shm_entries(const char *label) {
    printf("/dev/shm campuscompute entries (%s):", label);
    DIR *dir = opendir("/dev/shm");
    if (!dir) {
        printf(" [error opening /dev/shm: %s]\n", strerror(errno));
        return;
    }
    int found = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "campuscompute_", 14) == 0 ||
            strncmp(ent->d_name, "sem.campuscompute_", 18) == 0) {
            printf(" %s", ent->d_name);
            found++;
        }
    }
    closedir(dir);
    if (found == 0) {
        printf(" (none)");
    }
    printf("\n");
}

int run_ipc(void) {
    pid_t pid = getpid();
    g_owner_pid = pid;
    snprintf(g_shm_name, sizeof(g_shm_name), "/campuscompute_shm_%d", (int)pid);
    snprintf(g_semdata_name, sizeof(g_semdata_name), "/campuscompute_semdata_%d", (int)pid);
    snprintf(g_semslot_name, sizeof(g_semslot_name), "/campuscompute_semslot_%d", (int)pid);

    struct sigaction sa_new, sa_old;
    memset(&sa_new, 0, sizeof(sa_new));
    sa_new.sa_handler = sigint_cleanup_handler;
    sigemptyset(&sa_new.sa_mask);
    sigaction(SIGINT, &sa_new, &sa_old);

    int shm_fd = -1;
    shm_packet_t *shared = MAP_FAILED;
    sem_t *sem_slot = SEM_FAILED;
    sem_t *sem_data = SEM_FAILED;
    int ret = EXIT_RUNTIME;

    shm_fd = shm_open(g_shm_name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (shm_fd == -1) {
        fprintf(stderr, "Error: shm_open(%s) failed: %s\n", g_shm_name, strerror(errno));
        goto cleanup;
    }

    if (ftruncate(shm_fd, (off_t)sizeof(shm_packet_t)) == -1) {
        fprintf(stderr, "Error: ftruncate failed: %s\n", strerror(errno));
        goto cleanup;
    }

    shared = mmap(NULL, sizeof(shm_packet_t), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shared == MAP_FAILED) {
        fprintf(stderr, "Error: mmap failed: %s\n", strerror(errno));
        goto cleanup;
    }
    memset(shared, 0, sizeof(shm_packet_t));

    sem_slot = sem_open(g_semslot_name, O_CREAT | O_EXCL, 0600, 1);
    if (sem_slot == SEM_FAILED) {
        fprintf(stderr, "Error: sem_open(%s) failed: %s\n", g_semslot_name, strerror(errno));
        goto cleanup;
    }

    sem_data = sem_open(g_semdata_name, O_CREAT | O_EXCL, 0600, 0);
    if (sem_data == SEM_FAILED) {
        fprintf(stderr, "Error: sem_open(%s) failed: %s\n", g_semdata_name, strerror(errno));
        goto cleanup;
    }

    list_dev_shm_entries("active");

    /* Flush before fork so the child does not re-emit the parent's buffered output */
    fflush(stdout);
    pid_t child = fork();
    if (child < 0) {
        fprintf(stderr, "Error: fork() failed: %s\n", strerror(errno));
        goto cleanup;
    }

    if (child == 0) {
        /* Child consumer */
        int msgs_read = 0;
        for (;;) {
            if (sem_wait_nointr(sem_data) == -1) {
                fprintf(stderr, "Error: child sem_wait: %s\n", strerror(errno));
                _exit(2);
            }
            int seq = shared->seq;
            int done = shared->done;
            char buf[128];
            snprintf(buf, sizeof(buf), "%s", shared->msg);

            printf("[CHILD pid=%d] read #%d \"%s\"\n", (int)getpid(), seq, buf);
            fflush(stdout);
            msgs_read++;

            if (sem_post(sem_slot) == -1) {
                fprintf(stderr, "Error: child sem_post: %s\n", strerror(errno));
                _exit(2);
            }
            if (done) {
                break;
            }
        }
        munmap(shared, sizeof(shm_packet_t));
        close(shm_fd);
        sem_close(sem_data);
        sem_close(sem_slot);
        _exit((msgs_read == IPC_MSG_COUNT) ? 0 : 2);
    }

    /* Parent writer */
    for (int i = 1; i <= IPC_MSG_COUNT; i++) {
        if (sem_wait_nointr(sem_slot) == -1) {
            fprintf(stderr, "Error: parent sem_wait: %s\n", strerror(errno));
            goto cleanup;
        }
        shared->seq = i;
        shared->done = (i == IPC_MSG_COUNT) ? 1 : 0;
        snprintf(shared->msg, sizeof(shared->msg), "CampusCompute dispatch batch #%d", i);

        printf("[PARENT pid=%d] wrote #%d \"%s\"\n", (int)pid, i, shared->msg);
        fflush(stdout);

        if (sem_post(sem_data) == -1) {
            fprintf(stderr, "Error: parent sem_post: %s\n", strerror(errno));
            goto cleanup;
        }
    }

    int status = 0;
    if (waitpid(child, &status, 0) == -1) {
        fprintf(stderr, "Error: waitpid failed: %s\n", strerror(errno));
        goto cleanup;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        ret = EXIT_OK;
    }

cleanup:
    if (shared != MAP_FAILED) {
        munmap(shared, sizeof(shm_packet_t));
    }
    if (shm_fd != -1) {
        close(shm_fd);
    }
    if (sem_data != SEM_FAILED) {
        sem_close(sem_data);
    }
    if (sem_slot != SEM_FAILED) {
        sem_close(sem_slot);
    }
    sem_unlink(g_semdata_name);
    sem_unlink(g_semslot_name);
    shm_unlink(g_shm_name);

    sigaction(SIGINT, &sa_old, NULL);

    /* Verify shm object is gone */
    int verify_fd = shm_open(g_shm_name, O_RDONLY, 0);
    if (verify_fd == -1 && errno == ENOENT) {
        printf("Cleanup verified: %s removed\n", g_shm_name);
    } else {
        if (verify_fd != -1) {
            close(verify_fd);
        }
        fprintf(stderr, "Error: cleanup verification failed for %s\n", g_shm_name);
        ret = EXIT_RUNTIME;
    }

    list_dev_shm_entries("after cleanup");
    return ret;
}
