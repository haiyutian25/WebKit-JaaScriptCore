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
 * 标注需要导出到 JS 的成员（实例方法 / 静态方法 / 静态字段）。
 *
 * <p>用法：
 * <pre>{@code
 * public class Point {
 *     @JSExport public static final Object ORIGIN = "origin";      // 静态字段 → Point.ORIGIN（final → 只读）
 *     @JSExport public static Object create() { ... }              // 静态方法 → Point.create()
 *
 *     public Point() {}
 *     @JSExport public Object sum(Object a, Object b) { ... }      // 实例方法 → new Point().sum(1, 2)
 *     @JSExport("length") public Object distance(Object other) { ... }   // 改名
 * }
 * context.registerClass(Point.class);
 * }</pre>
 *
 * <p><b>签名约定</b>：导出方法的参数与返回值都应是 {@code Object} —— JS 的实参按
 * {@link JSCContext} 的常规规则转成 Java 对象后逐个按引用传入，返回值按 Java → JS 规则转回。
 * 这样桥接层无需为每个方法解析泛型化的 JVM 签名（见方案 §6.6 的取舍说明）。
 *
 * <p><b>实例成员与静态成员</b>：实例方法挂在该类的 {@code prototype} 上；静态方法与静态字段挂在
 * JS 的构造器函数对象上（{@code ClassName.staticMethod(...)} / {@code ClassName.STATIC_FIELD}），
 * 并沿构造器原型链继承（子类可访问父类的静态成员）。静态字段经 {@code java.lang.reflect.Field}
 * 读写，运行期修改对 JS 可见；{@code final} 字段只读。
 *
 * <p>{@link #value()} 非空时等价于 {@link JSExportAs}。
 */
@Retention(RetentionPolicy.RUNTIME)
@Target({ElementType.METHOD, ElementType.FIELD})
public @interface JSExport {
    /** 导出到 JS 时使用的属性名；为空则用 Java 方法名。 */
    String value() default "";
}
