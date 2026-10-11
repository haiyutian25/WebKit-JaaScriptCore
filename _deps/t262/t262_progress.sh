#!/system/bin/sh
# 输出进度：第一行=已完成条数，第二行=已收尾分片数，第三行=各分片最后行号
BASE=/data/local/tmp/t262
cd "$BASE" || exit 1
cat results_*.out 2>/dev/null | wc -l
ls state.* 2>/dev/null | wc -l
cat done.* 2>/dev/null | tr '\n' ' '
echo
