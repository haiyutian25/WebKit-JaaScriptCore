/*
 * Copyright (C) 2026 The WebKit Android JSC port authors.
 *
 * JNI 桥接层 —— JS 侧弱引用 + GC 通知（阶段 4.4 补全）。
 * 对应方案 §6.2 的 JSCJniWeakValue；参照 GLib 的 JSCWeakValue（cleared 通知）。
 */
package org.webkit.jni;

/**
 * JS 侧弱引用 + GC 通知（对应 GLib 的 {@code JSCWeakValue}）。
 *
 * <p>用法：
 * <pre>{@code
 * Object target = new Object();
 * try (JSCJniWeakValue weak = JSCJniWeakValue.create(context, "wvTarget", target)) {
 *     // JS 侧此时可经 globalThis.wvTarget 使用该包装
 *     context.evaluateScript("globalThis.wvTarget = null");   // JS 侧释放
 *     context.garbageCollect();                               // JS GC → 回收 → cleared
 *     weak.isCleared();                                       // true
 *     weak.get();                                             // null
 * }
 * }</pre>
 *
 * <p><b>语义</b>：观察「与 target 关联的 JS 包装对象」何时被 JS GC 回收。包装对象
 * <b>不受 {@code JSValueProtect} 保护</b> —— 存活完全由 JS 侧引用决定；因此当
 * {@code globalThis[name]} 被释放（赋 {@code null} / {@code delete}）且无其它 JS 引用时，
 * 下一次 JS GC 即回收它，本对象随之 {@linkplain #isCleared() cleared}。
 *
 * <p><b>通知时机</b>：JS GC 内部只记录"已清除"并入队；Java 回调在 <b>JNI 安全点</b>派发 ——
 * {@link JSCContext#garbageCollect()}、{@link #isCleared()}、{@link #get()} 任一调用都会派发。
 * 因此 {@code garbageCollect()} 返回后回调即已送达（若注册了回调）。
 *
 * <p><b>{@link #get()} 返回 {@code null} 的两种原因</b>：JS 侧包装已被回收（cleared），
 * 或目标 Java 对象已被 JVM 回收（弱引用的自然行为）。
 */
public final class JSCJniWeakValue implements AutoCloseable {
    private long mHandle;

    /** 建立弱引用并把包装暴露为 {@code globalThis[globalName]}。 */
    public static JSCJniWeakValue create(JSCContext context, String globalName, Object target) {
        return create(context, globalName, target, null);
    }

    /**
     * 建立弱引用并把包装暴露为 {@code globalThis[globalName]}。
     *
     * @param clearedCallback 可选：JS 侧包装被回收时回调（在 JNI 安全点派发，至多一次）
     */
    public static JSCJniWeakValue create(JSCContext context, String globalName, Object target,
        Runnable clearedCallback) {
        if (context == null || context.handle() == 0)
            throw new IllegalStateException("JSCContext is closed");
        if (globalName == null || globalName.isEmpty())
            throw new IllegalArgumentException("globalName == null or empty");
        if (target == null)
            throw new IllegalArgumentException("target == null");
        long handle = nativeCreate(context.handle(), globalName, target, clearedCallback);
        if (handle == 0)
            throw new IllegalStateException("cannot create JSCJniWeakValue");
        JSCJniWeakValue result = new JSCJniWeakValue();
        result.mHandle = handle;
        return result;
    }

    /** 取回目标对象；已 cleared（或目标被 JVM 回收）时返回 {@code null}。 */
    public Object get() {
        return mHandle == 0 ? null : nativeGet(mHandle);
    }

    /** JS 侧的包装是否已被回收（cleared）；调用时顺带派发待处理通知。 */
    public boolean isCleared() {
        return mHandle == 0 || nativeIsCleared(mHandle);
    }

    /**
     * 释放句柄。注意：<b>不撤销</b> JS 侧对 {@code globalThis[globalName]} 的引用 ——
     * 若 JS 侧仍持有包装，该包装不会被回收（本对象释放后也不再有通知）。
     */
    @Override
    public void close() {
        if (mHandle != 0) {
            nativeRelease(mHandle);
            mHandle = 0;
        }
    }

    private static native long nativeCreate(long contextHandle, String globalName, Object target,
        Runnable clearedCallback);
    private static native Object nativeGet(long handle);
    private static native boolean nativeIsCleared(long handle);
    private static native void nativeRelease(long handle);
}
