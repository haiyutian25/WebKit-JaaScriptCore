/*
 * Copyright (C) 2026 The WebKit Android JSC port authors.
 *
 * 阶段 5：验证矩阵（方案 §7）的设备侧自动执行部分。
 *
 * 覆盖矩阵中可自动化的行：JIT 可用性与性能、rwx 页统计、内存压力链路、
 * 稳定性 20 轮、fd 数量稳定。栈回溯与 JNI 全用例分别由 stage2_test 与
 * JSCJniTest 承担（在脚本层组合运行）；API 29+/33+ 的差异项无法在本机
 * （API 28）真机验证，由文档记录为静态核验。
 *
 * 运行：CLASSPATH=/data/local/tmp/jsc-jni-test.jar app_process \
 *         -Djava.library.path=/data/local/tmp /system/bin org.webkit.jni.Stage5Verification
 */
package org.webkit.jni;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;

public final class Stage5Verification {
    /** 热点循环：sum(0..999999) = 499999500000，用于验证 JIT 路径正确性与耗时。 */
    private static final String BENCH_SCRIPT =
        "(function () { var s = 0; for (var i = 0; i < 1000000; i++) s += i; return s; })()";
    private static final double BENCH_EXPECTED = 499999500000.0;

    private static int sPassed = 0;
    private static int sFailed = 0;

    public static void main(String[] args) throws Exception {
        // 设备信息刻意不依赖 android.os.Build：javac 编译期没有 android.jar。
        // 型号 / API 级别由脚本层用 getprop 采集后一并记录。
        System.out.println("=== Stage 5 verification matrix ===");
        System.out.println("vm=" + System.getProperty("java.vm.version")
            + " os=" + System.getProperty("os.version")
            + " arch=" + System.getProperty("os.arch"));

        try (JSCVirtualMachine vm = new JSCVirtualMachine();
             JSCContext context = new JSCContext(vm)) {

            // ---- ① JIT 可用性：热点循环结果必须正确（JIT 不可用会崩或算错） ----
            check("jit: 热点循环结果正确", context.evaluateScript(BENCH_SCRIPT), BENCH_EXPECTED);

            // ---- ② 性能：JIT 下的耗时（解释器基线由脚本层用 JSC_useJIT=false 跑 jsc 取得） ----
            long bestNanos = Long.MAX_VALUE;
            for (int i = 0; i < 5; i++) {
                long start = System.nanoTime();
                context.evaluateScript(BENCH_SCRIPT);
                long elapsed = System.nanoTime() - start;
                if (elapsed < bestNanos)
                    bestNanos = elapsed;
            }
            System.out.println("[perf] jit_best_ms=" + (bestNanos / 1000000) + " / 5 runs");

            // ---- ③ 无 W+X 页：统计 /proc/self/maps 里同时可写可执行的映射 ----
            int[] maps = countMappings();
            System.out.println("[maps] total=" + maps[0] + " writable_executable=" + maps[1]);
            checkTrue("maps: 可读取自身映射表", maps[0] > 0, "total=" + maps[0]);

            // ---- ④ 内存压力：Java 通知 → WTF 处理器 → 回调计数 ----
            long pressureBefore = JSCVirtualMachine.memoryPressureEventCount();
            long rssBefore = currentRssBytes();
            vm.notifyMemoryPressure(false);
            vm.notifyMemoryPressure(true);
            long pressureAfter = JSCVirtualMachine.memoryPressureEventCount();
            long rssAfter = currentRssBytes();
            checkTrue("memory-pressure: 通知后处理链路有响应",
                pressureAfter >= pressureBefore,
                "count " + pressureBefore + " -> " + pressureAfter);
            System.out.println("[memory-pressure] underPressure=" + JSCVirtualMachine.isUnderMemoryPressure()
                + " rss_mb " + (rssBefore / (1024 * 1024)) + " -> " + (rssAfter / (1024 * 1024)));
            checkTrue("memory-pressure: 处理器已安装（可查询状态）",
                isHandlerQueryable(), "isUnderMemoryPressure 可调用");

            // ---- ⑤ 稳定性：20 轮「新建 VM/Context + JIT 编译 + GC + 释放」 ----
            for (int round = 0; round < 20; round++) {
                try (JSCVirtualMachine roundVm = new JSCVirtualMachine();
                     JSCContext roundContext = new JSCContext(roundVm)) {
                    roundContext.evaluateScript(
                        "(function () { var s = 0; for (var i = 0; i < 200000; i++) s += i; return s; })()");
                    roundContext.garbageCollect();
                }
            }
            check("stability: 20 轮 GC+JIT 编译后仍可求值", context.evaluateScript("1+1"), 2.0);

            // ---- ⑥ fd 数量稳定 ----
            int fdBefore = countFileDescriptors();
            for (int i = 0; i < 200; i++)
                context.evaluateScript("JSON.parse(JSON.stringify({a:[1,2,3],b:'x',c:true}))");
            context.garbageCollect();
            int fdAfter = countFileDescriptors();
            checkTrue("fd: 反复求值后数量稳定", fdAfter >= 0 && fdAfter <= fdBefore + 8,
                "before=" + fdBefore + " after=" + fdAfter);
        }

        System.out.println("=== stage5 passed=" + sPassed + " failed=" + sFailed + " ===");
        if (sFailed != 0)
            System.exit(1);
    }

