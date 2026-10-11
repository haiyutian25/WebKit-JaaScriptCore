#!/system/bin/sh
# Baseline for the startup benchmark: same loop, but no jsc (fork/exec overhead only).
N="${1:-20}"
i=0
while [ "$i" -lt "$N" ]; do
    /system/bin/true
    i=$((i + 1))
done
