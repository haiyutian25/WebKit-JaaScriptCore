// 阶段 5：JIT vs 解释器 性能基线（同一脚本，靠 JSC_ 环境变量切换分层编译）
// 用法：
//   LD_LIBRARY_PATH=/data/local/tmp ./jsc bench.js                       （JIT）
//   JSC_useJIT=false ... LD_LIBRARY_PATH=/data/local/tmp ./jsc bench.js  （解释器）
var f = function () {
    var s = 0;
    for (var i = 0; i < 1000000; i++)
        s += i;
    return s;
};

var best = 1e9;
for (var round = 0; round < 5; round++) {
    var t0 = Date.now();
    for (var k = 0; k < 20; k++)
        f();
    var ms = Date.now() - t0;
    if (ms < best)
        best = ms;
    print("round " + round + ": " + ms + " ms");
}
print("best_ms=" + best + " result=" + f());