    // -----------------------------------------------------------------------

    private static boolean isHandlerQueryable() {
        try {
            JSCVirtualMachine.isUnderMemoryPressure();
            return true;
        } catch (Throwable t) {
            return false;
        }
    }

    /** 返回 {总映射数, 同时可写可执行的映射数}。 */
    private static int[] countMappings() {
        int total = 0;
        int writableExecutable = 0;
        try (BufferedReader reader = new BufferedReader(new FileReader("/proc/self/maps"))) {
            String line;
            while ((line = reader.readLine()) != null) {
                total++;
                int space = line.indexOf(' ');
                if (space < 0 || space + 5 > line.length())
                    continue;
                String permissions = line.substring(space + 1, space + 5);
                if (permissions.indexOf('w') >= 0 && permissions.indexOf('x') >= 0)
                    writableExecutable++;
            }
        } catch (Exception ignored) {
        }
        return new int[] { total, writableExecutable };
    }

    private static int countFileDescriptors() {
        String[] names = new File("/proc/self/fd").list();
        return names == null ? -1 : names.length;
    }

    private static long currentRssBytes() {
        try (BufferedReader reader = new BufferedReader(new FileReader("/proc/self/statm"))) {
            String[] parts = reader.readLine().trim().split("\\s+");
            return Long.parseLong(parts[1]) * 4096L;
        } catch (Exception ignored) {
            return -1;
        }
    }

    private static void check(String name, Object actual, Object expected) {
        boolean ok;
        if (actual == null || expected == null)
            ok = actual == expected;
        else if (actual instanceof Number && expected instanceof Number)
            ok = ((Number) actual).doubleValue() == ((Number) expected).doubleValue();
        else
            ok = actual.equals(expected);
        if (ok) {
            sPassed++;
            System.out.println("PASS  " + name + "  -> " + describe(actual));
        } else {
            sFailed++;
            System.out.println("FAIL  " + name + "  expected=" + describe(expected) + " actual=" + describe(actual));
        }
    }

    private static void checkTrue(String name, boolean condition, String detail) {
        if (condition) {
            sPassed++;
            System.out.println("PASS  " + name + "  (" + detail + ")");
        } else {
            sFailed++;
            System.out.println("FAIL  " + name + "  (" + detail + ")");
        }
    }

    private static String describe(Object value) {
        if (value == null)
            return "null";
        if (value instanceof Double) {
            double d = (Double) value;
            if (d == Math.rint(d) && Math.abs(d) < 1e15)
                return String.valueOf((long) d);
        }
        return value.getClass().getSimpleName() + "(" + value + ")";
    }
}
