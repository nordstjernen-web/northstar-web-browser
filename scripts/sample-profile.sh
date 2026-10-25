#!/usr/bin/env bash
# Northstar Web Browser poor-man's sampling profiler.
#
# Attaches gdb to a running process repeatedly, grabs the top of the page
# engine's stack each time, and aggregates where the engine spends
# wall-clock time. The GUI runs the engine on the "ns-engine" thread and
# headless runs on the main thread; the script samples the oldest thread
# named "ns-engine" (threads it starts inherit the name) and thread 1 when
# there is none (NS_PROFILE_THREAD names another). Useful
# for attributing cost in a long-running headless run (e.g. a Speedometer
# workload) without a perf/instrumented build — a debugoptimized build with
# frame pointers is enough:
#
#   meson setup builddir-prof --buildtype=debugoptimized -Db_lto=false
#   meson compile -C builddir-prof
#
# Usage: scripts/sample-profile.sh <pid> [num-samples]
#   num-samples defaults to 120.
#
# Prints two tables: the leaf function (where the CPU actually is) and the
# inclusive frequency of Northstar Web Browser (ns_*) frames across all stacks.
set -euo pipefail

PID=${1:?usage: sample-profile.sh <pid> [num-samples]}
N=${2:-120}
THREAD=${NS_PROFILE_THREAD:-ns-engine}
SAMPLES=$(mktemp)
trap 'rm -f "$SAMPLES"' EXIT

if ! command -v gdb >/dev/null 2>&1; then
    echo "error: gdb not found" >&2
    exit 1
fi
if ! kill -0 "$PID" 2>/dev/null; then
    echo "error: no process with pid $PID" >&2
    exit 1
fi

echo "Sampling thread \"$THREAD\" of pid $PID, $N times ..." >&2
for _ in $(seq 1 "$N"); do
    kill -0 "$PID" 2>/dev/null || break
    gdb -p "$PID" --batch -nx \
        -ex "set pagination off" -ex "thread apply all bt 12" 2>/dev/null \
        | awk -v want="\"$THREAD\"" '
            /^Thread [0-9]+ / {
                cur = $2; lwp = 0
                if (match($0, /LWP [0-9]+/)) lwp = substr($0, RSTART + 4, RLENGTH - 4) + 0
                inblock = index($0, want) > 0
                if (inblock && (best == 0 || lwp < best)) { best = lwp; stack = "" }
                else if (inblock) inblock = 0
                next
            }
            /^#/ { if (inblock) stack = stack $0 "\n"; else if (cur == "1") first = first $0 "\n" }
            END { printf "%s", best ? stack : first }
          ' >> "$SAMPLES" || true
    echo "---SAMPLE---" >> "$SAMPLES"
done

echo
echo "=== Leaf function (innermost non-waiting frame), top 25 ==="
awk '
/^---SAMPLE---/ { if (leaf!="") print leaf; leaf=""; next }
/^#/ {
    if (leaf=="") {
        if (match($0, / in [A-Za-z_][A-Za-z0-9_]*/)) fn=substr($0, RSTART+4, RLENGTH-4)
        else { n=split($0,a," "); fn=a[2] }
        if (fn ~ /^(poll|__poll|ppoll|g_main_context|g_main_loop|epoll|read|__libc_(read|poll))/) { fn=""; next }
        if (fn!="") leaf=fn
    }
}
END { if (leaf!="") print leaf }
' "$SAMPLES" | sort | uniq -c | sort -rn | head -25

echo
echo "=== Inclusive ns_* frames across all stacks, top 25 ==="
grep -oE " in ns_[A-Za-z0-9_]+| ns_[A-Za-z0-9_]+ \(" "$SAMPLES" \
    | sed -E 's/ in //; s/ \(//; s/^ //' | grep -E "^ns_" \
    | sort | uniq -c | sort -rn | head -25
