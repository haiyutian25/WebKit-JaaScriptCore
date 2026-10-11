#!/system/bin/sh
# 设备端 test262 批量执行器（在 Android 上本机循环，避免每条用例一次 adb 往返）。
#
# 用法: t262_run.sh <commands.txt> <result.out> <remainder> <step> [timeout_sec]
#
#   remainder/step 用于把任务切成多份并行：只执行行号 % step == remainder 的命令。
#   结果格式: <行号>|<退出码>|<截断后的输出>
#
# 说明：
#  - 上游 runner 的判定是「退出码 0 且无输出」；这里只记录原始退出码与输出，判定交给主机侧分析器。
#  - 旧 Android linker 会为 libc++_shared.so 打印一条 'unused DT entry' 警告，必须过滤，
#    否则每条用例都会因“有输出”被误判为失败。
BASE=/data/local/tmp/t262
CMDFILE="$1"
OUT="$2"
REM="$3"
STEP="$4"
TMO="${5:-30}"

export LD_LIBRARY_PATH="$BASE"
cd "$BASE" || exit 1

n=0
while IFS= read -r cmd; do
    n=$((n + 1))
    if [ $((n % STEP)) -ne "$REM" ]; then
        continue
    fi
    out=$(timeout "$TMO" sh -c "$cmd" 2>&1)
    rc=$?
    out=$(printf '%s' "$out" | grep -v 'unused DT entry' | tr '\n\t' '  ' | cut -c1-300)
    printf '%s|%s|%s\n' "$n" "$rc" "$out" >> "$OUT"
    echo "$n" > "$BASE/done.$REM"  # 进度标记（最新完成的命令序号）
done < "$CMDFILE"
echo "FINISHED" > "$BASE/state.$REM"
