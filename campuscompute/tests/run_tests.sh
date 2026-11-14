#!/usr/bin/env bash
set -u

PASS_CNT=0
FAIL_CNT=0

check_output() {
    local desc="$1"
    local pattern="$2"
    shift 2
    local out
    out=$("$@" 2>&1)
    local rc=$?
    if [ $rc -eq 0 ] && echo "$out" | grep -Fq "$pattern"; then
        echo "[PASS] $desc"
        PASS_CNT=$((PASS_CNT + 1))
    else
        echo "[FAIL] $desc (exit=$rc, expected pattern='$pattern')"
        echo "$out"
        FAIL_CNT=$((FAIL_CNT + 1))
    fi
}

check_exit_code() {
    local desc="$1"
    local expected_rc="$2"
    shift 2
    "$@" >/dev/null 2>&1
    local rc=$?
    if [ $rc -eq "$expected_rc" ]; then
        echo "[PASS] $desc (exit=$rc)"
        PASS_CNT=$((PASS_CNT + 1))
    else
        echo "[FAIL] $desc (got exit=$rc, expected=$expected_rc)"
        FAIL_CNT=$((FAIL_CNT + 1))
    fi
}

echo "=== Building campuscompute ==="
make clean >/dev/null && make
if [ $? -ne 0 ]; then
    echo "[FAIL] Build failed"
    exit 1
fi

echo "=== Section 7 Functional Checks ==="
check_output "FCFS Avg TAT 13.40" "Average Turnaround Time: 13.40" ./campuscompute sched fcfs tests/jobs.txt
check_output "FCFS Avg WT 8.20"   "Average Waiting Time:    8.20"  ./campuscompute sched fcfs tests/jobs.txt
check_output "RR q=3 Avg TAT 16.60" "Average Turnaround Time: 16.60" ./campuscompute sched rr tests/jobs.txt 3
check_output "RR q=3 Avg WT 11.40"  "Average Waiting Time:    11.40"  ./campuscompute sched rr tests/jobs.txt 3
check_output "Priority Avg TAT 12.20" "Average Turnaround Time: 12.20" ./campuscompute sched prio tests/jobs.txt
check_output "Priority Avg WT 7.00"   "Average Waiting Time:    7.00"  ./campuscompute sched prio tests/jobs.txt
check_output "Sched Compare Best PRIORITY" "Best (lowest avg WT): PRIORITY" ./campuscompute sched compare tests/jobs.txt 3

check_output "Banker Safe Sequence" "SAFE STATE. Safe sequence: P1 -> P3 -> P0 -> P2 -> P4" ./campuscompute banker tests/banker_safe.txt
check_output "Banker Unsafe State"  "UNSAFE STATE: no process can proceed. Deadlock risk. Unfinished: P0 P1 P2 P3 P4" ./campuscompute banker tests/banker_unsafe.txt

check_output "Logger Produced/Consumed 15" "Produced: 15  Consumed: 15  Threads joined: 5  Log lines: 15" ./campuscompute logger
check_output "IPC Cleanup Verified" "Cleanup verified:" ./campuscompute ipc
check_output "Worker Normal Summary" "Workers: 3  Normal: 3  Failed: 0  Zombies left: 0" ./campuscompute worker normal
check_output "Worker Fail Summary"   "Workers: 3  Normal: 1  Failed: 2  Zombies left: 0" ./campuscompute worker fail
check_output "Limits EMFILE" "EMFILE" ./campuscompute limit
check_output "Limits SIGXCPU" "SIGXCPU" ./campuscompute limit
check_output "All Demo Suite" "Overall: ALL PASS" ./campuscompute all

echo "=== Post-Run Leak & Zombie Checks ==="
SHM_LEFT=$(ls /dev/shm 2>/dev/null | grep "campuscompute" || true)
if [ -z "$SHM_LEFT" ]; then
    echo "[PASS] No leftover /dev/shm campuscompute objects"
    PASS_CNT=$((PASS_CNT + 1))
else
    echo "[FAIL] Leftover /dev/shm objects: $SHM_LEFT"
    FAIL_CNT=$((FAIL_CNT + 1))
fi

PGREP_LEFT=$(pgrep -x campuscompute || true)
if [ -z "$PGREP_LEFT" ]; then
    echo "[PASS] No stray campuscompute processes"
    PASS_CNT=$((PASS_CNT + 1))
else
    echo "[FAIL] Stray campuscompute processes: $PGREP_LEFT"
    FAIL_CNT=$((FAIL_CNT + 1))
fi

echo "=== Section 8 Error-Handling Checks ==="
check_exit_code "Unknown command 'foo'" 1 ./campuscompute foo
check_exit_code "Unknown policy 'xyz'" 1 ./campuscompute sched xyz tests/jobs.txt
check_exit_code "Missing RR quantum" 1 ./campuscompute sched rr tests/jobs.txt
check_exit_code "Zero RR quantum" 1 ./campuscompute sched rr tests/jobs.txt 0
check_exit_code "Non-numeric RR quantum" 1 ./campuscompute sched rr tests/jobs.txt abc
check_exit_code "Negative RR quantum" 1 ./campuscompute sched rr tests/jobs.txt -2
check_exit_code "Non-existent jobs file" 1 ./campuscompute sched fcfs nofile.txt
check_exit_code "Malformed jobs_bad.txt" 1 ./campuscompute sched fcfs tests/jobs_bad.txt
check_exit_code "Malformed banker_bad.txt" 1 ./campuscompute banker tests/banker_bad.txt

echo "=== Summary: $PASS_CNT passed, $FAIL_CNT failed ==="
[ $FAIL_CNT -eq 0 ]
