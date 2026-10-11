// Peak RSS sample: build a churn load (1e6 short-lived objects) and keep ~200k of them
// alive, then hold the process so the host sampler can read RSS.
var held = [];
for (var i = 0; i < 200000; i++)
    held.push({ x: i, s: 'str-' + i, a: [i, i + 1, i + 2] });
var churn = [];
for (var k = 0; k < 1000000; k++) {
    churn.push({ y: k });
    if (churn.length > 5000) churn = [];
}
for (;;) { }
