/*
 * Copyright (C) 2026 The WebKit Android JSC port authors.
 *
 * L4 host-contract tests (test plan section "L4"): the API surface that a Rust harness /
 * TS plugin host would rely on. Complements JSCJniTest (value conversion, wrappers, GC,
 * weak values, object graph, concurrency) with the contracts that were not covered:
 *
 *   1. value round trip incl. undefined
 *   2. deep nested object passing (Java -> JS -> Java)
 *   3. exception propagation JS -> Java
 *   4. exception isolation Java host function -> JS catch (process must survive)
 *   5. callback reentrancy (JS -> Java -> JS)
 *   6. microtask timing at the API boundary (Promise.then)
 *   7. async/await completion through the API boundary
 *   8. JS value lifetime across GC while held by Java
 *
 * Note 6/7 are backed by Source/JavaScriptCore/runtime/JSLock.cpp:301-315: the microtask
 * queue is drained in JSLock::willReleaseLock(), i.e. whenever the API releases the JS lock
 * at an outermost call boundary - so an embedding host needs no explicit drainMicrotasks().
 *
 * Run: CLASSPATH=/data/local/tmp/jsc-jni-test.jar app_process \
 *        -Djava.library.path=/data/local/tmp /system/bin org.webkit.jni.L4ContractTest
 */
package org.webkit.jni;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

public final class L4ContractTest {
    private static int sPassed = 0;
    private static int sFailed = 0;
    private static JSCContext sContext;

    /** Host object exported to JS: used for exception isolation and reentrancy. */
    public static class HostApi {
        @JSExport
        public Object echo(Object value) {
            return value;
        }

        /** Throws on the Java side; JS must be able to catch it and the process must survive. */
        @JSExport
        public Object boom() {
            throw new IllegalStateException("host-boom");
        }

        /** Reentrancy: called from JS, evaluates JS again before returning. */
        @JSExport
        public Object reenter(Object value) {
            return sContext.evaluateScript("(" + value + ") + 1");
        }
    }

    public static void main(String[] args) {
        System.out.println("=== L4 host contract tests ===");
        try (JSCVirtualMachine vm = new JSCVirtualMachine();
             JSCContext context = new JSCContext(vm)) {
            sContext = context;
            context.registerClass(HostApi.class);

            valueRoundTrip(context);
            nestedObject(context);
            exceptionJsToJava(context);
            exceptionJavaToJs(context);
            callbackReentrancy(context);
            microtaskTiming(context);
            asyncAwait(context);
            valueLifetime(context);
        } catch (Throwable t) {
            System.out.println("FATAL " + t);
            t.printStackTrace(System.out);
            sFailed++;
        }

        System.out.println("=== L4 result: passed=" + sPassed + " failed=" + sFailed + " ===");
        // 刻意**不**调用 System.exit()：本类也会被 APK 宿主内联调用，退出码会把 App 进程杀掉。
        // 判定以本节输出为准（解析 "passed=<n> failed=<n>"）。
    }

    // ---- 1. values ----------------------------------------------------------------
    private static void valueRoundTrip(JSCContext context) {
        checkNumber("value: number", context.evaluateScript("1 + 2"), 3.0);
        checkString("value: string", context.evaluateScript("'a' + 'b'"), "ab");
        checkString("value: bool", context.evaluateScript("true"), "true");
        checkNull("value: null", context.evaluateScript("null"));
        checkString("value: undefined via typeof",
            context.evaluateScript("typeof undefined"), "undefined");
        checkNumber("value: NaN is preserved",
            context.evaluateScript("Number.isNaN(0/0) ? 1 : 0"), 1.0);
        checkString("value: big number string form",
            context.evaluateScript("String(2 ** 53)"), "9007199254740992");
    }

    // ---- 2. deep nested structures ------------------------------------------------
    private static void nestedObject(JSCContext context) {
        Map<String, Object> level3 = new LinkedHashMap<>();
        level3.put("tag", "deep");
        level3.put("nums", listOf(1.0, 2.0, 3.0));
        Map<String, Object> level2 = new LinkedHashMap<>();
        level2.put("l3", level3);
        Map<String, Object> level1 = new LinkedHashMap<>();
        level1.put("l2", level2);
        Map<String, Object> root = new LinkedHashMap<>();
        root.put("l1", level1);

        checkString("nested: Java map visible in JS",
            context.evaluateScript("__jscArgs[0].l1.l2.l3.tag", root), "deep");
        checkNumber("nested: element of nested list",
            context.evaluateScript("__jscArgs[0].l1.l2.l3.nums[2]", root), 3.0);
        Object back = context.evaluateScript("__jscArgs[0].l1.l2.l3", root);
        checkTrue("nested: back to Java as Map",
            back instanceof Map, "type=" + (back == null ? "null" : back.getClass().getName()));
    }

