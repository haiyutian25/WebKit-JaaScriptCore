/*
 * Copyright (C) 2026 The WebKit Android JSC port authors.
 *
 * Minimal reproducer for the L4 finding: returning a JS object that (transitively) wraps a
 * Java object back to Java aborts the process with "JNI DETECTED ERROR: mid == null"
 * (art::JNI::NewObjectV called from libJavaScriptCore.so).
 *
 * Usage: app_process ... org.webkit.jni.L4Probe <case>
 *   A  plain JS object                     -> Map
 *   B  Java Map passed in, returned as-is
 *   C  nested Java Map, return the sub-object
 *   D  Java List passed in, returned as-is
 *   E  Java Map containing a Java List, return the List
 *   F  JS object built in JS from the Java Map (spread copy), return it
 *
 * Every case runs in its own process on purpose: a crashing case kills the VM.
 */
package org.webkit.jni;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

public final class L4Probe {
    public static void main(String[] args) {
        String c = args.length > 0 ? args[0] : "A";
        System.out.println("case=" + c + " start");
        try (JSCVirtualMachine vm = new JSCVirtualMachine();
             JSCContext context = new JSCContext(vm)) {
            Object result = run(context, c);
            System.out.println("case=" + c + " OK result=" + describe(result));
        } catch (Throwable t) {
            System.out.println("case=" + c + " THROW " + t);
        }
        System.out.println("case=" + c + " end");
    }

    private static Object run(JSCContext context, String c) {
        switch (c) {
            case "A":
                return context.evaluateScript("({a: 1, b: 'x', c: [1, 2]})");
            case "B": {
                Map<String, Object> m = map("k", "v");
                return context.evaluateScript("__jscArgs[0]", m);
            }
            case "C": {
                Map<String, Object> inner = map("n", 2.0);
                Map<String, Object> outer = map("k", inner);
                return context.evaluateScript("__jscArgs[0].k", outer);
            }
            case "D": {
                List<Object> l = new ArrayList<>();
                l.add("a");
                l.add(1.0);
                return context.evaluateScript("__jscArgs[0]", l);
            }
            case "E": {
                List<Object> l = new ArrayList<>();
                l.add(1.0);
                l.add(2.0);
                Map<String, Object> m = map("list", l);
                return context.evaluateScript("__jscArgs[0].list", m);
            }
            case "F": {
                Map<String, Object> m = map("k", "v");
                return context.evaluateScript(
                    "(function (o) { var copy = {}; for (var p in o) copy[p] = o[p]; return copy; })"
                    + "(__jscArgs[0])", m);
            }
            case "G": {
                // Argument path only: pass a Java object in, return a constant.
                Map<String, Object> m = map("k", "v");
                return context.evaluateScript("'const'", m);
            }
            case "H": {
                // Return path only: no arguments, return a plain JS object.
                return context.evaluateScript("({k: 'v'})");
            }
            default:
                return "unknown case";
        }
    }

    private static Map<String, Object> map(String k, Object v) {
        Map<String, Object> m = new LinkedHashMap<>();
        m.put(k, v);
        return m;
    }

    private static String describe(Object v) {
        if (v == null)
            return "null";
        if (v instanceof Map)
            return "Map" + v;
        if (v instanceof List)
            return "List" + v;
        return v.getClass().getSimpleName() + "[" + v + "]";
    }
}
