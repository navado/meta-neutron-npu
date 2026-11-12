#!/bin/sh
# Neutron NPU driver tests on an i.MX95 board, stock or patched driver. Runs ON THE BOARD as a user
# with passwordless sudo (drop_caches, module parameters). No NPU job is submitted and no model is needed.
#
#   ./run_tests.sh [phases]          default: corner cache writeback
#   phases: corner     correctness and corner cases (TAP lines): sizes, mmap, CACHE_SYNC ranges, zeroing,
#                      fd lifetime, 4 concurrent processes with a data check, 300-buffer leak rounds
#           cache      fill the page cache from a large file, then allocate 2 GiB (5 reps)
#           writeback  heavy buffered writes in the background (dirty pages, like loading container
#                      images), then allocate 2 GiB (5 reps). The stock driver usually fails here.
#           exhaust    OPT-IN, disruptive: allocate until failure; no OOM kill expected
#   env: TESTDIR (default /var/tmp; needs ~12 GB free for writeback, 4 GB for cache)
#
# With the patched driver (/sys/module/neutron/parameters/iommu_noncontig present) every phase runs
# twice: chunked allocation (Y) and the stock contiguous allocation (N). Results: ./results-<stamp>/.
set -u
here=$(cd "$(dirname "$0")" && pwd)
P=/sys/module/neutron/parameters
TESTDIR=${TESTDIR:-/var/tmp}
phases=${*:-corner cache writeback}
R=$here/results-$(date +%Y%m%d-%H%M%S); mkdir -p "$R"
log() { echo "$(date +%T) $*" | tee -a "$R/run.log"; }
[ -c /dev/neutron0 ] || { echo "no /dev/neutron0" >&2; exit 2; }
sudo -n true 2>/dev/null || { echo "needs passwordless sudo" >&2; exit 2; }
gcc -O2 -Wall -o "$R/neutron_test" "$here/neutron_test.c" || exit 2
T=$R/neutron_test

if [ -f $P/iommu_noncontig ]; then
    MODES="Y N"; orig=$(cat $P/iommu_noncontig)
    trap 'echo "$orig" | sudo tee $P/iommu_noncontig >/dev/null; writer_stop' EXIT INT TERM
else
    MODES=stock; trap 'writer_stop' EXIT INT TERM
fi
mode() { [ "$1" = stock ] || echo "$1" | sudo tee $P/iommu_noncontig >/dev/null; }
{
    echo "kernel: $(uname -r) $(uname -v)"; echo "cmdline: $(cat /proc/cmdline)"
    grep -E 'MemTotal|CmaTotal|CmaFree' /proc/meminfo; echo "modes: $MODES"
} > "$R/env.txt"
log "results in $R; phases: $phases; modes: $MODES"

kmark() { cut -d' ' -f1 /proc/uptime; }
kcheck() { # kernel log since marker $1 -> dmesg_$2.log; flag anything alarming
    sudo dmesg | awk -v t="$1" '{ s = $0; sub(/^\[ */, "", s); if (s + 0 >= t + 0) print }' > "$R/dmesg_$2.log"
    if grep -qiE 'Oops|BUG:|WARNING:|Call trace|oom-kill|Out of memory|Unable to handle|smmu.*(fault|event)' "$R/dmesg_$2.log"; then
        log "KERNEL PROBLEM in $2: see dmesg_$2.log"
    fi
}
cma() { awk '/CmaFree/{printf "CmaFree=%dMiB", $2/1024}' /proc/meminfo; }
WFILE=$TESTDIR/neutron-tests-fill
WPID=
writer_start() { # 3 x 4 GB of buffered writes in the background
    ( for i in 1 2 3; do sudo dd if=/dev/zero of=$WFILE-$i bs=4M count=1000 status=none; done ) &
    WPID=$!
}
writer_stop() {
    [ -n "$WPID" ] && kill $WPID 2>/dev/null
    sudo pkill -f "of=$WFILE" 2>/dev/null; [ -n "$WPID" ] && wait $WPID 2>/dev/null; WPID=
    sudo rm -f $WFILE-*
}

for ph in $phases; do case $ph in
corner)
    for m in $MODES; do
        mode $m; k=$(kmark)
        "$T" corner > "$R/corner_$m.tap" 2>&1
        log "corner mode=$m: $(tail -1 "$R/corner_$m.tap")"; kcheck "$k" "corner_$m"
    done ;;
cache)
    sudo dd if=/dev/zero of=$WFILE-cache bs=4M count=1000 status=none; sync
    for rep in 1 2 3 4 5; do for m in $MODES; do
        mode $m; k=$(kmark)
        sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches'; cat $WFILE-cache > /dev/null
        r=$("$T" alloc 2048 1); echo "$r" >> "$R/cache.jsonl"
        log "cache rep=$rep mode=$m $(cma) $r"; kcheck "$k" "cache_${rep}_$m"
    done; done
    sudo rm -f $WFILE-cache ;;
writeback)
    for rep in 1 2 3 4 5; do for m in $MODES; do
        mode $m; k=$(kmark)
        sudo sh -c 'sync; echo 3 > /proc/sys/vm/drop_caches'
        writer_start; sleep 8
        r=$("$T" alloc 2048 1); echo "$r" >> "$R/writeback.jsonl"
        writer_stop
        log "writeback rep=$rep mode=$m $(cma) $r"; kcheck "$k" "writeback_${rep}_$m"
    done; done ;;
exhaust)
    m=$(echo $MODES | cut -d' ' -f1); mode $m; k=$(kmark)
    "$T" exhaust 256 8192 > "$R/exhaust.jsonl" 2>&1
    log "exhaust mode=$m: $(tail -1 "$R/exhaust.jsonl")"; kcheck "$k" exhaust ;;
*) log "unknown phase $ph" ;;
esac; done
log "done: $R"
