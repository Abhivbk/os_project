#include "common.h"

#include <ctype.h>
#include <errno.h>
#include <string.h>

typedef struct {
    int n;
    int m;
    int avail[MAX_RES];
    int alloc[MAX_PROCS][MAX_RES];
    int max[MAX_PROCS][MAX_RES];
    int need[MAX_PROCS][MAX_RES];
} banker_state_t;

/* Reads next non-empty, non-comment line and parses exact count of integers */
static int read_int_row(FILE *fp, const char *path, int *line_num, int *out, int expected) {
    char line[512];
    while (fgets(line, sizeof(line), fp) != NULL) {
        (*line_num)++;
        char *p = line;
        while (isspace((unsigned char)*p)) {
            p++;
        }
        if (*p == '\0' || *p == '#') {
            continue;
        }

        int count = 0;
        char *saveptr = NULL;
        for (char *tok = strtok_r(p, " \t\r\n", &saveptr);
             tok != NULL;
             tok = strtok_r(NULL, " \t\r\n", &saveptr)) {
            if (tok[0] == '#') {
                break;
            }
            if (count >= expected) {
                fprintf(stderr, "Error: %s:%d: too many integers (expected %d)\n",
                        path, *line_num, expected);
                return -1;
            }
            int val = 0;
            if (!parse_strict_int(tok, &val) || val < 0) {
                fprintf(stderr, "Error: %s:%d: invalid non-negative integer '%s'\n",
                        path, *line_num, tok);
                return -1;
            }
            out[count++] = val;
        }

        if (count != expected) {
            fprintf(stderr, "Error: %s:%d: expected %d integers, got %d\n",
                    path, *line_num, expected, count);
            return -1;
        }
        return 0;
    }

    fprintf(stderr, "Error: %s: unexpected end of file\n", path);
    return -1;
}

static int load_banker_file(const char *path, banker_state_t *st) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "Error: cannot open '%s': %s\n", path, strerror(errno));
        return EXIT_USAGE;
    }

    int line_num = 0;
    int dims[2];
    if (read_int_row(fp, path, &line_num, dims, 2) != 0) {
        fclose(fp);
        return EXIT_USAGE;
    }

    st->n = dims[0];
    st->m = dims[1];
    if (st->n < 1 || st->n > MAX_PROCS || st->m < 1 || st->m > MAX_RES) {
        fprintf(stderr, "Error: %s:%d: N and M must be in 1..10 (got N=%d, M=%d)\n",
                path, line_num, st->n, st->m);
        fclose(fp);
        return EXIT_USAGE;
    }

    if (read_int_row(fp, path, &line_num, st->avail, st->m) != 0) {
        fclose(fp);
        return EXIT_USAGE;
    }

    for (int i = 0; i < st->n; i++) {
        if (read_int_row(fp, path, &line_num, st->alloc[i], st->m) != 0) {
            fclose(fp);
            return EXIT_USAGE;
        }
    }

    for (int i = 0; i < st->n; i++) {
        if (read_int_row(fp, path, &line_num, st->max[i], st->m) != 0) {
            fclose(fp);
            return EXIT_USAGE;
        }
        for (int j = 0; j < st->m; j++) {
            if (st->max[i][j] < st->alloc[i][j]) {
                fprintf(stderr,
                        "Error: %s:%d: Max[%d][%d] (%d) < Allocation[%d][%d] (%d)\n",
                        path, line_num, i, j, st->max[i][j], i, j, st->alloc[i][j]);
                fclose(fp);
                return EXIT_USAGE;
            }
            st->need[i][j] = st->max[i][j] - st->alloc[i][j];
        }
    }

    fclose(fp);
    return EXIT_OK;
}

static void print_matrix(const char *name, int mat[MAX_PROCS][MAX_RES], int n, int m) {
    printf("%s:\n", name);
    for (int i = 0; i < n; i++) {
        printf("  P%d:", i);
        for (int j = 0; j < m; j++) {
            printf(" %2d", mat[i][j]);
        }
        printf("\n");
    }
}

int run_banker(const char *path, int *out_is_safe) {
    banker_state_t st;
    memset(&st, 0, sizeof(st));

    int rc = load_banker_file(path, &st);
    if (rc != EXIT_OK) {
        return rc;
    }

    printf("=== Banker's Safety Algorithm (%s) ===\n", path);
    printf("Available:");
    for (int j = 0; j < st.m; j++) {
        printf(" %d", st.avail[j]);
    }
    printf("\n");

    print_matrix("Allocation", st.alloc, st.n, st.m);
    print_matrix("Max", st.max, st.n, st.m);
    print_matrix("Need", st.need, st.n, st.m);

    int work[MAX_RES];
    for (int j = 0; j < st.m; j++) {
        work[j] = st.avail[j];
    }

    bool finish[MAX_PROCS] = {false};
    int safe_seq[MAX_PROCS];
    int seq_len = 0;

    printf("\nStep trace:\n");
    while (seq_len < st.n) {
        bool progressed = false;
        /* Scan i from 0 each iteration and pick the first unfinished i with Need[i] <= Work */
        for (int i = 0; i < st.n; i++) {
            if (finish[i]) {
                continue;
            }
            bool can_run = true;
            for (int j = 0; j < st.m; j++) {
                if (st.need[i][j] > work[j]) {
                    can_run = false;
                    break;
                }
            }
            if (can_run) {
                printf("P%d can run: Need", i);
                for (int j = 0; j < st.m; j++) {
                    printf(" %d", st.need[i][j]);
                }
                printf(" <= Work");
                for (int j = 0; j < st.m; j++) {
                    printf(" %d", work[j]);
                }
                for (int j = 0; j < st.m; j++) {
                    work[j] += st.alloc[i][j];
                }
                printf(" -> Work becomes");
                for (int j = 0; j < st.m; j++) {
                    printf(" %d", work[j]);
                }
                printf("\n");

                finish[i] = true;
                safe_seq[seq_len++] = i;
                progressed = true;
                break; /* Restart scan from 0 */
            }
        }
        if (!progressed) {
            break;
        }
    }

    if (seq_len == st.n) {
        printf("SAFE STATE. Safe sequence: ");
        for (int i = 0; i < st.n; i++) {
            printf("P%d%s", safe_seq[i], (i + 1 < st.n) ? " -> " : "\n");
        }
        if (out_is_safe) {
            *out_is_safe = 1;
        }
    } else {
        printf("UNSAFE STATE: no process can proceed. Deadlock risk. Unfinished:");
        for (int i = 0; i < st.n; i++) {
            if (!finish[i]) {
                printf(" P%d", i);
            }
        }
        printf("\n");
        if (out_is_safe) {
            *out_is_safe = 0;
        }
    }

    return EXIT_OK;
}
