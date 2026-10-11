#!/system/bin/sh
# Startup benchmark: run jsc N times (used with the host's `time` wrapper).
BASE=/data/local/tmp
export LD_LIBRARY_PATH="$BASE"
cd "$BASE" || exit 1
N="${1:-20}"
i=0
while [ "$i" -lt "$N" ]; do
    ./jsc -e "0" >/dev/null 2>&1
    i=$((i + 1))
done
