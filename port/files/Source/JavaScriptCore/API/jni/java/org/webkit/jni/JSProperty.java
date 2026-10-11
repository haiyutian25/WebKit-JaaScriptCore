/*
 * Copyright (C) 2026 The WebKit Android JSC port authors.
 *
 * JNI 桥接层 —— 声明式导出注解（阶段 4.4 的属性访问器）。
 */
package org.webkit.jni;

import java.lang.annotation.ElementType;
import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;
import java.lang.annotation.Target;

/**
 * 把 getter/setter 方法导出为 JS 的**属性访问器**（而非方法）。
 *
 * <pre>{@code
 * public class Point {
 *     private double mX;
 *
 *     @JSProperty
 *     public Object getX() { return mX; }              // → JS 属性 x（读）
 *
 *     @JSProperty
 *     public Object setX(Object value) {               // → JS 属性 x（写）
 *         mX = ((Number) value).doubleValue();
 *         return null;
 *     }
 * }
 *
 * context.registerClass(Point.class);
 * context.evaluateScript("var p = new Point(); p.x = 5; p.x");   // 5
 * }</pre>
 *
 * <p><b>命名规则</b>：{@link #value()} 为空时按<br>
 * {@code getXxx()} / {@code setXxx(...)} 推导为属性名 {@code xxx}（首字母小写；{@code getURL} → {@code URL}）；
 * 非 {@code get}/{@code set} 前缀的方法则用方法名本身。<br>
 * 显式指定：{@code @JSProperty("length")}。
 *
 * <p><b>读写判定</b>：无参方法视为 getter，单参方法视为 setter；同名者合并为同一个属性。
 *
 * <p><b>签名约定</b>：与方法导出一致 —— 参数与返回值都用 {@code Object}（setter 可由桥接层直接调用，
 * 返回 {@code void} 也可以）。
 *
 * <p><b>回退</b>：属性名未命中时，JS 侧继续沿原型链查找（因此同名的方法仍可访问，
 * 父类声明的属性也对子类实例生效）。
 */
@Retention(RetentionPolicy.RUNTIME)
@Target(ElementType.METHOD)
public @interface JSProperty {
    /** 显式指定属性名；为空则按 getter/setter 命名规则推导。 */
    String value() default "";
}
