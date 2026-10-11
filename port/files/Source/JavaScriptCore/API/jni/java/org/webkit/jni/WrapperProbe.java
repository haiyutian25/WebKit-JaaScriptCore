/*
 * Copyright (C) 2026 The WebKit Android JSC port authors.
 *
 * 最小探针：逐步骤验证「Java 对象 → JS wrapper（JSAPIWrapperObject cell）」路径。
 * 用于定位 wrapper 创建/反查/回收链路上的崩溃或挂起点（阶段 4.4 补全调试）。
 */
package org.webkit.jni;

public final class WrapperProbe {
    public static void main(String[] args) throws Exception {
        System.out.println("A: create vm/context");
        System.out.flush();
        try (JSCVirtualMachine vm = new JSCVirtualMachine();
             JSCContext context = new JSCContext(vm)) {
            System.out.println("B: evaluate 1+1 = " + context.evaluateScript("1+1"));
            System.out.flush();

            Object target = new Object();
            System.out.println("C: wrapperCount before = " + context.wrapperCount());
            System.out.flush();

            Object roundTrip = context.evaluateScript("__jscArgs[0]", target);
            System.out.println("D: roundTrip == target : " + (roundTrip == target));
            System.out.flush();

            System.out.println("E: wrapperCount after = " + context.wrapperCount());
            System.out.flush();

            Object roundTrip2 = context.evaluateScript("__jscArgs[0]", target);
            System.out.println("F: second round-trip same : " + (roundTrip2 == target));
            System.out.flush();

            System.out.println("G: JS 侧同一性 = " + context.evaluateScript(
                "(function () { var a = __jscArgs[0]; var b = __jscArgs[1]; return a === b; })()",
                roundTrip, roundTrip2));
            System.out.flush();

            System.out.println("H: releaseWrapper = " + context.releaseWrapper(target));
            System.out.flush();
            System.out.println("I: gc + sweep = " + context.sweep());
            System.out.flush();
            context.garbageCollect();
            System.out.println("J: wrapperCount = " + context.wrapperCount());
            System.out.flush();
            System.out.println("K: done");
            System.out.flush();
        }
    }
}