    // ---- 3. exception: JS -> Java -------------------------------------------------
    private static void exceptionJsToJava(JSCContext context) {
        boolean thrown = false;
        String detail = "";
        try {
            context.evaluateScript("throw new TypeError('boom-js')");
        } catch (Throwable e) {
            thrown = true;
            detail = describe(e);
        }
        checkTrue("exception: JS throw reaches Java", thrown, detail);
        checkTrue("exception: message preserved", detail.contains("boom-js"), detail);

        // Context must remain usable afterwards.
        checkNumber("exception: context usable after throw",
            context.evaluateScript("1 + 1"), 2.0);
    }

    // ---- 4. exception isolation: Java host fn -> JS catch -------------------------
    private static void exceptionJavaToJs(JSCContext context) {
        Object caught = context.evaluateScript(
            "(function () { try { new HostApi().boom(); return 'not-thrown'; }"
            + " catch (e) { return 'caught:' + e; } })()");
        checkTrue("exception: Java host throw catchable in JS",
            caught != null && String.valueOf(caught).startsWith("caught:"),
            describe(caught));
        checkNumber("exception: engine alive after host throw",
            context.evaluateScript("40 + 2"), 42.0);
    }

    // ---- 5. reentrancy ------------------------------------------------------------
    private static void callbackReentrancy(JSCContext context) {
        checkNumber("reentrancy: JS -> Java -> JS",
            context.evaluateScript("new HostApi().reenter(41)"), 42.0);
        checkString("reentrancy: host echo of JS value",
            context.evaluateScript("new HostApi().echo('ping')"), "ping");
    }

    // ---- 6. microtask timing ------------------------------------------------------
    private static void microtaskTiming(JSCContext context) {
        context.evaluateScript(
            "globalThis.__mt = 'pending';"
            + "Promise.resolve().then(function () { globalThis.__mt = 'ran'; });"
            + " 'scheduled'");
        checkString("microtask: runs before API call returns (JSLock drain)",
            context.evaluateScript("globalThis.__mt"), "ran");
    }

    // ---- 7. async/await -----------------------------------------------------------
    private static void asyncAwait(JSCContext context) {
        context.evaluateScript(
            "globalThis.__av = 0;"
            + "(async function () { globalThis.__av = await Promise.resolve(7); })();"
            + " 'scheduled'");
        checkNumber("async/await: awaited value visible at boundary",
            context.evaluateScript("globalThis.__av"), 7.0);
    }

    // ---- 8. lifetime across GC ----------------------------------------------------
    private static void valueLifetime(JSCContext context) {
        Object held = context.evaluateScript("({ tag: 'kept', n: 5 })");
        context.garbageCollect();
        context.evaluateScript("for (var __i = 0; __i < 20000; __i++) { var __o = { i: __i }; }");
        context.garbageCollect();
        checkString("lifetime: JS object still valid after GC",
            context.evaluateScript("__jscArgs[0].tag", held), "kept");
        checkNumber("lifetime: field still valid after GC",
            context.evaluateScript("__jscArgs[0].n", held), 5.0);
    }

    // ---- helpers ------------------------------------------------------------------
    private static List<Object> listOf(Object... values) {
        List<Object> list = new ArrayList<>();
        for (Object v : values)
            list.add(v);
        return list;
    }

    private static void checkNumber(String name, Object actual, double expected) {
        if (actual instanceof Number && ((Number) actual).doubleValue() == expected)
            pass(name, describe(actual));
        else
            fail(name, describe(actual), String.valueOf(expected));
    }

    private static void checkString(String name, Object actual, String expected) {
        if (actual != null && expected.equals(String.valueOf(actual)))
            pass(name, describe(actual));
        else
            fail(name, describe(actual), expected);
    }

    private static void checkNull(String name, Object actual) {
        if (actual == null)
            pass(name, "null");
        else
            fail(name, describe(actual), "null");
    }

    private static void checkTrue(String name, boolean condition, String actual) {
        if (condition)
            pass(name, actual);
        else
            fail(name, actual, "true");
    }

    private static void pass(String name, String actual) {
        sPassed++;
        System.out.println("PASS  " + name + "  -> " + actual);
    }

    private static void fail(String name, String actual, String expected) {
        sFailed++;
        System.out.println("FAIL  " + name + "  -> actual=" + actual + " expected=" + expected);
    }

    private static String describe(Object value) {
        if (value == null)
            return "null";
        if (value instanceof Double || value instanceof Float)
            return String.valueOf(((Number) value).doubleValue());
        return value.getClass().getSimpleName() + "[" + value + "]";
    }
}
