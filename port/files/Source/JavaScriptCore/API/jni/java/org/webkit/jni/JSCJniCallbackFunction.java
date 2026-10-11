/*
 * Copyright (C) 2026 The WebKit Android JSC port authors.
 *
 * JNI 桥接层 —— JS 回调接口（阶段 4.2 回调桥）。
 * 对应方案 §6.2 的 JSCJniCallbackFunction。
 */
package org.webkit.jni;

/**
 * 可由 JS 调用的 Java 回调。
 *
 * <p>把本接口的实现（通常是 lambda）作为参数传给
 * {@link JSCContext#evaluateScript(String, Object...)}，它在 JS 侧就是一个**可调用的函数**：
 *
 * <pre>{@code
 * Object result = context.evaluateScript(
 *     "[1,2,3].map(function (x) { return __jscArgs[0](x); })",
 *     (JSCJniCallbackFunction) (args) -> ((Number) args[0]).doubleValue() * 2);
 * }</pre>
 *
 * <p><b>参数与返回值编组</b>：{@code arguments} 是 JS 实参按 {@link JSCContext} 的常规转换规则
 * 转成的 Java 对象（JS 的 {@code null}/{@code undefined} → Java {@code null}）；
 * 返回值按 Java → JS 规则转回。返回 {@code null} 时 JS 侧得到 {@code undefined}。
 *
 * <p><b>异常</b>：本方法抛出的异常会被转成 JS 侧可 {@code catch} 的 {@code Error}
 * （{@code message} 取 {@code Throwable.toString()}），JS 侧 catch 后流程正常继续。
 *
 * <p><b>线程</b>：回调在 JS 执行线程上同步发生；若该线程尚未附着到 JVM，桥接层会自动附着。
 *
 * <p><b>重入</b>：回调内可以再次调用 {@link JSCContext#evaluateScript}（JSC 的 apiLock 可重入）。
 *
 * <p><b>保活</b>：桥接层持有回调对象的强引用，并在 JS 侧函数对象被回收时释放；
 * 因此 JS 侧仍持有该函数期间，回调对象不会被 JVM 回收。
 *
 * <p>本接口不带 {@code context} 参数：若回调内需要操作 JS，请让 lambda 捕获外层的
 * {@link JSCContext}（见上文示例的用法）。
 */
@FunctionalInterface
public interface JSCJniCallbackFunction {
    /**
     * 被 JS 调用。
     *
     * @param arguments JS 实参（可为空数组，元素可为 {@code null}）
     * @return 返回给 JS 的值；{@code null} 表示 {@code undefined}
     * @throws Exception 任意异常都会被转成 JS 侧的 {@code Error}
     */
    Object invoke(Object[] arguments) throws Exception;
}
