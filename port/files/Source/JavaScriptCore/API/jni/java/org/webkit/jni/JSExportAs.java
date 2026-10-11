/*
 * Copyright (C) 2026 The WebKit Android JSC port authors.
 *
 * JNI 桥接层 —— 声明式导出注解（阶段 4.4）。
 * 对应方案 §6.2 的 JSCJniClass / §6.3「声明式导出」的 Java 注解轨。
 */
package org.webkit.jni;

import java.lang.annotation.ElementType;
import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;
import java.lang.annotation.Target;

/**
 * 把导出的方法改名（对齐 ObjC 的 {@code JSExportAs}）。
 *
 * <pre>{@code
 * @JSExportAs("scaled")
 * public Object multiplyBy(Object factor) { ... }
 * // JS: point.scaled(3)
 * }</pre>
 *
 * <p>与 {@link JSExport} 的关系：{@code @JSExportAs("x")} 等价于 {@code @JSExport("x")}。
 * 两者同时存在时以 {@code @JSExportAs} 为准。
 */
@Retention(RetentionPolicy.RUNTIME)
@Target({ElementType.METHOD, ElementType.FIELD})
public @interface JSExportAs {
    /** 导出到 JS 时使用的属性名。 */
    String value();
}
