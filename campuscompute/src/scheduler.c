#include "common.h"

#include <ctype.h>
#include <errno.h>
#include <string.h>

typedef struct {
    int job_id; /* 0 means IDLE */
    int start;
    int end;
} gantt_seg_t;

static const char *status_str(job_status_t st) {
    switch (st) {
        case JOB_NEW:     return "NEW";
        case JOB_READY:   return "READY";
        case JOB_RUNNING: return "RUNNING";
        case JOB_DONE:    return "DONE";
        default:          return "UNKNOWN";
    }
}

/* Tie-break comparator: earlier arrival first, then lower job ID */
static int cmp_arrival_id(const job_t *a, const job_t *b) {
    if (a->arrival != b->arrival) {
        return (a->arrival < b->arrival) ? -1 : 1;
    }
    return (a->id < b->id) ? -1 : (a->id > b->id);
}

static int qsort_id_asc(const void *pa, const void *pb) {
    const job_t *a = (const job_t *)pa;
    const job_t *b = (const job_t *)pb;
    return (a->id < b->id) ? -1 : (a->id > b->id);
}

/* Parse and validate jobs file. Rejects entire file with line number on failure. */
static int load_jobs(const char *path, job_t *jobs, int *out_count) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "Error: cannot open '%s': %s\n", path, strerror(errno));
        return EXIT_USAGE;
    }

    char line[512];
    int line_num = 0;
    int count = 0;

    while (fgets(line, sizeof(line), fp) != NULL) {
        line_num++;
        char *p = line;
        while (isspace((unsigned char)*p)) {
            p++;
        }
        if (*p == '\0' || *p == '#') {
            continue;
        }

        /* Strip inline comments if any or validate strictly 4 tokens */
        char *toks[5];
        int ntok = 0;
        char *saveptr = NULL;
        for (char *tok = strtok_r(p, " \t\r\n", &saveptr);
             tok != NULL && ntok < 5;
             tok = strtok_r(NULL, " \t\r\n", &saveptr)) {
            toks[ntok++] = tok;
        }

        if (ntok != 4) {
            fprintf(stderr, "Error: %s:%d: expected exactly 4 integers per line\n",
                    path, line_num);
            fclose(fp);
            return EXIT_USAGE;
        }

        int id, at, bt, pr;
        if (!parse_strict_int(toks[0], &id) ||
            !parse_strict_int(toks[1], &at) ||
            !parse_strict_int(toks[2], &bt) ||
            !parse_strict_int(toks[3], &pr)) {
            fprintf(stderr, "Error: %s:%d: invalid integer token\n", path, line_num);
            fclose(fp);
            return EXIT_USAGE;
        }

        if (id <= 0 || at < 0 || bt < 1 || pr < 0) {
            fprintf(stderr,
                    "Error: %s:%d: out of range (require id>0, arrival>=0, burst>=1, priority>=0)\n",
                    path, line_num);
            fclose(fp);
            return EXIT_USAGE;
        }

        for (int i = 0; i < count; i++) {
            if (jobs[i].id == id) {
                fprintf(stderr, "Error: %s:%d: duplicate job id %d\n",
                        path, line_num, id);
                fclose(fp);
                return EXIT_USAGE;
            }
        }

        if (count >= MAX_JOBS) {
            fprintf(stderr, "Error: %s:%d: too many jobs (max %d)\n",
                    path, line_num, MAX_JOBS);
            fclose(fp);
            return EXIT_USAGE;
        }

        jobs[count].id = id;
        jobs[count].arrival = at;
        jobs[count].burst = bt;
        jobs[count].priority = pr;
        jobs[count].remaining = bt;
        jobs[count].start = -1;
        jobs[count].completion = 0;
        jobs[count].tat = 0;
        jobs[count].wt = 0;
        jobs[count].status = JOB_NEW;
        count++;
    }

    if (fclose(fp) != 0) {
        fprintf(stderr, "Error: fclose failed: %s\n", strerror(errno));
        return EXIT_RUNTIME;
    }

    if (count < 1) {
        fprintf(stderr, "Error: %s: no valid jobs found (require 1 to %d)\n",
                path, MAX_JOBS);
        return EXIT_USAGE;
    }

    *out_count = count;
    return EXIT_OK;
}

