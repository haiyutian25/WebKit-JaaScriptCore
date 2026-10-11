/*
 * Copyright (C) 2026 The WebKit Android JSC port authors.
 *
 * 最小探针：定位 JSCJniWeakValue（JS 侧弱引用 + GC 通知）未被回收的原因。
 */
package org.webkit.jni;

public final class WeakProbe {
    public static void main(String[] args) throws Exception {
        System.out.println("1: create vm/context");
        System.out.flush();
        try (JSCVirtualMachine vm = new JSCVirtualMachine();
             JSCContext context = new JSCContext(vm)) {
            Object target = new Object();
            JSCJniWeakValue wv = JSCJniWeakValue.create(context, "__wp", target);
            System.out.println("2: created; isCleared=" + wv.isCleared());
            System.out.println("3: JS sees it: typeof globalThis.__wp = " + context.evaluateScript("typeof globalThis.__wp"));
            System.out.flush();

            System.out.println("4: set null");
            context.evaluateScript("globalThis.__wp = null");
            System.out.println("5: after null: value = " + context.evaluateScript("String(globalThis.__wp)"));
            System.out.flush();

            System.out.println("6: gc x1");
            context.garbageCollect();
            System.out.println("7: isCleared=" + wv.isCleared());
            System.out.flush();

            System.out.println("8: delete property");
            context.evaluateScript("delete globalThis.__wp");
            System.out.println("9: after delete: " + context.evaluateScript("typeof globalThis.__wp"));
            System.out.flush();

            for (int i = 0; i < 3; i++)
                context.garbageCollect();
            System.out.println("10: after gc x3 → isCleared=" + wv.isCleared());

            System.out.println("11: allocation pressure (100k objects)");
            context.evaluateScript("for (var i = 0; i < 100000; i++) { var o = {a:i}; }");
            context.garbageCollect();
            System.out.println("12: after pressure+gc → isCleared=" + wv.isCleared());
            System.out.flush();

            wv.close();
            System.out.println("13: done");
        }
    }
}
