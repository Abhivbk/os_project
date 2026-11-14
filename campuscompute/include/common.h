#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

/* Exit codes */
#define EXIT_OK      0
#define EXIT_USAGE   1
#define EXIT_RUNTIME 2

/* Scheduler constants */
#define MAX_JOBS     50
#define MAX_SEGMENTS 1024

/* Logger constants */
#define LOG_BUF_CAP      4
#define NUM_PRODUCERS    3
#define EVENTS_PER_PROD  5
#define NUM_CONSUMERS    2
#define LOG_FILE_PATH    "logs/campus.log"

/* Banker constants */
#define MAX_PROCS 10
#define MAX_RES   10

typedef enum {
    JOB_NEW = 0,
    JOB_READY,
    JOB_RUNNING,
    JOB_DONE
} job_status_t;

typedef struct {
    int id;
    int arrival;
    int burst;
    int priority;
    int remaining;
    int start;
    int completion;
    int tat;
    int wt;
    job_status_t status;
} job_t;

typedef struct {
    double avg_tat;
    double avg_wt;
} sched_metrics_t;

/* CLI and helper functions in main.c */
void print_usage(FILE *out);
bool parse_strict_int(const char *str, int *out_val);
int count_campuscompute_ipc(bool remove_them, bool verbose);

/* Module entry points */
int run_sched(int argc, char **argv);
int run_sched_compare_file(const char *path, int quantum);
int run_logger(void);
int run_ipc(void);
int run_banker(const char *path, int *out_is_safe);
int run_worker(bool fail_mode);
int run_limits(void);

#endif /* COMMON_H */
