// JIT 曲线探针 v2（方案 §7.x：JIT 是否真的分层接管）
//
// 原理：同一条热点循环，按**递增块大小**测量 ns/迭代。
//   · JIT 开启：小块先走 LLInt（ns/iter 高）→ 基线 JIT / DFG(OSR) / FTL 陆续接管 → ns/iter 显著下降
//     （典型一个数量级以上），即"冷启动慢、热身后快"的曲线。
//   · 纯解释器：各块 ns/iter 基本恒定，曲线平直。
//
// 用法：
//   jsc jit_curve.js
//   jsc --useJIT=false --useBaselineJIT=false --useDFGJIT=false --useFTLJIT=false jit_curve.js
//
// 注：Date.now() 只有 ms 分辨率，故块大小取到 ns/iter 可测的量级。

function hot(n) {
    var s = 0;
    for (var i = 0; i < n; i++)
        s = (s + (i * 3) - (i % 7)) | 0;
    return s;
}

var sizes = [100000, 200000, 400000, 1000000, 2000000, 4000000, 8000000];
var sink = 0;

print("n            ms      ns/iter");
for (var b = 0; b < sizes.length; b++) {
    var n = sizes[b];
    var t0 = Date.now();
    sink += hot(n);
    var dt = Date.now() - t0;
    print(n + "        " + dt + "      " + ((dt * 1e6) / n).toFixed(3));
}

// 再跑一轮完整序列，取"热身后稳定态"的 ns/iter（用于冷/热对比）
var t1 = Date.now();
sink += hot(8000000);
var dt1 = Date.now() - t1;
print("steady_last_block_ns_per_iter = " + ((dt1 * 1e6) / 8000000).toFixed(3));
print("sink = " + (sink | 0));
