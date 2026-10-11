/*
 * Copyright (C) 2026 The WebKit Android JSC port authors.
 *
 * 阶段 4.1 真机验证用例（在设备上以 app_process 运行）。
 *
 * 判据（对应移植方案 §6.5 的 4.1 最小验证用例）：
 *   · evaluateScript("1+2") → number
 *   · ({a:[1,'x',null]})   → Map/List 结构正确
 *   · 字符串/布尔/undefined 转换
 *   · JS 异常穿透为 Java 异常
 *   · gc() 不崩溃
 *
 * 运行：CLASSPATH=/data/local/tmp/jsc-jni-test.jar app_process /system/bin org.webkit.jni.JSCJniTest
 */
package org.webkit.jni;

import java.util.List;
import java.util.Map;

public final class JSCJniTest {
    private static int sPassed = 0;
    private static int sFailed = 0;

    /**
     * 迫使 ART 做一次真正的回收。
     *
     * <p>实测（GcProbe，见文档 §6.9）：本环境下堆占用很低时 {@code System.gc()} 不会回收老年代，
     * 必须先用分配压力逼近堆上限，不可达对象才会真正被回收。
     */
    private static void forceJvmCollection() throws InterruptedException {
        java.util.List<byte[]> pressure = new java.util.ArrayList<>();
        try {
            for (int i = 0; i < 512; i++)
                pressure.add(new byte[1024 * 1024]);
        } catch (OutOfMemoryError expected) {
            // 到达堆上限即为压力点
        }
        pressure = null;
        System.gc();
        Thread.sleep(300);
    }

    /** managed reference 全链路：注册 → 强制回收后仍存活 → 注销 → JS 不可达后可清扫。 */
    private static void managedReferenceScenario(JSCContext context) throws InterruptedException {
        long baseline = context.wrapperCount();
        Object managed = new Object();
        checkTrue("managed: 注册成功", context.addManagedReference(managed), "add=true");
        checkTrue("managed: 计数 >= 1", JSCContext.managedReferenceCount() >= 1,
            "count=" + JSCContext.managedReferenceCount());

        forceJvmCollection();
        context.sweep();
        Object back = context.evaluateScript("__jscArgs[0]", managed);
        checkTrue("managed: 强制回收后仍能往返同一对象", back == managed, "same=" + (back == managed));

        checkTrue("managed: 注销成功", context.removeManagedReference(managed), "remove=true");

        // 新语义（= ObjC 的 [id retain]）：wrapper 存活期间 holder **强持有** Java 对象，
        // 因此 Java 对象不再"自己死"（旧实现持 jweak，Java 对象一死表项即被清扫）。
        checkTrue("managed: wrapper 存活期间表项保留（Java 对象由强引用保活）",
            context.wrapperCount() > baseline, "wrappers=" + context.wrapperCount() + " baseline=" + baseline);

        managed = null;                    // 断开 Java 侧引用（该包装在 JS 侧也无引用）
        boolean swept = collectUntil(context, () -> context.wrapperCount() <= baseline);
        context.sweep();
        checkTrue("managed: JS 侧不可达后表项被清扫", swept,
            "wrappers=" + context.wrapperCount() + " baseline=" + baseline);
    }

