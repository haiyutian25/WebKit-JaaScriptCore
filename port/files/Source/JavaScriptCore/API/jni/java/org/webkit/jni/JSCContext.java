/*
 * Copyright (C) 2026 The WebKit Android JSC port authors.
 *
 * JNI 桥接层 —— JS 上下文（包装 JSGlobalContextRef）。
 * 移植方案 §6.2 的 JSCJniContext 对应物（阶段 4.1 骨架 + 值转换）。
 */
package org.webkit.jni;

/**
 * JS 执行上下文。
 *
 * <p>值转换（阶段 4.1，规则顺序对齐 ObjC 版 {@code JSValue.mm}）：
 * <ul>
 *   <li>Java → JS：{@code null}→undefined、{@code Boolean}→bool、{@code Number}→number、
 *       {@code String}→string、{@code List}→数组、{@code Map}→对象</li>
 *   <li>JS → Java：undefined/null→{@code null}、bool→{@code Boolean}、number→{@code Double}、
 *       string→{@code String}、array→{@code ArrayList}、object→{@code HashMap}</li>
 * </ul>
 * 对象同一性（wrapper）与 Java 回调在 4.1 后续批次 / 4.2 提供。
 */
public final class JSCContext implements AutoCloseable {
    private long mHandle;
    // 保持虚拟机存活：Context 属于某个 ContextGroup，先释放组会导致悬挂。
    private JSCVirtualMachine mVirtualMachine;

    /** 在新建的独立虚拟机中创建上下文。 */
    public JSCContext() {
        this(null);
    }

    /** 在指定虚拟机中创建上下文。 */
    public JSCContext(JSCVirtualMachine virtualMachine) {
        mVirtualMachine = virtualMachine;
        mHandle = nativeCreate(virtualMachine != null ? virtualMachine.handle() : 0);
    }

    /** 求值一段 JS，返回值的 Java 表示（见类注释的转换规则）。抛 Java 异常表示 JS 侧异常。 */
    public Object evaluateScript(String code) {
        if (mHandle == 0)
            throw new IllegalStateException("JSCContext has been closed");
        return nativeEvaluateScript(mHandle, code);
    }

    /**
     * 求值一段 JS，并把 {@code args} 注入为全局 {@code __jscArgs[i]}（4.1 第二批次的参数通道）。
     *
     * <p>主要用途是验证 Java 对象 ↔ JS 对象的同一性：同一 Java 对象两次传入应得到同一 JS 对象，
     * 且该 JS 对象回传 Java 时应取回原对象（如 {@code evaluateScript("__jscArgs[0]", obj) == obj}）。
     * 正式的回调与参数编组在 4.2。
     */
    public Object evaluateScript(String code, Object... args) {
        if (mHandle == 0)
            throw new IllegalStateException("JSCContext has been closed");
        return nativeEvaluateScriptWithArguments(mHandle, code, args);
    }

    /** 触发一次全量 GC（等价于 JSGarbageCollect），并顺带清扫 wrapper 表。 */
    public void garbageCollect() {
        if (mHandle != 0)
            nativeGarbageCollect(mHandle);
    }

    /** 当前上下文里已登记的 Java 对象包装数量（泄漏与回收观测用）。 */
    public long wrapperCount() {
        return mHandle == 0 ? 0 : nativeWrapperCount(mHandle);
    }

    /** 全部上下文已登记的包装数量之和。 */
    public static long totalWrapperCount() {
        return nativeTotalWrapperCount();
    }

    /**
     * 显式清扫：把已被 JVM 回收的 Java 对象对应的包装移出表，并释放其 JS 侧保护。
     *
     * @return 清理掉的条目数
     */
    public long sweep() {
        return mHandle == 0 ? 0 : nativeSweepWrappers(mHandle);
    }

