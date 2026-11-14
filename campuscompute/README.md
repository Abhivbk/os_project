# CampusCompute (OS Level 3 Mini-Project, UE25CS242A)

A C11 command-line prototype demonstrating CPU scheduling, a pthread producer-consumer logger, POSIX shared memory with named semaphores, the Banker's safety algorithm, fork/waitpid worker monitoring, and setrlimit resource limits.

## Prerequisites
- Ubuntu or WSL Ubuntu
- gcc (C11, pthreads, librt) and make:  sudo apt install build-essential

## Build, test, clean
    make                       # build ./campuscompute (no warnings expected)
    make test                  # automated checks
    make clean                 # remove binary and logs/*.log
    ./campuscompute cleanup    # remove stale campuscompute_* objects in /dev/shm

Run all commands from the project root (the demos read tests/*.txt).

## Commands
    ./campuscompute help
    ./campuscompute sched fcfs    tests/jobs.txt
    ./campuscompute sched rr      tests/jobs.txt 3
    ./campuscompute sched prio    tests/jobs.txt
    ./campuscompute sched compare tests/jobs.txt 3
    ./campuscompute logger
    ./campuscompute ipc
    ./campuscompute banker tests/banker_safe.txt
    ./campuscompute banker tests/banker_unsafe.txt
    ./campuscompute worker normal
    ./campuscompute worker fail
    ./campuscompute limit
    ./campuscompute all

Exit codes: 0 success, 1 usage or malformed input, 2 runtime failure.

## Input formats
Jobs file: one job per line, "id arrival burst priority"; '#' starts a comment.
Banker file: "N M", then Available (M ints), Allocation (N rows), Max (N rows).

## Design notes
- Priority: lower number = higher priority (non-preemptive).
- Tie-break everywhere: earlier arrival first, then lower job ID.
- Round Robin: jobs that arrive during a slice are queued before the preempted job; the quantum is a command-line argument.
- Formulas: TAT = CT - AT, WT = TAT - BT.
- Logger: bounded circular buffer (capacity 4), one mutex, two condition variables (not_full, not_empty), 3 producers x 5 events, 2 consumers; log written to logs/campus.log.
- IPC: shared memory /campuscompute_shm_<pid> plus semaphores /campuscompute_semdata_<pid> and /campuscompute_semslot_<pid>; all removed on exit and on Ctrl+C.
- Banker: the scan restarts from P0 after each step so the safe sequence is deterministic.
- Worker failures are SIMULATED (tagged [SIMULATED FAILURE]); they are not real OS faults.
- Limits: RLIMIT_NOFILE (open() fails with EMFILE) and RLIMIT_CPU (SIGXCPU), applied only inside forked children.