static void compute_metrics(job_t *jobs, int n, sched_metrics_t *out) {
    double sum_tat = 0.0;
    double sum_wt = 0.0;
    for (int i = 0; i < n; i++) {
        jobs[i].tat = jobs[i].completion - jobs[i].arrival;
        jobs[i].wt = jobs[i].tat - jobs[i].burst;
        sum_tat += jobs[i].tat;
        sum_wt += jobs[i].wt;
    }
    out->avg_tat = sum_tat / (double)n;
    out->avg_wt = sum_wt / (double)n;
}

static void print_schedule_report(const char *title,
                                  const char *extra_header,
                                  const gantt_seg_t *segs,
                                  int nsegs,
                                  const job_t *jobs_in,
                                  int n,
                                  sched_metrics_t metrics) {
    printf("=== %s ===\n", title);
    printf("Tie-break: earlier arrival, then lower job ID\n");
    printf("Convention: lower priority number = higher priority; TAT = CT - AT, WT = TAT - BT\n");
    if (extra_header) {
        printf("%s\n", extra_header);
    }

    printf("Gantt: |");
    for (int i = 0; i < nsegs; i++) {
        if (segs[i].job_id == 0) {
            printf("IDLE %d-%d|", segs[i].start, segs[i].end);
        } else {
            printf("J%d %d-%d|", segs[i].job_id, segs[i].start, segs[i].end);
        }
    }
    printf("\n");

    /* Sort copy by ID ascending for clean table display */
    job_t sorted[MAX_JOBS];
    memcpy(sorted, jobs_in, (size_t)n * sizeof(job_t));
    qsort(sorted, (size_t)n, sizeof(job_t), qsort_id_asc);

    printf("ID  AT  BT  PR  ST  CT  TAT  WT  Status\n");
    for (int i = 0; i < n; i++) {
        printf("%2d  %2d  %2d  %2d  %2d  %2d   %2d  %2d  %s\n",
               sorted[i].id,
               sorted[i].arrival,
               sorted[i].burst,
               sorted[i].priority,
               sorted[i].start,
               sorted[i].completion,
               sorted[i].tat,
               sorted[i].wt,
               status_str(sorted[i].status));
    }
    printf("Average Turnaround Time: %.2f\n", metrics.avg_tat);
    printf("Average Waiting Time:    %.2f\n", metrics.avg_wt);
}

/* Pure simulation: FCFS */
static sched_metrics_t sim_fcfs(const job_t *input, int n, bool verbose) {
    job_t jobs[MAX_JOBS];
    memcpy(jobs, input, (size_t)n * sizeof(job_t));

    /* Sort by (arrival, id) */
    for (int i = 0; i < n - 1; i++) {
        for (int j = i + 1; j < n; j++) {
            if (cmp_arrival_id(&jobs[j], &jobs[i]) < 0) {
                job_t tmp = jobs[i];
                jobs[i] = jobs[j];
                jobs[j] = tmp;
            }
        }
    }

    gantt_seg_t segs[MAX_SEGMENTS];
    int nsegs = 0;
    int cur_time = 0;

    for (int i = 0; i < n; i++) {
        jobs[i].status = JOB_READY;
        if (cur_time < jobs[i].arrival) {
            segs[nsegs++] = (gantt_seg_t){0, cur_time, jobs[i].arrival};
            cur_time = jobs[i].arrival;
        }
        jobs[i].status = JOB_RUNNING;
        jobs[i].start = cur_time;
        int end_time = cur_time + jobs[i].burst;
        segs[nsegs++] = (gantt_seg_t){jobs[i].id, cur_time, end_time};
        cur_time = end_time;
        jobs[i].remaining = 0;
        jobs[i].completion = cur_time;
        jobs[i].status = JOB_DONE;
    }

    sched_metrics_t m;
    compute_metrics(jobs, n, &m);
    if (verbose) {
        print_schedule_report("FCFS Scheduling", NULL, segs, nsegs, jobs, n, m);
    }
    return m;
}

