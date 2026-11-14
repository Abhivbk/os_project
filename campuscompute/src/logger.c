#include "common.h"

#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    int seq;
    int producer_id;
    char text[128];
} log_entry_t;

typedef struct {
    log_entry_t buf[LOG_BUF_CAP];
    int head;
    int tail;
    int count;
    int next_seq;
    int produced_total;
    int consumed_total;
    int done;
    pthread_mutex_t mtx;
    pthread_cond_t not_full;
    pthread_cond_t not_empty;
    pthread_mutex_t file_mtx;
    FILE *log_fp;
} logger_ctx_t;

typedef struct {
    int id;
    logger_ctx_t *ctx;
} thread_arg_t;

static void *producer_thread(void *arg) {
    thread_arg_t *targ = (thread_arg_t *)arg;
    logger_ctx_t *ctx = targ->ctx;
    unsigned int seed = (unsigned int)(targ->id * 1337u + 42u);

    for (int i = 1; i <= EVENTS_PER_PROD; i++) {
        /* Deterministic jitter so both full and empty conditions occur */
        usleep((useconds_t)((rand_r(&seed) % 8 + 1) * 1000));

        int rc = pthread_mutex_lock(&ctx->mtx);
        if (rc != 0) {
            fprintf(stderr, "Error: pthread_mutex_lock: %s\n", strerror(rc));
            return NULL;
        }

        while (ctx->count == LOG_BUF_CAP) {
            printf("[P%d] BUFFER FULL, waiting\n", targ->id);
            rc = pthread_cond_wait(&ctx->not_full, &ctx->mtx);
            if (rc != 0) {
                fprintf(stderr, "Error: pthread_cond_wait: %s\n", strerror(rc));
                pthread_mutex_unlock(&ctx->mtx);
                return NULL;
            }
        }

        int seq = ++ctx->next_seq;
        log_entry_t entry;
        entry.seq = seq;
        entry.producer_id = targ->id;
        snprintf(entry.text, sizeof(entry.text),
                 "producer %d: job %d submitted (seq #%d)", targ->id, i, seq);

        ctx->buf[ctx->tail] = entry;
        ctx->tail = (ctx->tail + 1) % LOG_BUF_CAP;
        ctx->count++;
        ctx->produced_total++;

        printf("[P%d] enqueue #%d (count=%d)\n", targ->id, seq, ctx->count);

        pthread_cond_signal(&ctx->not_empty);
        pthread_mutex_unlock(&ctx->mtx);
    }
    return NULL;
}

static void *consumer_thread(void *arg) {
    thread_arg_t *targ = (thread_arg_t *)arg;
    logger_ctx_t *ctx = targ->ctx;
    unsigned int seed = (unsigned int)(targ->id * 9001u + 7u);

    for (;;) {
        usleep((useconds_t)((rand_r(&seed) % 12 + 3) * 1000));

        int rc = pthread_mutex_lock(&ctx->mtx);
        if (rc != 0) {
            fprintf(stderr, "Error: pthread_mutex_lock: %s\n", strerror(rc));
            return NULL;
        }

        while (ctx->count == 0 && !ctx->done) {
            printf("[C%d] BUFFER EMPTY, waiting\n", targ->id);
            rc = pthread_cond_wait(&ctx->not_empty, &ctx->mtx);
            if (rc != 0) {
                fprintf(stderr, "Error: pthread_cond_wait: %s\n", strerror(rc));
                pthread_mutex_unlock(&ctx->mtx);
                return NULL;
            }
        }

        if (ctx->count == 0 && ctx->done) {
            pthread_mutex_unlock(&ctx->mtx);
            break;
        }

        log_entry_t item = ctx->buf[ctx->head];
        ctx->head = (ctx->head + 1) % LOG_BUF_CAP;
        ctx->count--;
        ctx->consumed_total++;

        printf("[C%d] dequeue #%d (count=%d)\n", targ->id, item.seq, ctx->count);

        pthread_cond_signal(&ctx->not_full);
        pthread_mutex_unlock(&ctx->mtx);

        /* Write timestamped log entry protected by dedicated file_mtx */
        time_t now = time(NULL);
        struct tm tm_now;
        localtime_r(&now, &tm_now);
        char ts[32];
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_now);

        pthread_mutex_lock(&ctx->file_mtx);
        fprintf(ctx->log_fp, "%s [C%d] %s\n", ts, targ->id, item.text);
        fflush(ctx->log_fp);
        pthread_mutex_unlock(&ctx->file_mtx);
    }
    return NULL;
}