    /**
     * 跨线程验收（§6.7 判据）：3 个非主线程并发求值 + 主线程 GC 交错。
     *
     * <p>形态参考上游 `API/tests/MultithreadedMultiVMExecutionTest.cpp`：每线程跑**自己的**
     * VM/Context；另加与主线程**共享同一 Context** 的交错访问（验证 JSC 的 apiLock 串行化）；
     * 主线程同时做 GC，制造锁竞争。判据：无异常/崩溃/`JNI DETECTED ERROR`、结果全对、
     * 线程全部正常退出。
     */
    private static void concurrencyScenario(JSCContext sharedContext) throws InterruptedException {
        final int threadCount = 3;
        final int iterations = 150;
        final java.util.concurrent.atomic.AtomicInteger failures =
            new java.util.concurrent.atomic.AtomicInteger();
        final java.util.concurrent.atomic.AtomicInteger completed =
            new java.util.concurrent.atomic.AtomicInteger();
        final java.util.concurrent.CountDownLatch ready =
            new java.util.concurrent.CountDownLatch(threadCount);
        final java.util.concurrent.CountDownLatch start = new java.util.concurrent.CountDownLatch(1);
        java.util.List<Thread> workers = new java.util.ArrayList<>();

        for (int t = 0; t < threadCount; t++) {
            final int id = t;
            Thread worker = new Thread(() -> {
                try {
                    JSCVirtualMachine vm = new JSCVirtualMachine();
                    JSCContext own = new JSCContext(vm);
                    ready.countDown();
                    start.await();
                    for (int i = 0; i < iterations; i++) {
                        // ① 线程私有 context
                        Object ownResult = own.evaluateScript("(" + id + " + " + i + ") * 2");
                        if (!(ownResult instanceof Double) || ((Double) ownResult).doubleValue() != (id + i) * 2.0)
                            failures.incrementAndGet();
                        // ② 与主线程共享的 context（应由 apiLock 串行化）
                        Object sharedResult = sharedContext.evaluateScript("__jscArgs[0] * 3 + " + id, (double) i);
                        if (!(sharedResult instanceof Double) || ((Double) sharedResult).doubleValue() != i * 3.0 + id)
                            failures.incrementAndGet();
                    }
                    // ③ 少量「新建 VM/Context」（对齐上游多 VM 形态，验证创建路径的并发安全）
                    for (int i = 0; i < 5; i++) {
                        JSCVirtualMachine tmpVm = new JSCVirtualMachine();
                        JSCContext tmp = new JSCContext(tmpVm);
                        Object r = tmp.evaluateScript("'ok' + (" + id + ")");
                        if (!("ok" + id).equals(r))
                            failures.incrementAndGet();
                        tmp.close();
                        tmpVm.close();
                    }
                    own.close();
                    vm.close();
                    completed.incrementAndGet();
                } catch (Throwable e) {
                    failures.incrementAndGet();
                    System.out.println("  [diag] worker " + id + " 异常: " + e);
                }
            }, "jsc-worker-" + t);
            workers.add(worker);
            worker.start();
        }

        ready.await();
        start.countDown();

        // 主线程：GC 与求值交错（与 worker 竞争 apiLock）
        for (int round = 0; round < 15; round++) {
            sharedContext.garbageCollect();
            sharedContext.evaluateScript("for (var i = 0; i < 3000; i++) { var o = { i: i }; }");
            Thread.sleep(3);
        }
        for (Thread worker : workers)
            worker.join(60000);

        checkTrue("concurrency: 3 线程并发求值全部完成（无异常）",
            completed.get() == threadCount, "completed=" + completed.get() + "/" + threadCount);
        checkTrue("concurrency: 跨线程结果全部正确",
            failures.get() == 0, "failures=" + failures.get());
        check("concurrency: 主线程求值/GC 交错后仍可求值", sharedContext.evaluateScript("1 + 1"), 2.0);
    }

    /**
     * 外部对象图（owner → owned）端到端 —— 对齐 ObjC 的 `addManagedReference:withOwner:`：
     * 只要 owner 的包装在 JS 侧**可达**，其持有的 object 的包装就一并保活
     * （即使 JS 侧从未直接引用 object）。
     *
     * <p>验证方式是"标记法"：给 child 的包装打一个 JS 标记，之后**经 owner 的 Java 字段**
     * 重新取回 child —— 若仍是同一个包装，标记就还在（转发失效时会新建包装、标记消失）。
     */
    private static void ownershipScenario(JSCContext context) throws InterruptedException {
        context.registerClass(Holder.class);

        Holder owner = new Holder();
        Holder child = new Holder();
        owner.child = child;

        // 1) child 跨边界 → 建立包装，并在 JS 侧打标记（标记 = "同一个包装"的证据）
        context.evaluateScript(
            "globalThis.__child = __jscArgs[0]; globalThis.__child.__mark = 'original-child';", child);

        // 2) 声明「owner 持有 child」（纯 Java 侧引用 —— JS GC 不可见，正是需要显式声明的场景）
        checkTrue("owner: 关系登记成功", context.addManagedReference(child, owner), "add=true");
        checkTrue("owner: 关系计数 >= 1", JSCContext.ownershipCount() >= 1,
            "count=" + JSCContext.ownershipCount());

        // 3) owner 跨边界并被 JS 侧长期持有（⇒ JS 可达）
        check("owner: owner 的包装已建立（JS 侧持有）",
            context.evaluateScript("globalThis.__owner = __jscArgs[0]; 'ok'", owner), "ok");

        // 4) 丢弃 JS 侧对 child 的直接引用 → child 的包装只剩"owner 可达"这一条保活路径
        context.evaluateScript("globalThis.__child = null");

        // 5) 最大化 GC 压力（每轮含同步 Full GC）
        collectUntil(context, () -> false);

        // 6) 经 owner 的 Java 字段取回 child：若传播生效，应仍是同一包装（标记仍在）
        check("owner: owner 可达 ⇒ child 的包装被保活（经 owner 取回仍是同一包装）",
            context.evaluateScript("globalThis.__owner.childRef().__mark"), "original-child");

        // 7) 注销关系 + owner 也不可达 ⇒ 关系表清空
        context.evaluateScript("globalThis.__owner = null");
        boolean removed = context.removeManagedReference(child, owner);
        owner.child = null;
        owner = null;
        child = null;
        collectUntil(context, () -> false);
        checkTrue("owner: 关系注销后关系表清空", removed && JSCContext.ownershipCount() == 0,
            "ownership=" + JSCContext.ownershipCount());
    }