    /**
     * 撤销某 Java 对象包装的 JS 侧保护（{@code JSValueUnprotect}）。
     *
     * <p>默许路径下 Java → JS 的包装由桥接层用 {@code JSValueProtect} 保活（只要 Java 侧
     * 还持有它就不会被 JS GC 回收）。调用本方法后保护被撤销：若 JS 侧也没有引用，该包装
     * 将在随后一次 JS GC 时被回收（{@code JSAPIWrapperObject} cell 的 WeakSet 生命周期会把
     * "已回收"标记回传），包装表条目随之被 {@link #sweep()}（或 {@link #garbageCollect()}）
     * 摘除 —— 即包装生命周期由 JS GC 驱动，而非仅靠 JVM 侧判死。
     *
     * @return 是否找到该包装并撤销了保护（已撤销/已被回收时返回 {@code false}）
     */
    public boolean releaseWrapper(Object javaObject) {
        if (mHandle == 0 || javaObject == null)
            return false;
        return nativeReleaseWrapper(mHandle, javaObject);
    }

    /**
     * 注册**永久根**：让 {@code object} 在显式注销前不被 JVM 回收（与 JS 侧引用关系无关）。
     * 注册用强引用保活；注册与注销必须成对，否则是强引用泄漏。
     *
     * <p>若需要的是"owner 在 JS 侧可达时，其持有的 object 一并保活"（ObjC 的 external object
     * graph 语义），请用 {@link #addManagedReference(Object, Object)}。
     */
    public boolean addManagedReference(Object object) {
        if (mHandle == 0 || object == null)
            return false;
        return nativeAddManagedReference(object);
    }

    /** 注销永久根（与 {@link #addManagedReference(Object)} 成对）。 */
    public boolean removeManagedReference(Object object) {
        if (mHandle == 0 || object == null)
            return false;
        return nativeRemoveManagedReference(object);
    }

    /**
     * 登记「{@code owner} 持有 {@code object}」的外部对象图关系 —— 对齐 ObjC 的
     * {@code JSVirtualMachine.addManagedReference:withOwner:}。
     *
     * <p>语义：只要 {@code owner} 的 JS 包装在 JS 侧可达，{@code object} 的 JS 包装就一并保活
     * （即使 JS 侧没有直接引用它），从而保证"经由 owner 再次取回 object 时仍是同一个包装"。
     * 本方法**不新增任何 JNI 引用**；存活判定由 JS GC 沿本关系做 opaque root 传播完成。
     *
     * <p>与注销成对调用，否则关系表会常驻（不泄漏内存，但会持续保活）。
     */
    public boolean addManagedReference(Object object, Object owner) {
        if (mHandle == 0 || object == null || owner == null)
            return false;
        return nativeAddManagedReferenceWithOwner(mHandle, object, owner);
    }

    /** 注销外部对象图关系（与 {@link #addManagedReference(Object, Object)} 成对）。 */
    public boolean removeManagedReference(Object object, Object owner) {
        if (mHandle == 0 || object == null || owner == null)
            return false;
        return nativeRemoveManagedReferenceWithOwner(object, owner);
    }

    /** 当前进程内 managed reference（永久根）的总数。 */
    public static long managedReferenceCount() {
        return nativeManagedReferenceCount();
    }

    /** 当前进程内外部对象图的关系总数（观测：泄漏与回收）。 */
    public static long ownershipCount() {
        return nativeOwnershipCount();
    }

    /** 当前进程内已注册的导出类总数（跨 context；泄漏与回收观测用）。 */
    public static long registeredClassCount() {
        return nativeRegisteredClassCount();
    }

    /** 当前进程内存活的 {@link JSCJniWeakValue} 句柄数（跨 context；泄漏与回收观测用）。 */
    public static long liveWeakValueCount() {
        return nativeLiveWeakValueCount();
    }

    /**
     * 诊断：context 释放阶段（{@code nativeRelease} 内）出现的 wrapper finalize 次数。
     * 0 表示该阶段不触发 finalize（据此决定 holder 的删除策略）。
     */
    public static long finalizeDuringReleaseCount() {
        return nativeFinalizeDuringReleaseCount();
    }

