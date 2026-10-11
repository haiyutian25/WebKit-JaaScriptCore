/*
 * Copyright (C) 2026 The WebKit Android JSC port authors.
 *
 * 诊断程序（阶段 4.3）：确认 ART 在 app_process 环境下 System.gc() 是否真的回收对象。
 *
 * 背景：JSCJniTest 的回收用例（⑧）中 sweep 报告 reclaimed=0，即 wrapper 表里的 jweak
 * 在被 System.gc() 之后仍全部"存活"。需要先分清两种可能：
 *   A. ART 的 System.gc() 在本环境下确实不回收（则问题在测试/环境，而非桥接实现）；
 *   B. ART 回收了，但我们对 JNI weak global reference 的判死方式有误。
 * 本探针只涉及纯 Java（不加载 native 库），用于排除 A。
 *
 * 运行：CLASSPATH=/data/local/tmp/jsc-jni-test.jar app_process /system/bin org.webkit.jni.GcProbe
 */
package org.webkit.jni;

import java.lang.ref.WeakReference;

public final class GcProbe {
    private static final int COUNT = 2000;

    public static void main(String[] args) throws Exception {
        System.out.println("=== GC probe: ART System.gc() behavior ===");
        System.out.println("vm.version=" + System.getProperty("java.vm.version"));

        Object[] temps = new Object[COUNT];
        @SuppressWarnings("unchecked")
        WeakReference<Object>[] references = new WeakReference[COUNT];
        for (int i = 0; i < COUNT; i++) {
            temps[i] = new Object();
            references[i] = new WeakReference<>(temps[i]);
        }
        temps = null;

        print("baseline              ", references);

        System.gc();
        Thread.sleep(300);
        print("after System.gc()     ", references);

        System.gc();
        System.gc();
        System.gc();
        Thread.sleep(500);
        print("after System.gc() x3  ", references);

        Runtime runtime = Runtime.getRuntime();
        System.out.println("heap: totalMB=" + (runtime.totalMemory() / (1024 * 1024))
            + " freeMB=" + (runtime.freeMemory() / (1024 * 1024))
            + " maxMB=" + (runtime.maxMemory() / (1024 * 1024)));

        // 堆压力段：System.gc() 在堆占用极低时可能不动老年代，用分配压力迫使 ART 做真正的回收
        System.out.println("--- heap pressure ---");
        java.util.List<byte[]> pressure = new java.util.ArrayList<>();
        int allocatedMiB = 0;
        try {
            for (int i = 0; i < 512; i++) {
                pressure.add(new byte[1024 * 1024]);
                allocatedMiB++;
            }
        } catch (OutOfMemoryError expected) {
            System.out.println("hit heap limit at " + allocatedMiB + " MiB (expected)");
        }
        System.out.println("allocatedMiB=" + allocatedMiB);
        pressure = null;
        System.gc();
        Thread.sleep(400);
        print("after heap pressure   ", references);
        System.out.println("=== probe done ===");
    }

    private static void print(String label, WeakReference<Object>[] references) {
        System.out.println(label + " cleared=" + countCleared(references) + "/" + references.length);
    }

    private static int countCleared(WeakReference<Object>[] references) {
        int cleared = 0;
        for (WeakReference<Object> reference : references) {
            if (reference.get() == null)
                cleared++;
        }
        return cleared;
    }
}