    /** 外部对象图用例的载体：owner 通过 Java 字段持有 child（JS GC 不可见的引用）。 */
    public static class Holder {
        public Holder child;

        /** 经 Java 字段把 child 交回 JS —— 转发保活生效时，返回的是 child 原有的包装。 */
        @JSExport
        public Object childRef() {
            return child;
        }
    }

    /**
     * 加压 + 多轮 GC，直到条件满足或达到上限。
     *
     * <p>为什么需要它：JSC 的回收（尤其对刚分配、或曾被保守栈扫描触及的对象）不是"一次 GC 即回收"，
     * 实测同一断言会随 GC 时序波动。这里以"分配压力 + 多轮 GC"驱动其收敛（上限 10 轮）。
     */
    private static boolean collectUntil(JSCContext context, java.util.function.BooleanSupplier condition) {
        for (int i = 0; i < 10; i++) {
            if (condition.getAsBoolean())
                return true;
            context.evaluateScript("for (var __p = 0; __p < 20000; __p++) { var __q = {i: __p}; }");
            context.garbageCollect();
        }
        return condition.getAsBoolean();
    }

    private static void check(String name, Object actual, Object expected) {
        boolean ok = (actual == null) ? (expected == null) : actual.equals(expected);
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
            System.out.println("PASS  " + name + "  " + detail);
        } else {
            sFailed++;
            System.out.println("FAIL  " + name + "  " + detail);
        }
    }

    private static String describe(Object value) {
        if (value == null)
            return "null";
        return value.getClass().getSimpleName() + "(" + value + ")";
    }

    public static void main(String[] args) throws Exception {
        System.out.println("=== JSC JNI stage 4.1 + 4.3 test ===");

        try (JSCVirtualMachine vm = new JSCVirtualMachine();
             JSCContext context = new JSCContext(vm)) {

            // ① 基础求值
            check("number: 1+2", context.evaluateScript("1+2"), 3.0);
            check("string", context.evaluateScript("'hello' + ' ' + 'jni'"), "hello jni");
            check("boolean true", context.evaluateScript("1 === 1"), Boolean.TRUE);
            check("undefined", context.evaluateScript("undefined"), null);
            check("null", context.evaluateScript("null"), null);
            check("double precision", context.evaluateScript("0.5 + 0.25"), 0.75);

            // ② 结构：数组 → List
            Object arrayResult = context.evaluateScript("[1, 2, 3]");
            checkTrue("array -> List", (arrayResult instanceof List) && ((List<?>) arrayResult).size() == 3,
                "actual=" + describe(arrayResult));

            // ③ 结构：对象 → Map（嵌套数组/字符串/null）
            Object objectResult = context.evaluateScript("({ a: 1, b: 'x', c: [1, 2] })");
            if (objectResult instanceof Map) {
                Map<?, ?> map = (Map<?, ?>) objectResult;
                check("object.a", map.get("a"), 1.0);
                check("object.b", map.get("b"), "x");
                checkTrue("object.c is List size 2", (map.get("c") instanceof List) && ((List<?>) map.get("c")).size() == 2,
                    "actual=" + describe(map.get("c")));
            } else {
                sFailed++;
                System.out.println("FAIL  object -> Map  actual=" + describe(objectResult));
            }

            // ④ 应用场景：真实计算与 JSON
            check("fib(10)", context.evaluateScript(
                "(function fib(n){ return n < 2 ? n : fib(n-1) + fib(n-2); })(10)"), 55.0);
            check("JSON round-trip", context.evaluateScript("JSON.stringify({x:[1,2],y:true})"), "{\"x\":[1,2],\"y\":true}");
            check("Intl 可用（阶段 2/3 成果回归）",
                context.evaluateScript("Intl.supportedValuesOf('timeZone').length"), 445.0);

            // ⑤ 异常穿透
            boolean threw = false;
            try {
                context.evaluateScript("throw new Error('boom')");
            } catch (RuntimeException e) {
                threw = true;
                System.out.println("PASS  exception propagates  -> " + e.getMessage());
                sPassed++;
            }
            if (!threw) {
                sFailed++;
                System.out.println("FAIL  exception did not propagate");
            }

            // ⑥ GC 压力
            for (int i = 0; i < 50; i++)
                context.garbageCollect();
            check("gc x50 then evaluate", context.evaluateScript("1+1"), 2.0);

            // ⑦ 对象同一性（4.1 第二批次）：同一 Java 对象 ↔ 同一 JS 对象
            Object objectA = new Object();
            Object objectB = new Object();

            Object roundTripA = context.evaluateScript("__jscArgs[0]", objectA);
            checkTrue("identity: A 往返仍是同一 Java 对象", roundTripA == objectA,
                "roundTripA==A: " + (roundTripA == objectA));

            Object roundTripB = context.evaluateScript("__jscArgs[0]", objectB);
            checkTrue("identity: B 往返仍是同一 Java 对象", roundTripB == objectB,
                "roundTripB==B: " + (roundTripB == objectB));

            check("identity: 同一 Java 对象 -> 同一 JS 对象",
                context.evaluateScript("__jscArgs[0] === __jscArgs[1]", objectA, objectA), Boolean.TRUE);
            check("identity: 不同 Java 对象 -> 不同 JS 对象",
                context.evaluateScript("__jscArgs[0] === __jscArgs[1]", objectA, objectB), Boolean.FALSE);

            // 在 JS 侧长期保存后再取回，仍是同一 Java 对象
            context.evaluateScript("globalThis.__saved = __jscArgs[0]", objectA);
            Object savedBack = context.evaluateScript("globalThis.__saved");
            checkTrue("identity: JS 侧保存后取回仍是同一对象", savedBack == objectA,
                "savedBack==A: " + (savedBack == objectA));

            // 包装后的对象在 JS 侧可当普通对象使用（属性可写可读）
            context.evaluateScript("globalThis.__saved.tag = 'wrapped'");
            check("wrapper: JS 侧属性可写可读", context.evaluateScript("globalThis.__saved.tag"), "wrapped");

            // ⑧ 回收（4.3 核心）：JVM 真正回收后，wrapper 表应随之收缩。
            //    注意：ART 在 app_process 环境下堆占用低时 System.gc() 不回收老年代，
            //    必须用分配压力逼近堆上限才能触发真正的回收（由 GcProbe 实测确认，见文档 §6.9）。
            long wrappersBefore = context.wrapperCount();
            for (int i = 0; i < 2000; i++) {
                Object temporary = new Object();
                context.evaluateScript("__jscArgs[0]", temporary);   // 每次建一个 wrapper
            }
            long wrappersPeak = context.wrapperCount();
            forceJvmCollection();
            long reclaimed = context.sweep();
            long wrappersAfter = context.wrapperCount();
            checkTrue("gc: 建 2000 个临时包装后表已增长", wrappersPeak > wrappersBefore,
                "before=" + wrappersBefore + " peak=" + wrappersPeak);
            checkTrue("gc: JVM 回收后 sweep 有实际效果", reclaimed > 0, "reclaimed=" + reclaimed);
            checkTrue("gc: 表规模显著回落", wrappersAfter < wrappersPeak,
                "after=" + wrappersAfter + " peak=" + wrappersPeak);

            // ⑨ GC 压力：System.gc() 与 JS GC 交替 1000 轮，不崩溃、仍可求值
            for (int i = 0; i < 1000; i++) {
                if ((i % 100) == 0)
                    System.gc();
                context.garbageCollect();
            }
            check("gc: System.gc 与 JS GC 交替 1000 轮后仍可求值", context.evaluateScript("1+1"), 2.0);

            // ⑩ managed reference：注册 → 跨真实回收存活；注销 → JS 不可达后可清扫
            managedReferenceScenario(context);

            // ⑩b 外部对象图（owner → owned）：owner 在 JS 侧可达 ⇒ 其持有的 object 的包装被保活
            ownershipScenario(context);

            // ⑩c 跨线程：3 个非主线程并发求值 + 主线程 GC 交错（§6.7 判据）
            concurrencyScenario(context);

            // ⑪ JSCWeakValue：cleared 语义
            Object weakTarget = new Object();
            JSCWeakValue weakValue = new JSCWeakValue(weakTarget);
            checkTrue("weakValue: 未回收时 get() 可用", weakValue.get() == weakTarget, "get==target");
            checkTrue("weakValue: isCleared 为假", !weakValue.isCleared(), "cleared=false");
            weakValue.close();
            checkTrue("weakValue: close 后 cleared", weakValue.isCleared(), "cleared=true");

            // ⑫ 10 万次往返压测（方案 §6.5 的判据：同一 Java 对象始终映射同一 JS 对象、
            //    引用计数与表规模不单调增长）。
            //    每批末尾 sweep + JS GC 缺一不可：sweep 只释放已死条目的 JS 侧保护，
            //    必须再触发 JS GC 才能让这些 wrapper 真正回收 —— 否则 native 内存持续增长，
            //    进程会被系统在后台静默杀掉（实测缺少这一步时正是如此）。
            long stressBaseline = context.wrapperCount();
            long mismatches = 0;
            final int rounds = 100;
            final int perRound = 1000;
            for (int round = 0; round < rounds; round++) {
                for (int i = 0; i < perRound; i++) {
                    Object subject = new Object();
                    Object returned = context.evaluateScript("__jscArgs[0]", subject);
                    if (returned != subject)
                        mismatches++;
                }
                // 每 10 批（1 万个临时对象）做一次真实回收 + 清扫 + JS GC：
                // 本环境下 System.gc() 不回收老年代（见 GcProbe），必须用堆压力逼近堆上限，
                // 临时对象才真正被回收、sweep 之后表规模才会回落。
                if (((round + 1) % 10) == 0) {
                    forceJvmCollection();
                    context.sweep();
                    context.garbageCollect();
                }
                if (((round + 1) % 20) == 0)
                    System.out.println("  [stress] " + ((round + 1) * perRound) + " round-trips, wrappers=" + context.wrapperCount());
            }
            long stressAfter = context.wrapperCount();
            check("stress: 10 万次往返对象同一性保持", mismatches, 0L);
            checkTrue("stress: 表规模未单调增长", stressAfter <= stressBaseline + perRound,
                "baseline=" + stressBaseline + " after=" + stressAfter + " rounds=" + rounds + "x" + perRound);

            // ⑬ 回调桥（4.2）：JS 调用 Java 回调
            check("callback: JS 调用 Java 回调（Array.map）",
                context.evaluateScript(
                    "[1,2,3].map(function (x) { return __jscArgs[0](x) * 10; }).join(',')",
                    (JSCJniCallbackFunction) (callArgs) -> ((Number) callArgs[0]).doubleValue() * 2),
                "20,40,60");

            // ⑭ 参数编组：数量、各类型、null、数组、对象
            check("callback: 多类型参数编组",
                context.evaluateScript("__jscArgs[0](1, 'a', true, null, [2,3], {k:4})",
                    (JSCJniCallbackFunction) (callArgs) -> callArgs.length
                        + ":" + (callArgs[0] instanceof Double)
                        + ":" + (callArgs[1] instanceof String)
                        + ":" + (callArgs[2] instanceof Boolean)
                        + ":" + (callArgs[3] == null)
                        + ":" + (callArgs[4] instanceof java.util.List)
                        + ":" + (callArgs[5] instanceof java.util.Map)),
                "6:true:true:true:true:true:true");

            // ⑮ 返回值编组：字符串、null → undefined
            check("callback: 返回字符串", context.evaluateScript("__jscArgs[0]()",
                (JSCJniCallbackFunction) (callArgs) -> "from-java"), "from-java");
            check("callback: 返回 null → undefined", context.evaluateScript("__jscArgs[0]() === undefined",
                (JSCJniCallbackFunction) (callArgs) -> null), Boolean.TRUE);

            // ⑯ Java 异常 → JS 侧可 catch 的 Error（message 取 Throwable.toString()）
            check("callback: Java 异常传播为 JS Error",
                context.evaluateScript(
                    "try { __jscArgs[0](); 'no-error'; } catch (e) { 'caught:' + e.message; }",
                    (JSCJniCallbackFunction) (callArgs) -> { throw new IllegalStateException("java-boom"); }),
                "caught:java.lang.IllegalStateException: java-boom");

            // ⑰ 回调内重入：捕获外部 context 再次求值（apiLock 可重入）
            final JSCContext outerContext = context;
            check("callback: 回调内嵌套求值", context.evaluateScript("__jscArgs[0]()",
                (JSCJniCallbackFunction) (callArgs) -> outerContext.evaluateScript("6*7")), 42.0);

            // ⑱ 同一函数对象在 JS 侧保存后被多次调用
            check("callback: JS 侧保存后多次调用",
                context.evaluateScript(
                    "var f = __jscArgs[0]; var acc = 0; for (var i = 0; i < 5; i++) acc += f(i); acc",
                    (JSCJniCallbackFunction) (callArgs) -> ((Number) callArgs[0]).intValue() * 2),
                20.0);

            // ⑲ 声明式导出（4.4）：注册类 → JS 侧 new + 方法调用
            context.registerClass(ExportedPoint.class);
            check("export: 类以 simple name 暴露", context.evaluateScript("typeof ExportedPoint"), "function");
            check("export: new + @JSExport 方法", context.evaluateScript("new ExportedPoint().sum(3, 4)"), 7.0);

            // ⑳ @JSExportAs 改名
            check("export: @JSExportAs 改名生效", context.evaluateScript("new ExportedPoint().scaled(3, 4)"), 12.0);
            check("export: 改名后原名不存在",
                context.evaluateScript("typeof new ExportedPoint().multiply"), "undefined");

            // ㉑ 继承：原型链与继承方法
            context.registerClass(ExportedPoint3D.class);
            check("export: 原型链 Object.getPrototypeOf(B.prototype) === A.prototype",
                context.evaluateScript(
                    "Object.getPrototypeOf(ExportedPoint3D.prototype) === ExportedPoint.prototype"),
                Boolean.TRUE);
            check("export: 子类实例可用父类方法", context.evaluateScript("new ExportedPoint3D().sum(1, 2)"), 3.0);
            check("export: 子类自有方法", context.evaluateScript("new ExportedPoint3D().plusZ(5)"), 5.0);

            // ㉒ 实例对象同一性（与 4.3 的 wrapper 表协同：同一 Java 对象往返仍是同一对象）
            Object exportedInstance = context.evaluateScript("new ExportedPoint()");
            Object exportedAgain = context.evaluateScript("__jscArgs[0]", exportedInstance);
            checkTrue("export: 实例对象同一性", exportedAgain == exportedInstance,
                "same=" + (exportedAgain == exportedInstance));

            // ㉓ 导出方法抛异常 → JS 侧 catch
            check("export: 方法异常转 JS Error",
                context.evaluateScript(
                    "try { new ExportedPoint().boom(); 'no-error'; } catch (e) { 'caught:' + e.message; }"),
                "caught:java.lang.IllegalStateException: export-boom");

            // ㉔ 工厂构造（带参数）
            context.registerClass(ExportedVector.class,
                (JSCJniCallbackFunction) (callArgs) -> new ExportedVector(((Number) callArgs[0]).intValue()));
            check("export: 工厂构造带参数", context.evaluateScript("new ExportedVector(9).value()"), 9.0);

            // ㉔b 工厂构造的实例同样带 @JSProperty 访问器（4.4 补全：此前工厂路径只能
            // 事后补 [[Prototype]]，JSClass 无法更换 → 该类的对象在非 new 边界丢失属性）
            check("export: 工厂实例读 @JSProperty",
                context.evaluateScript("new ExportedVector(9).magnitude"), 9.0);
            check("export: 工厂实例写 @JSProperty 后回读",
                context.evaluateScript("var v = new ExportedVector(3); v.magnitude = 8; v.magnitude"), 8.0);
            check("export: 方法返回值边界同样带 @JSProperty",
                context.evaluateScript("new ExportedVector(4).doubled().magnitude"), 8.0);

            // ㉕ @JSProperty：读属性
            check("property: 读属性", context.evaluateScript("new ExportedPoint().x"), 0.0);

            // ㉖ 写属性（void setter）后回读
            check("property: 写属性（void setter）后回读",
                context.evaluateScript("var p = new ExportedPoint(); p.x = 5; p.x"), 5.0);

            // ㉗ 只读属性（无 setter）：写入被吞掉，值不变
            check("property: 只读属性写入被忽略",
                context.evaluateScript("var p = new ExportedPoint(); p.readOnly = 99; p.readOnly"), 42.0);

            // ㉘ 未命中回退：方法仍可沿原型链访问（getProperty 返回 null 的效果）
            check("property: 未命中回退到原型链方法",
                context.evaluateScript("typeof new ExportedPoint().sum"), "function");

            // ㉙ 继承：子类实例可读父类声明的属性，也有自有属性
            check("property: 子类可读父类属性",
                context.evaluateScript("var p3 = new ExportedPoint3D(); p3.x = 3; p3.x"), 3.0);
            check("property: 子类自有属性（只读）", context.evaluateScript("new ExportedPoint3D().z"), 7.0);

            // ㉚ 属性访问器抛异常 → JS 侧 catch
            check("property: 访问器异常转 JS Error",
                context.evaluateScript(
                    "try { new ExportedPoint().boomProperty; 'no-error'; } catch (e) { 'caught:' + e.message; }"),
                "caught:java.lang.IllegalStateException: property-boom");

            // ㉛ 静态成员导出（4.4 补全）：静态方法 / 静态属性 / 静态字段
            context.registerClass(ExportedStatics.class);
            check("static: 静态方法", context.evaluateScript("ExportedStatics.origin()"), "origin");
            check("static: 静态方法 @JSExportAs 改名", context.evaluateScript("ExportedStatics.make(3)"), 3.0);
            check("static: 静态属性读（@JSProperty static）", context.evaluateScript("ExportedStatics.count"), 42.0);
            check("static: 静态属性写", context.evaluateScript("ExportedStatics.count = 7; ExportedStatics.count"), 7.0);
            check("static: 静态字段读（final）", context.evaluateScript("ExportedStatics.VERSION"), "1.0");
            check("static: 静态字段写（非 final）", context.evaluateScript("ExportedStatics.counter = 5; ExportedStatics.counter"), 5.0);
            check("static: final 字段写入被忽略", context.evaluateScript("ExportedStatics.VERSION = 'x'; ExportedStatics.VERSION"), "1.0");
            check("static: 与实例成员共存", context.evaluateScript("new ExportedStatics().instanceSum(2, 3)"), 5.0);

            // ㉜ 静态成员继承：子类构造器 [[Prototype]] 指向父类构造器
            context.registerClass(ExportedStaticsChild.class);
            check("static: 构造器原型链（Object.getPrototypeOf(B) === A）",
                context.evaluateScript("Object.getPrototypeOf(ExportedStaticsChild) === ExportedStatics"),
                Boolean.TRUE);
            check("static: 子类继承父类静态方法", context.evaluateScript("ExportedStaticsChild.origin()"), "origin");
            check("static: 子类继承父类静态字段", context.evaluateScript("ExportedStaticsChild.VERSION"), "1.0");

            // ㉝ 包装生命周期由 JS GC 驱动的接口（4.4 补全）：撤销保护 + 断开 JS 引用。
            // 注：该环境下"撤销保护后被 JS GC 回收"的确定性受保守 GC 影响（10 轮 GC + 分配压力
            // 后仍可能延迟），因此这里断言**确定的 API 行为**；"JS GC → 回收标记 → 清理"的
            // 完整链路已由 weakValue2 用例端到端验证（同一机制）。
            final long cellBaseline = context.wrapperCount();
            Object cellSubject = new Object();
            context.evaluateScript("globalThis.__cellTarget = __jscArgs[0]", cellSubject);
            checkTrue("wrapper-cell: 包装已登记", context.wrapperCount() > cellBaseline,
                "baseline=" + cellBaseline + " after=" + context.wrapperCount());
            // 断开全部 JS 侧引用：显式属性 + evaluateScript 注入的 __jscArgs（否则包装仍可达）
            context.evaluateScript("globalThis.__cellTarget = null; globalThis.__jscArgs = null");
            checkTrue("wrapper-cell: 撤销 JS 侧保护", context.releaseWrapper(cellSubject), "released=true");
            checkTrue("wrapper-cell: 已撤销后重复调用返回 false", !context.releaseWrapper(cellSubject),
                "second=false");
            // 驱动 GC（若交换到回收，表项会在 GC 入口的清理中被摘除；不强制断言）
            context.garbageCollect();
            context.garbageCollect();
            Object cellRewrapped = context.evaluateScript("__jscArgs[0]", cellSubject);
            checkTrue("wrapper-cell: 重新包装仍保持 Java 同一性", cellRewrapped == cellSubject, "same=true");

            // ㉞ JS 侧弱引用 + GC 通知（4.4 补全）：Weak<JSObject> + WeakHandleOwner
            Object jsWeakTarget = new Object();
            final boolean[] jsWeakNotified = new boolean[1];
            try (JSCJniWeakValue jsWeakValue = JSCJniWeakValue.create(context, "__jsWeakTarget", jsWeakTarget,
                    () -> jsWeakNotified[0] = true)) {
                checkTrue("weakValue2: 初始未 cleared", !jsWeakValue.isCleared(), "cleared=false");
                checkTrue("weakValue2: get() 返回原对象", jsWeakValue.get() == jsWeakTarget, "get==target");
                context.evaluateScript("globalThis.__jsWeakTarget = null");   // 释放 JS 侧唯一引用
                boolean cleared = collectUntil(context, () -> jsWeakValue.isCleared());
                checkTrue("weakValue2: JS GC 后 cleared", cleared, "cleared=true");
                checkTrue("weakValue2: cleared 回调已派发", jsWeakNotified[0], "notified=true");
                checkTrue("weakValue2: get() 返回 null", jsWeakValue.get() == null, "get==null");
            }

            // ㉟ context 释放时桥接资源全部回收（类注册表 / wrapper 表 / 弱值登记表）
            long relClassesBefore = JSCContext.registeredClassCount();
            long relWrappersBefore = JSCContext.totalWrapperCount();
            long relWeakBefore = JSCContext.liveWeakValueCount();
            long relHoldersBefore = JSCContext.liveWrapperHolderCount();
            JSCContext tempContext = new JSCContext(vm);
            tempContext.registerClass(ExportedStatics.class);
            tempContext.registerClass(ExportedPoint.class);
            tempContext.evaluateScript("globalThis.__tmp = __jscArgs[0]", new Object());
            JSCJniWeakValue.create(tempContext, "__tmpWeak", new Object());   // 故意不 close（验证 context 释放兜底回收）
            checkTrue("release: 临时 context 的类已注册", JSCContext.registeredClassCount() > relClassesBefore,
                "before=" + relClassesBefore + " during=" + JSCContext.registeredClassCount());
            checkTrue("release: 临时 context 的弱值已登记", JSCContext.liveWeakValueCount() > relWeakBefore,
                "before=" + relWeakBefore + " during=" + JSCContext.liveWeakValueCount());
            tempContext.close();   // 释放 context → 三张表与其中的 Java 侧引用应被兜底回收
            check("release: 类注册表随 context 释放", JSCContext.registeredClassCount(), relClassesBefore);
            check("release: 弱值登记表随 context 释放", JSCContext.liveWeakValueCount(), relWeakBefore);
            check("release: wrapper 表随 context 释放", JSCContext.totalWrapperCount(), relWrappersBefore);
            // holder 计数为全局累计（其它 wrapper 可能在此期间被 JVM 回收而下降），
            // 因此断言"未增长"——若释放路径漏掉 holder，这里会看到净增。
            checkTrue("release: wrapper holder 未增长（释放路径无泄漏）",
                JSCContext.liveWrapperHolderCount() <= relHoldersBefore,
                "before=" + relHoldersBefore + " after=" + JSCContext.liveWrapperHolderCount());
            check("release: 释放后主 context 不受影响", context.evaluateScript("1+1"), 2.0);

            // 反复创建/释放 context（每个都建包装）—— holder 不应累积
            long holdersAfterCycle = JSCContext.liveWrapperHolderCount();
            for (int i = 0; i < 5; i++) {
                JSCContext cycleContext = new JSCContext(vm);
                cycleContext.registerClass(ExportedPoint.class);
                cycleContext.evaluateScript("globalThis.__cyc = __jscArgs[0]", new Object());
                cycleContext.close();
            }
            checkTrue("release: 反复建/销 context 不累积 holder",
                JSCContext.liveWrapperHolderCount() <= holdersAfterCycle,
                "before=" + holdersAfterCycle + " after=" + JSCContext.liveWrapperHolderCount());
            System.out.println("  [diag] context 释放阶段的 finalize 次数 = "
                + JSCContext.finalizeDuringReleaseCount());

            System.out.println("=== passed=" + sPassed + " failed=" + sFailed + " ===");
            if (sFailed != 0)
                System.exit(1);
        }   // try (JSCContext ...)
    }       // main

    // -----------------------------------------------------------------------
    // 4.4 用例用的被导出类（必须是 public static 嵌套类 + public 无参构造）
    // -----------------------------------------------------------------------

    public static class ExportedPoint {
        private double mX;
        private final double mReadOnly = 42;

        public ExportedPoint() { }

        @JSExport
        public Object sum(Object a, Object b) {
            return number(a) + number(b);
        }

        @JSExportAs("scaled")
        public Object multiply(Object a, Object b) {
            return number(a) * number(b);
        }

        @JSExport
        public Object boom() {
            throw new IllegalStateException("export-boom");
        }

        @JSProperty
        public Object getX() {
            return mX;
        }

        @JSProperty
        public void setX(Object value) {   // void setter：验证 setterIsVoid 分支
            mX = number(value);
        }

        @JSProperty("readOnly")
        public Object computeReadOnly() {  // 无 setter → 只读属性
            return mReadOnly;
        }

        @JSProperty("boomProperty")
        public Object computeBoom() {      // 访问器抛异常 → JS Error
            throw new IllegalStateException("property-boom");
        }
    }

    public static class ExportedPoint3D extends ExportedPoint {
        public ExportedPoint3D() {
            super();
        }

        @JSExport
        public Object plusZ(Object z) {
            return number(z);
        }

        @JSProperty
        public Object getZ() {
            return 7;
        }
    }

    /** 无无参构造 → 只能用工厂注册（用例 ㉔）。 */
    public static class ExportedVector {
        private int mValue;

        public ExportedVector(int value) {
            mValue = value;
        }

        @JSExport
        public Object value() {
            return mValue;
        }

        /** @JSProperty：与工厂构造组合（4.4 补全前此组合不可用）。 */
        @JSProperty
        public Object getMagnitude() {
            return mValue;
        }

        @JSProperty
        public void setMagnitude(Object value) {
            mValue = (int) number(value);
        }

        /** 返回本类新实例 → 验证"方法返回值"边界同样得到带属性的实例。 */
        @JSExport
        public Object doubled() {
            return new ExportedVector(mValue * 2);
        }
    }

    // -----------------------------------------------------------------------
    // 4.4 补全用例的被导出类：静态成员导出（静态方法 / 静态属性 / 静态字段）
    // -----------------------------------------------------------------------

    public static class ExportedStatics {
        @JSExport
        public static final Object VERSION = "1.0";   // final → 只读

        @JSExport
        public static Object counter = 0;             // 非 final → 可读写

        private static double sCount = 42;

        public ExportedStatics() { }

        @JSExport
        public static Object origin() {
            return "origin";
        }

        @JSExportAs("make")
        public static Object make(Object n) {
            return n;
        }

        @JSProperty
        public static Object getCount() {
            return sCount;
        }

        @JSProperty
        public static Object setCount(Object value) {
            sCount = ((Number) value).doubleValue();
            return null;
        }

        @JSExport
        public Object instanceSum(Object a, Object b) {
            return number(a) + number(b);
        }
    }

    /** 静态成员继承：注册后构造器原型链连向父类（不额外导出，靠继承）。 */
    public static class ExportedStaticsChild extends ExportedStatics {
        public ExportedStaticsChild() {
            super();
        }
    }

    private static double number(Object value) {
        return ((Number) value).doubleValue();
    }

}