    /**
     * 当前存活的 wrapper holder 数（累计分配 − 累计释放）。
     * context 释放后应回到基线 —— 这是"释放路径不泄漏"的直接证据。
     */
    public static long liveWrapperHolderCount() {
        return nativeLiveWrapperHolderCount();
    }

    /**
     * 把一个 Java 类注册为 JS 构造器（阶段 4.4 声明式导出）。
     *
     * <p>注册后 JS 侧可按类的 simple name 使用 {@code new ClassName()} 创建实例，并调用
     * {@link JSExport}/{@link JSExportAs} 标注过的实例方法；Java 的继承关系会映射为 JS 的原型链
     * （{@code Object.getPrototypeOf(B.prototype) === A.prototype}）。
     *
     * <p>本重载要求类有 public 无参构造器；需要带参数构造时用
     * {@link #registerClass(Class, JSCJniCallbackFunction)} 提供工厂。
     */
    public void registerClass(Class<?> javaClass) {
        registerClass(javaClass, null);
    }

    /**
     * 注册 Java 类，并指定构造工厂。
     *
     * @param javaClass 要导出的 Java 类
     * @param factory   可选：JS 侧 {@code new} 时调用，入参是 JS 实参转换成的 {@code Object[]}，
     *                  返回新建的 Java 实例（通常是 lambda 里 {@code new X(...)}）；
     *                  {@code null} 表示使用类的 public 无参构造器
     */
    public void registerClass(Class<?> javaClass, JSCJniCallbackFunction factory) {
        if (mHandle == 0)
            throw new IllegalStateException("JSCContext is closed");
        if (javaClass == null)
            throw new IllegalArgumentException("javaClass == null");

        // 注册时一次性反射扫描成「导出符号表」（方案 §6.3：声明即导出）。
        // 注解扫描放在 Java 侧：反射在此最直接，native 只负责绑定。四类成员：
        //   · 实例方法（@JSExport / @JSExportAs）→ prototype 上的函数
        //   · 实例属性（@JSProperty 的 getter/setter 方法对）→ 实例的访问器
        //   · 静态方法（@JSExport / @JSExportAs 的 static 方法）→ 构造器上的函数（子类沿构造器原型链继承）
        //   · 静态属性（@JSProperty 的 static 方法对）与静态字段（@JSExport 的 public static 字段）
        //     → 构造器上的访问器（final 字段只读）
        java.util.List<String> jsNames = new java.util.ArrayList<>();
        java.util.List<java.lang.reflect.Method> exportedMethods = new java.util.ArrayList<>();
        java.util.List<String> staticJsNames = new java.util.ArrayList<>();
        java.util.List<java.lang.reflect.Method> staticMethods = new java.util.ArrayList<>();
        for (java.lang.reflect.Method method : javaClass.getMethods()) {
            String exportedName = exportedNameOf(method, method.getName());
            if (exportedName == null)
                continue;
            if (java.lang.reflect.Modifier.isStatic(method.getModifiers())) {
                staticJsNames.add(exportedName);
                staticMethods.add(method);
            } else {
                jsNames.add(exportedName);
                exportedMethods.add(method);
            }
        }

        // 属性访问器（@JSProperty）：无参 → getter、单参 → setter，同名合并为同一属性；静态与实例分开收集
        java.util.LinkedHashMap<String, java.lang.reflect.Method> propertyGetters = new java.util.LinkedHashMap<>();
        java.util.LinkedHashMap<String, java.lang.reflect.Method> propertySetters = new java.util.LinkedHashMap<>();
        java.util.LinkedHashMap<String, java.lang.reflect.Method> staticPropertyGetters = new java.util.LinkedHashMap<>();
        java.util.LinkedHashMap<String, java.lang.reflect.Method> staticPropertySetters = new java.util.LinkedHashMap<>();
        for (java.lang.reflect.Method method : javaClass.getMethods()) {
            JSProperty property = method.getAnnotation(JSProperty.class);
            if (property == null)
                continue;
            String propertyName = property.value().isEmpty()
                ? derivePropertyName(method.getName())
                : property.value();
            boolean isStatic = java.lang.reflect.Modifier.isStatic(method.getModifiers());
            java.util.LinkedHashMap<String, java.lang.reflect.Method> targetGetters =
                isStatic ? staticPropertyGetters : propertyGetters;
            java.util.LinkedHashMap<String, java.lang.reflect.Method> targetSetters =
                isStatic ? staticPropertySetters : propertySetters;
            if (method.getParameterCount() == 0)
                targetGetters.put(propertyName, method);
            else if (method.getParameterCount() == 1)
                targetSetters.put(propertyName, method);
        }

        // 静态字段（@JSExport 的 public static 字段；final → 只读）
        java.util.List<String> staticFieldNames = new java.util.ArrayList<>();
        java.util.List<java.lang.reflect.Field> staticFields = new java.util.ArrayList<>();
        java.util.List<Boolean> staticFieldFinal = new java.util.ArrayList<>();
        for (java.lang.reflect.Field field : javaClass.getFields()) {
            if (!java.lang.reflect.Modifier.isStatic(field.getModifiers()))
                continue;
            String exportedName = exportedNameOf(field, field.getName());
            if (exportedName == null)
                continue;
            staticFieldNames.add(exportedName);
            staticFields.add(field);
            staticFieldFinal.add(java.lang.reflect.Modifier.isFinal(field.getModifiers()));
        }

        PropertyArrays instanceProperties = assemblePropertyArrays(propertyGetters, propertySetters);
        PropertyArrays staticProperties = assemblePropertyArrays(staticPropertyGetters, staticPropertySetters);
        boolean[] staticFieldFinalArray = new boolean[staticFieldFinal.size()];
        for (int i = 0; i < staticFieldFinalArray.length; i++)
            staticFieldFinalArray[i] = staticFieldFinal.get(i);

        nativeRegisterClass(mHandle, javaClass, javaClass.getSimpleName(), javaClass.getSuperclass(), factory,
            jsNames.toArray(new String[0]),
            exportedMethods.toArray(new java.lang.reflect.Method[0]),
            instanceProperties.names, instanceProperties.getters, instanceProperties.setters,
            instanceProperties.setterIsVoid,
            staticJsNames.toArray(new String[0]),
            staticMethods.toArray(new java.lang.reflect.Method[0]),
            staticProperties.names, staticProperties.getters, staticProperties.setters,
            staticProperties.setterIsVoid,
            staticFieldNames.toArray(new String[0]),
            staticFields.toArray(new java.lang.reflect.Field[0]),
            staticFieldFinalArray);
    }