static int count_file_lines(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return -1;
    }
    int lines = 0;
    int ch;
    while ((ch = fgetc(fp)) != EOF) {
        if (ch == '\n') {
            lines++;
        }
    }
    fclose(fp);
    return lines;
}

int run_logger(void) {
    if (mkdir("logs", 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "Error: mkdir(logs) failed: %s\n", strerror(errno));
        return EXIT_RUNTIME;
    }

    logger_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    ctx.log_fp = fopen(LOG_FILE_PATH, "w");
    if (!ctx.log_fp) {
        fprintf(stderr, "Error: fopen(%s) failed: %s\n", LOG_FILE_PATH, strerror(errno));
        return EXIT_RUNTIME;
    }

    if (pthread_mutex_init(&ctx.mtx, NULL) != 0 ||
        pthread_mutex_init(&ctx.file_mtx, NULL) != 0 ||
        pthread_cond_init(&ctx.not_full, NULL) != 0 ||
        pthread_cond_init(&ctx.not_empty, NULL) != 0) {
        fprintf(stderr, "Error: pthread sync init failed\n");
        fclose(ctx.log_fp);
        return EXIT_RUNTIME;
    }

    printf("=== Producer-Consumer Logger (buffer capacity=%d, file protected by file_mtx) ===\n",
           LOG_BUF_CAP);

    pthread_t prod_tids[NUM_PRODUCERS];
    pthread_t cons_tids[NUM_CONSUMERS];
    thread_arg_t prod_args[NUM_PRODUCERS];
    thread_arg_t cons_args[NUM_CONSUMERS];

    for (int i = 0; i < NUM_CONSUMERS; i++) {
        cons_args[i].id = i + 1;
        cons_args[i].ctx = &ctx;
        int rc = pthread_create(&cons_tids[i], NULL, consumer_thread, &cons_args[i]);
        if (rc != 0) {
            fprintf(stderr, "Error: pthread_create consumer: %s\n", strerror(rc));
            fclose(ctx.log_fp);
            return EXIT_RUNTIME;
        }
    }

    for (int i = 0; i < NUM_PRODUCERS; i++) {
        prod_args[i].id = i + 1;
        prod_args[i].ctx = &ctx;
        int rc = pthread_create(&prod_tids[i], NULL, producer_thread, &prod_args[i]);
        if (rc != 0) {
            fprintf(stderr, "Error: pthread_create producer: %s\n", strerror(rc));
            fclose(ctx.log_fp);
            return EXIT_RUNTIME;
        }
    }

    int joined = 0;
    for (int i = 0; i < NUM_PRODUCERS; i++) {
        if (pthread_join(prod_tids[i], NULL) == 0) {
            joined++;
        }
    }

    pthread_mutex_lock(&ctx.mtx);
    ctx.done = 1;
    pthread_cond_broadcast(&ctx.not_empty);
    pthread_mutex_unlock(&ctx.mtx);

    for (int i = 0; i < NUM_CONSUMERS; i++) {
        if (pthread_join(cons_tids[i], NULL) == 0) {
            joined++;
        }
    }

    pthread_cond_destroy(&ctx.not_empty);
    pthread_cond_destroy(&ctx.not_full);
    pthread_mutex_destroy(&ctx.file_mtx);
    pthread_mutex_destroy(&ctx.mtx);

    if (fclose(ctx.log_fp) != 0) {
        fprintf(stderr, "Error: fclose(%s) failed: %s\n", LOG_FILE_PATH, strerror(errno));
        return EXIT_RUNTIME;
    }

    int log_lines = count_file_lines(LOG_FILE_PATH);
    printf("Produced: %d  Consumed: %d  Threads joined: %d  Log lines: %d\n",
           ctx.produced_total, ctx.consumed_total, joined, log_lines);

    int expected = NUM_PRODUCERS * EVENTS_PER_PROD;
    if (ctx.produced_total != expected ||
        ctx.consumed_total != expected ||
        log_lines != expected ||
        joined != (NUM_PRODUCERS + NUM_CONSUMERS)) {
        fprintf(stderr, "Error: logger self-check failed!\n");
        return EXIT_RUNTIME;
    }

    return EXIT_OK;
}