/* Pure simulation: Round Robin */
static sched_metrics_t sim_rr(const job_t *input, int n, int quantum, bool verbose) {
    job_t jobs[MAX_JOBS];
    memcpy(jobs, input, (size_t)n * sizeof(job_t));

    /* Order by (arrival, id) so newly arriving jobs are scanned in tie-break order */
    for (int i = 0; i < n - 1; i++) {
        for (int j = i + 1; j < n; j++) {
            if (cmp_arrival_id(&jobs[j], &jobs[i]) < 0) {
                job_t tmp = jobs[i];
                jobs[i] = jobs[j];
                jobs[j] = tmp;
            }
        }
    }

    int queue[MAX_SEGMENTS];
    int q_head = 0, q_tail = 0;
    bool in_queue[MAX_JOBS] = {false};

    gantt_seg_t segs[MAX_SEGMENTS];
    int nsegs = 0;
    int cur_time = 0;
    int completed = 0;

    /* Enqueue any jobs arriving at t = 0 */
    for (int i = 0; i < n; i++) {
        if (jobs[i].arrival <= cur_time) {
            jobs[i].status = JOB_READY;
            queue[q_tail++] = i;
            in_queue[i] = true;
        }
    }

    while (completed < n) {
        if (q_head == q_tail) {
            /* Queue is empty; advance clock to earliest unqueued job arrival */
            int next_idx = -1;
            for (int i = 0; i < n; i++) {
                if (jobs[i].status != JOB_DONE && !in_queue[i]) {
                    if (next_idx == -1 || jobs[i].arrival < jobs[next_idx].arrival) {
                        next_idx = i;
                    }
                }
            }
            if (next_idx != -1 && cur_time < jobs[next_idx].arrival) {
                segs[nsegs++] = (gantt_seg_t){0, cur_time, jobs[next_idx].arrival};
                cur_time = jobs[next_idx].arrival;
            }
            for (int i = 0; i < n; i++) {
                if (jobs[i].status != JOB_DONE && !in_queue[i] &&
                    jobs[i].arrival <= cur_time) {
                    jobs[i].status = JOB_READY;
                    queue[q_tail++] = i;
                    in_queue[i] = true;
                }
            }
            continue;
        }

        int idx = queue[q_head++];
        jobs[idx].status = JOB_RUNNING;
        if (jobs[idx].start == -1) {
            jobs[idx].start = cur_time;
        }

        int slice = (jobs[idx].remaining < quantum) ? jobs[idx].remaining : quantum;
        int start_t = cur_time;
        cur_time += slice;
        jobs[idx].remaining -= slice;
        segs[nsegs++] = (gantt_seg_t){jobs[idx].id, start_t, cur_time};

        /* First enqueue newly arrived jobs (arrival <= cur_time) in tie-break order */
        for (int i = 0; i < n; i++) {
            if (i != idx && jobs[i].status != JOB_DONE && !in_queue[i] &&
                jobs[i].arrival <= cur_time) {
                jobs[i].status = JOB_READY;
                queue[q_tail++] = i;
                in_queue[i] = true;
            }
        }

        /* Then re-enqueue preempted job if unfinished */
        if (jobs[idx].remaining > 0) {
            jobs[idx].status = JOB_READY;
            queue[q_tail++] = idx;
        } else {
            jobs[idx].completion = cur_time;
            jobs[idx].status = JOB_DONE;
            completed++;
        }
    }

    sched_metrics_t m;
    compute_metrics(jobs, n, &m);
    if (verbose) {
        char qline[64];
        snprintf(qline, sizeof(qline), "Time quantum = %d", quantum);
        print_schedule_report("Round Robin Scheduling", qline, segs, nsegs, jobs, n, m);
    }
    return m;
}

/* Pure simulation: Non-preemptive Priority */
static sched_metrics_t sim_prio(const job_t *input, int n, bool verbose) {
    job_t jobs[MAX_JOBS];
    memcpy(jobs, input, (size_t)n * sizeof(job_t));

    gantt_seg_t segs[MAX_SEGMENTS];
    int nsegs = 0;
    int cur_time = 0;
    int completed = 0;

    while (completed < n) {
        int best = -1;
        for (int i = 0; i < n; i++) {
            if (jobs[i].status == JOB_DONE) {
                continue;
            }
            if (jobs[i].arrival <= cur_time) {
                jobs[i].status = JOB_READY;
                if (best == -1) {
                    best = i;
                } else if (jobs[i].priority < jobs[best].priority) {
                    best = i;
                } else if (jobs[i].priority == jobs[best].priority &&
                           cmp_arrival_id(&jobs[i], &jobs[best]) < 0) {
                    best = i;
                }
            }
        }

        if (best == -1) {
            /* Jump to next arrival */
            int next_arrival = -1;
            for (int i = 0; i < n; i++) {
                if (jobs[i].status != JOB_DONE) {
                    if (next_arrival == -1 || jobs[i].arrival < next_arrival) {
                        next_arrival = jobs[i].arrival;
                    }
                }
            }
            segs[nsegs++] = (gantt_seg_t){0, cur_time, next_arrival};
            cur_time = next_arrival;
            continue;
        }

        jobs[best].status = JOB_RUNNING;
        jobs[best].start = cur_time;
        int end_t = cur_time + jobs[best].burst;
        segs[nsegs++] = (gantt_seg_t){jobs[best].id, cur_time, end_t};
        cur_time = end_t;
        jobs[best].remaining = 0;
        jobs[best].completion = cur_time;
        jobs[best].status = JOB_DONE;
        completed++;
    }

    sched_metrics_t m;
    compute_metrics(jobs, n, &m);
    if (verbose) {
        print_schedule_report("Priority Scheduling (Non-Preemptive)",
                              "Priority tie-break: earlier arrival, then lower job ID",
                              segs, nsegs, jobs, n, m);
    }
    return m;
}