    /** 从 @JSExportAs / @JSExport 取导出名；两者都无则返回 null（不导出）。 */
    private static String exportedNameOf(java.lang.reflect.AnnotatedElement member, String defaultName) {
        JSExportAs exportAs = member.getAnnotation(JSExportAs.class);
        if (exportAs != null)
            return exportAs.value();
        JSExport export = member.getAnnotation(JSExport.class);
        if (export == null)
            return null;
        return export.value().isEmpty() ? defaultName : export.value();
    }

    /** 属性名 + getter/setter 的平行数组（nativeRegisterClass 的参数形态）。 */
    private static final class PropertyArrays {
        String[] names;
        java.lang.reflect.Method[] getters;
        java.lang.reflect.Method[] setters;
        boolean[] setterIsVoid;
    }

    /** 把 getter/setter 映射装配为平行数组；同名 getter + setter 合并为同一属性。 */
    private static PropertyArrays assemblePropertyArrays(
        java.util.LinkedHashMap<String, java.lang.reflect.Method> gettersMap,
        java.util.LinkedHashMap<String, java.lang.reflect.Method> settersMap) {
        java.util.LinkedHashSet<String> allNames = new java.util.LinkedHashSet<>();
        allNames.addAll(gettersMap.keySet());
        allNames.addAll(settersMap.keySet());
        PropertyArrays arrays = new PropertyArrays();
        arrays.names = allNames.toArray(new String[0]);
        arrays.getters = new java.lang.reflect.Method[arrays.names.length];
        arrays.setters = new java.lang.reflect.Method[arrays.names.length];
        arrays.setterIsVoid = new boolean[arrays.names.length];
        int index = 0;
        for (String name : arrays.names) {
            arrays.getters[index] = gettersMap.get(name);
            arrays.setters[index] = settersMap.get(name);
            arrays.setterIsVoid[index] = arrays.setters[index] != null
                && arrays.setters[index].getReturnType() == void.class;
            index++;
        }
        return arrays;
    }

