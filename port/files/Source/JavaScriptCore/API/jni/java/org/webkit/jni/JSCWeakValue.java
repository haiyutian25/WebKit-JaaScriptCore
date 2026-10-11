/*
 * Copyright (C) 2026 The WebKit Android JSC port authors.
 *
 * JNI 桥接层 —— 弱值（阶段 4.3 简化版）。
 * 对应方案 §6.2 的 JSCJniWeakValue，参照 glib 的 JSCWeakValue（cleared 通知）。
 */
package org.webkit.jni;

import java.lang.ref.WeakReference;

/**
 * 对某个 Java 对象的弱观察。
 *
 * <p>语义与 glib 的 {@code JSCWeakValue} 对齐：持有的值被回收后，{@link #get()} 返回
 * {@code null}、{@link #isCleared()} 为真（惰性判断，不保证即时通知）。
 *
 * <p><b>实现说明（阶段 4.3）</b>：本版本用 Java 侧 {@link WeakReference} 实现。在 JNI 桥的
 * 语义下这是等价的：wrapper 的生命周期跟随其 Java 对象 —— {@code JSCContext.garbageCollect()}
 * 会在 JVM 回收后清扫 wrapper 表、释放对应的 JS 侧保护，因此"Java 对象被回收"即等价于
 * "JS 侧的包装已失效"。完整的"JS 侧弱引用 + GC 回调通知"版本留待后续（需要 JSC 的
 * {@code Weak<JSObject>} 与 WeakHandleOwner，属跨语言 GC 集成的深水区）。
 */
public final class JSCWeakValue implements AutoCloseable {
    private WeakReference<Object> mReference;

    public JSCWeakValue(Object value) {
        if (value == null)
            throw new IllegalArgumentException("JSCWeakValue cannot wrap null");
        mReference = new WeakReference<>(value);
    }

    /** 取回原对象；已被回收则返回 {@code null}。 */
    public Object get() {
        WeakReference<Object> reference = mReference;
        return reference == null ? null : reference.get();
    }

    /** 值是否已被回收（cleared）。 */
    public boolean isCleared() {
        return get() == null;
    }

    @Override
    public void close() {
        mReference = null;
    }
}
