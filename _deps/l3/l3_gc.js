// RSS after forced GC: same load as l3_peak.js, then drop the references and force a full
// GC, so the host sampler can check whether RSS falls back (no monotonic growth / leak).
var held = [];
for (var i = 0; i < 200000; i++)
    held.push({ x: i, s: 'str-' + i, a: [i, i + 1, i + 2] });
var churn = [];
for (var k = 0; k < 1000000; k++) {
    churn.push({ y: k });
    if (churn.length > 5000) churn = [];
}
held = null;
churn = null;
if (typeof gc === 'function') gc();
for (;;) { }