    /** 按 getXxx / setXxx / isXxx 推导属性名（首字母小写；getURL → URL）；非该前缀则用方法名本身。 */
    private static String derivePropertyName(String methodName) {
        String suffix = null;
        if (methodName.startsWith("get") && methodName.length() > 3)
            suffix = methodName.substring(3);
        else if (methodName.startsWith("set") && methodName.length() > 3)
            suffix = methodName.substring(3);
        else if (methodName.startsWith("is") && methodName.length() > 2)
            suffix = methodName.substring(2);
        if (suffix == null)
            return methodName;
        if (suffix.length() >= 2 && Character.isUpperCase(suffix.charAt(0)) && Character.isUpperCase(suffix.charAt(1)))
            return suffix;   // 连续大写（如 URL）保持原样
        return Character.toLowerCase(suffix.charAt(0)) + suffix.substring(1);
    }

    @Override
    public void close() {
        if (mHandle != 0) {
            nativeRelease(mHandle);
            mHandle = 0;
        }
        mVirtualMachine = null;
    }

    long handle() {
        return mHandle;
    }

    private static native long nativeCreate(long groupHandle);
    private static native void nativeRelease(long handle);
    private static native Object nativeEvaluateScript(long handle, String code);
    private static native Object nativeEvaluateScriptWithArguments(long handle, String code, Object[] arguments);
    private static native void nativeGarbageCollect(long handle);
    private static native long nativeSweepWrappers(long handle);
    private static native long nativeWrapperCount(long handle);
    private static native boolean nativeReleaseWrapper(long handle, Object object);
    private static native long nativeRegisteredClassCount();
    private static native long nativeLiveWeakValueCount();
    private static native long nativeFinalizeDuringReleaseCount();
    private static native long nativeLiveWrapperHolderCount();
    private static native long nativeTotalWrapperCount();
    private static native boolean nativeAddManagedReference(Object object);
    private static native boolean nativeRemoveManagedReference(Object object);
    private static native long nativeManagedReferenceCount();
    private static native boolean nativeAddManagedReferenceWithOwner(long handle, Object object, Object owner);
    private static native boolean nativeRemoveManagedReferenceWithOwner(Object object, Object owner);
    private static native long nativeOwnershipCount();
    private static native void nativeRegisterClass(long handle, Class<?> javaClass, String jsName,
        Class<?> superClass, JSCJniCallbackFunction factory, String[] methodNames,
        java.lang.reflect.Method[] methods, String[] propertyNames,
        java.lang.reflect.Method[] getters, java.lang.reflect.Method[] setters,
        boolean[] setterIsVoid,
        String[] staticMethodNames, java.lang.reflect.Method[] staticMethods,
        String[] staticPropertyNames, java.lang.reflect.Method[] staticGetters,
        java.lang.reflect.Method[] staticSetters, boolean[] staticSetterIsVoid,
        String[] staticFieldNames, java.lang.reflect.Field[] staticFields, boolean[] staticFieldFinal);
}
