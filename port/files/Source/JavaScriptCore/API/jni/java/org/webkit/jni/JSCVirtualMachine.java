/*
 * Copyright (C) 2026 The WebKit Android JSC port authors.
 *
 * JNI 桥接层 —— JS 虚拟机（包装 JSContextGroupRef）。
 * 移植方案 §6.2 的 JSCJniVirtualMachine 对应物（阶段 4.1 骨架）。
 */
package org.webkit.jni;

/** 一个独立的 JS 虚拟机（对应 JSC 的 JSContextGroup）。闭包后不可再用。 */
public final class JSCVirtualMachine implements AutoCloseable {
    static {
        // libJavaScriptCore.so 提供 JNI_OnLoad（native 侧借此拿到 JavaVM*）。
        // 开发期（app_process）用：app_process -Djava.library.path=/data/local/tmp …；
        // 生产环境（APK）由 jniLibs/<abi>/ 自动解析，无需额外设置。
        tryLoadCcShared();
        System.loadLibrary("JavaScriptCore");
    }

    /**
     * 尽力预加载 libc++_shared.so（libJavaScriptCore.so 的依赖）。
     *
     * <p><b>为什么需要</b>：Android 9 及以下，linker 可以从主库所在目录解析其依赖；
     * <b>Android 10+ 的 linker namespace 不允许</b>（实测 Android 12 上直接加载主库会得到
     * {@code dlopen failed: library "libc++_shared.so" not found: needed by
     * /data/local/tmp/libJavaScriptCore.so in namespace classloader-namespace}）。
     * 先显式加载同名库后，主库的依赖解析会命中已加载的它。
     *
     * <p>加载失败不在此报错：Android 9 上无需预加载，且主库加载失败时给出的错误信息更完整。
     */
    private static void tryLoadCcShared() {
        try {
            System.loadLibrary("c++_shared");
            return;
        } catch (Throwable ignored) {
            // 落到显式路径扫描（app_process 场景把两个 .so 放在同一目录时走这里）
        }
        try {
            String libraryPath = System.getProperty("java.library.path", "");
            for (String directory : libraryPath.split(java.io.File.pathSeparator)) {
                if (directory.isEmpty())
                    continue;
                java.io.File candidate = new java.io.File(directory, "libc++_shared.so");
                if (candidate.isFile()) {
                    System.load(candidate.getAbsolutePath());
                    return;
                }
            }
        } catch (Throwable ignored) {
            // 交给后续 System.loadLibrary("JavaScriptCore") 报告真实错误
        }
    }

    private long mHandle;

    public JSCVirtualMachine() {
        mHandle = nativeCreate();
    }

    /** 供 JSCContext 使用；0 表示已关闭。 */
    long handle() {
        return mHandle;
    }

    @Override
    public void close() {
        if (mHandle != 0) {
            nativeRelease(mHandle);
            mHandle = 0;
        }
    }

    /**
     * 通知内存压力（阶段 5 验证矩阵的「内存压力」项）。
     *
     * <p><b>为什么需要宿主转发</b>：JSC 在 Android 上的 {@code MemoryPressureHandler::install()}
     * 只置一个标志、**不注册任何信号源**（见 {@code WTF/wtf/unix/MemoryPressureHandlerUnix.cpp}），
     * 因此真实的内存压力必须由宿主转发过来 —— 在 APK 场景即
     * {@code ComponentCallbacks2.onTrimMemory(int level)}：
     *
     * <pre>{@code
     * @Override public void onTrimMemory(int level) {
     *     boolean critical = (level >= TRIM_MEMORY_COMPLETE)
     *         || (level >= TRIM_MEMORY_RUNNING_CRITICAL);
     *     mVirtualMachine.notifyMemoryPressure(critical);
     * }
     * }</pre>
     *
     * @param critical 是否为严重压力（对应 onTrimMemory 的较高等级）
     */
    public void notifyMemoryPressure(boolean critical) {
        if (mHandle != 0)
            nativeNotifyMemoryPressure(critical);
    }

    /** 观测：内存压力状态变化回调被调用的次数（阶段 5 验证用）。 */
    public static long memoryPressureEventCount() {
        return nativeMemoryPressureEventCount();
    }

    /** 当前是否处于内存压力下（阶段 5 验证用）。 */
    public static boolean isUnderMemoryPressure() {
        return nativeIsUnderMemoryPressure();
    }

    private static native long nativeCreate();
    private static native void nativeRelease(long handle);
    private static native void nativeNotifyMemoryPressure(boolean critical);
    private static native long nativeMemoryPressureEventCount();
    private static native boolean nativeIsUnderMemoryPressure();
}