int run_sched_compare_file(const char *path, int quantum) {
    job_t base_jobs[MAX_JOBS];
    int n = 0;
    int rc = load_jobs(path, base_jobs, &n);
    if (rc != EXIT_OK) {
        return rc;
    }

    sched_metrics_t m_fcfs = sim_fcfs(base_jobs, n, false);
    sched_metrics_t m_rr   = sim_rr(base_jobs, n, quantum, false);
    sched_metrics_t m_prio = sim_prio(base_jobs, n, false);

    char rr_label[32];
    snprintf(rr_label, sizeof(rr_label), "RR (q=%d)", quantum);

    printf("Policy      Avg TAT   Avg WT\n");
    printf("%-11s %7.2f   %6.2f\n", "FCFS", m_fcfs.avg_tat, m_fcfs.avg_wt);
    printf("%-11s %7.2f   %6.2f\n", rr_label, m_rr.avg_tat, m_rr.avg_wt);
    printf("%-11s %7.2f   %6.2f\n", "PRIORITY", m_prio.avg_tat, m_prio.avg_wt);

    const char *best = "FCFS";
    double best_wt = m_fcfs.avg_wt;
    if (m_rr.avg_wt < best_wt) {
        best_wt = m_rr.avg_wt;
        best = rr_label;
    }
    if (m_prio.avg_wt < best_wt) {
        best_wt = m_prio.avg_wt;
        best = "PRIORITY";
    }
    printf("Best (lowest avg WT): %s\n", best);
    return EXIT_OK;
}

int run_sched(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Error: missing scheduling policy or jobs file\n\n");
        print_usage(stderr);
        return EXIT_USAGE;
    }

    const char *policy = argv[0];
    const char *file = argv[1];

    if (strcmp(policy, "fcfs") == 0) {
        if (argc != 2) {
            fprintf(stderr, "Error: 'sched fcfs' requires only <jobs_file>\n\n");
            print_usage(stderr);
            return EXIT_USAGE;
        }
        job_t jobs[MAX_JOBS];
        int n = 0;
        int rc = load_jobs(file, jobs, &n);
        if (rc != EXIT_OK) {
            return rc;
        }
        sim_fcfs(jobs, n, true);
        return EXIT_OK;
    }

    if (strcmp(policy, "prio") == 0) {
        if (argc != 2) {
            fprintf(stderr, "Error: 'sched prio' requires only <jobs_file>\n\n");
            print_usage(stderr);
            return EXIT_USAGE;
        }
        job_t jobs[MAX_JOBS];
        int n = 0;
        int rc = load_jobs(file, jobs, &n);
        if (rc != EXIT_OK) {
            return rc;
        }
        sim_prio(jobs, n, true);
        return EXIT_OK;
    }

    if (strcmp(policy, "rr") == 0 || strcmp(policy, "compare") == 0) {
        if (argc != 3) {
            fprintf(stderr, "Error: 'sched %s' requires <jobs_file> <quantum>\n\n", policy);
            print_usage(stderr);
            return EXIT_USAGE;
        }
        int q = 0;
        if (!parse_strict_int(argv[2], &q) || q < 1) {
            fprintf(stderr, "Error: invalid quantum '%s' (must be integer >= 1)\n\n", argv[2]);
            print_usage(stderr);
            return EXIT_USAGE;
        }
        if (strcmp(policy, "compare") == 0) {
            return run_sched_compare_file(file, q);
        }
        job_t jobs[MAX_JOBS];
        int n = 0;
        int rc = load_jobs(file, jobs, &n);
        if (rc != EXIT_OK) {
            return rc;
        }
        sim_rr(jobs, n, q, true);
        return EXIT_OK;
    }

    fprintf(stderr, "Error: unknown scheduling policy '%s'\n\n", policy);
    print_usage(stderr);
    return EXIT_USAGE;
}
