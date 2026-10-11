#!/system/bin/sh
# RSS sampler (host-driven): start jsc on a JS file that stays alive, sample its RSS, kill it.
#
# Usage: l3_rss.sh <js-file> <delay-seconds> <label>
# Prints: "<label> rss_kb=<n>"
#
# Why not in-process: jsc's readFile() uses a size-based reader and returns nothing for
# /proc pseudo-files, so the JS side cannot read its own /proc/self/status.
BASE=/data/local/tmp
export LD_LIBRARY_PATH="$BASE"
cd "$BASE" || exit 1

JS="$1"
DELAY="${2:-3}"
LABEL="$3"

./jsc "$JS" >/dev/null 2>&1 &
PID=$!
sleep "$DELAY"
RSS=$(ps -o RSS= -p "$PID" 2>/dev/null | tr -d ' \r\n')
if [ -z "$RSS" ]; then RSS=0; fi
kill -9 "$PID" 2>/dev/null
wait "$PID" 2>/dev/null
echo "$LABEL rss_kb=$RSS"
