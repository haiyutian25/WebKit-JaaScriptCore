// L3 probe for the Android JSC port (part 2): forced-GC pause + streaming-token latency.
//
// RSS is measured separately (host-side sampler `l3_rss.sh`), because jsc's `readFile()`
// goes through a size-based reader and returns nothing for /proc pseudo-files
// (verified: readFile('/proc/self/status') -> not a string), so in-process RSS is not
// available on this build.
//
// Run on device:  ./jsc l3_probe.js
// Output: one JSON object per line, prefixed with "L3 ".

var clock = (function () {
    if (typeof performance !== 'undefined' && performance.now) {
        return function () { return performance.now(); };
    }
    return function () { return Date.now(); };
})();

function percentile(sorted, p) {
    if (!sorted.length) return 0;
    var idx = Math.ceil(p / 100 * sorted.length) - 1;
    if (idx < 0) idx = 0;
    if (idx >= sorted.length) idx = sorted.length - 1;
    return sorted[idx];
}

function emit(name, obj) {
    obj.probe = name;
    print('L3 ' + JSON.stringify(obj));
}

function forceGC() {
    if (typeof gc === 'function') { gc(); return; }
    if (typeof $262 !== 'undefined' && $262.gc) { $262.gc(); return; }
}

// ---------- 1. forced full-GC pause ----------
// NOTE: this is a *forced full GC* (jsc's gc() = GC + sweep), i.e. the worst case an
// embedding host can trigger on purpose - not the distribution of incremental pauses.
var gcTimes = [];
for (var k = 0; k < 20; k++) {
    var junk = [];
    for (var j = 0; j < 200000; j++) junk.push({ v: j });
    junk = null;
    var t0 = clock();
    forceGC();
    var t1 = clock();
    gcTimes.push(t1 - t0);
}
gcTimes.sort(function (a, b) { return a - b; });
emit('gc_pause_forced_full', {
    n: gcTimes.length,
    p50ms: percentile(gcTimes, 50),
    p99ms: percentile(gcTimes, 99),
    maxms: gcTimes[gcTimes.length - 1]
});

// ---------- 2. streaming token latency ----------
// AI-CLI style stream: append small chunks, and whenever the buffer exceeds ~100 chars
// parse it as JSON and transform it (string ops + object traversal).
function transform(tokens) {
    var out = [];
    for (var i = 0; i < tokens.length; i++) out.push(String(tokens[i]).toUpperCase());
    return out.join('|');
}

var buffer = '';
var latencies = [];
var TOKENS = 20000;
var tStart = clock();
for (var t = 0; t < TOKENS; t++) {
    var c0 = clock();
    buffer += 'tok' + t + ' ';
    if (buffer.length > 100) {
        var parsed = JSON.parse('{"tokens":"' + buffer.replace(/\s+$/, '') + '"}');
        transform(parsed.tokens.split(' '));
        buffer = '';
    }
    latencies.push(clock() - c0);
}
var streamTotal = clock() - tStart;
latencies.sort(function (a, b) { return a - b; });
emit('stream', {
    tokens: TOKENS,
    totalMs: Math.round(streamTotal * 100) / 100,
    perTokenAvgUs: Math.round(streamTotal / TOKENS * 1000),
    p50us: Math.round(percentile(latencies, 50) * 1000),
    p99us: Math.round(percentile(latencies, 99) * 1000),
    maxUs: Math.round(latencies[latencies.length - 1] * 1000)
});

// ---------- 3. long-run throughput decay ----------
// Same transform loop, measured in 5 buckets, to expose throughput decay / GC interference.
var buckets = [];
var BUCKET = 4000;
for (var b = 0; b < 5; b++) {
    var buf = '';
    var s0 = clock();
    for (var i = 0; i < BUCKET; i++) {
        buf += 'tok' + i + ' ';
        if (buf.length > 100) {
            JSON.parse('{"tokens":"' + buf.replace(/\s+$/, '') + '"}');
            buf = '';
        }
    }
    buckets.push(Math.round((clock() - s0) / BUCKET * 1000) / 1000);
}
emit('stream_decay_us_per_token', { buckets: buckets });

emit('done', {});
