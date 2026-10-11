#!/system/bin/sh
# 启动 test262 并行执行（设备端）。
# 用法: t262_launch.sh <chunks> <per_test_timeout_sec>
BASE=/data/local/tmp/t262
CH="${1:-4}"
TMO="${2:-30}"
cd "$BASE" || exit 1

# 先清理**残留的旧 worker**：若上一轮被中断（编排进程被杀），旧 worker 仍会继续写
# results_*.out → 与新 worker 同时写同一批文件名 → 行号重复、结果被污染（实测踩到）。
ps -A -o PID,ARGS 2>/dev/null | awk '/t262_run\.sh/ && !/awk/ {print $1}' | xargs -r kill -9 2>/dev/null
pkill -9 jsc 2>/dev/null
sleep 1

rm -f results_*.out done.* state.* run_*.log
i=0
while [ "$i" -lt "$CH" ]; do
    nohup sh t262_run.sh commands.txt results_"$i".out "$i" "$CH" "$TMO" > run_"$i".log 2>&1 &
    i=$((i + 1))
done
sleep 2
echo "started workers: $(ls results_*.out 2>/dev/null | wc -l)"
