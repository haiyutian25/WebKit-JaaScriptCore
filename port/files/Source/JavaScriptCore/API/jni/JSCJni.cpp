/*
 * Copyright (C) 2025 Apple Inc. All rights reserved.
 * Copyright (C) 2026 The WebKit Android JSC port authors.
 *
 * JNI 桥接层 —— 阶段 4.1 骨架 + 值转换（移植方案 §6）
 *
 * 本文件提供最小可验证闭环：
 *   · JNI_OnLoad / JNI_OnUnload（JavaVM* 的唯一标准获取途径，是后续一切 Attach/GetEnv 的前提）
 *   · 线程附着（attachAndGetEnv：任何可能由 JSC 侧线程发起的 JNI 调用都要先附着）
 *   · Context 生命周期（JSGlobalContextRef 包装）
 *   · 值转换（Java → JS / JS → Java），规则顺序对齐 ObjC 版 JSValue.mm
 *   · evaluateScript
 *
 * 设计取舍（与方案 §6.3 对应）：
 *   · 值转换用"调用方持有的 JSGlobalContextRef 句柄 + 立即转换"的模型；
 *     wrapper（Java 对象 ↔ JS 对象同一性）与回调桥在 4.1 后续批次 / 4.2 补齐。
 *   · Java 对象引用：本批不做 NewGlobalRef 保活（无跨调用长生命周期对象）；
 *     引入 wrapper 时必须成对（NewGlobalRef + JSValueProtect），见方案 §6.6 第 1 条。
 *   · 所有 Java 调用返回后先 ExceptionCheck/ExceptionClear 再碰 JSC（方案 §6.6 第 3 条）。
 */

#include "config.h"

#if defined(JSC_JNI_API_ENABLED)

#include "APICast.h"
#include "JavaScript.h"
#include "JSContextRefPrivate.h"

#include <atomic>
#include <jni.h>
#include <wtf/Assertions.h>
#include <wtf/DataLog.h>
#include <wtf/FastMalloc.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/Vector.h>
#include <wtf/text/CString.h>
#include <wtf/text/WTFString.h>

// 阶段 4.4 补全（JSCJniWeakValue 与 cell 复用）需要 JSC 内部头：Weak<T> / WeakSet /
// WeakHandleOwner / JSLockHolder。用 JSCInlines.h 聚合头（与 GLib 的 JSCWeakValue 实现同一做法）。
// 注意：这些头只能在与 JSC 同库编译的 TU 里使用 —— JSCJni.cpp 正是（JNI.cmake 登记）。
#include "AbstractSlotVisitor.h"
#include "JSCInlines.h"
#include "JSCallbackObject.h"    // JSCallbackObject<JSAPIWrapperObject>（cell 复用）
#include "JSAPIWrapperObject.h"  // 同上
#include "JSLock.h"              // JSLockHolder（WebKit 无独立的 JSLockHolder.h）
#include "Structure.h"
#include "Weak.h"
#include "WeakHandleOwner.h"
#include "WeakSet.h"
#include "Collector.h"           // Collector::collectorSlotVisitor（"老/新对象"判定）
#include "Heap.h"                // VM::heap（判定 + 同步 Full GC）
#include <wtf/HashSet.h>

// ---------------------------------------------------------------------------
// JVM 与线程附着
// ---------------------------------------------------------------------------

static JavaVM* g_jvm { nullptr };

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* /*reserved*/)
{
    g_jvm = vm;
    return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT void JNICALL JNI_OnUnload(JavaVM* /*vm*/, void* /*reserved*/)
{
    g_jvm = nullptr;
}

// 取当前线程的 JNIEnv；非 Java 线程则附着（由 JSC 侧线程发起的 JNI 调用走这条路径）。
// 注意：JNIEnv* 不可跨线程复用，也不可存成全局。
static JNIEnv* attachAndGetEnv()
{
    if (!g_jvm)
        return nullptr;
    JNIEnv* env = nullptr;
    if (g_jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK)
        return env;
    if (g_jvm->AttachCurrentThread(&env, nullptr) == JNI_OK)
        return env;
    return nullptr;
}

// ---------------------------------------------------------------------------
// 常用 Java 类的缓存（用 FindClass 后转 GlobalRef）
//   FindClass 在 Java 线程的 native 方法里可靠；因此缓存时机放在 native 调用中，
//   而不是 JNI_OnLoad（那时 org.webkit.jni.* 可能尚未加载）。
// ---------------------------------------------------------------------------

namespace {

struct JavaClasses {
    jclass booleanClass { nullptr };
    jclass numberClass { nullptr };
    jclass stringClass { nullptr };
    // 注意：**接口与具体类必须分开缓存**。cachedClass 是「首次查询即定值」的槽位，
    // 若接口（java/util/List）与实现类（java/util/ArrayList）共用同一槽位，先到者会
    // 污染后来的用法 —— 典型后果是 java/util/HashMap 的 "<init>" 在接口 java/util/Map
    // 上解析为 null，随后 NewObject(…, nullptr) 触发 CheckJNI abort("mid == null")。
    jclass listClass { nullptr };           // java/util/ArrayList —— JS→Java 构造用
    jclass listInterfaceClass { nullptr };  // java/util/List —— Java→JS 判定/取值用
    jclass mapClass { nullptr };            // java/util/HashMap —— JS→Java 构造用
    jclass mapInterfaceClass { nullptr };   // java/util/Map —— Java→JS 判定/取值用
    jclass doubleClass { nullptr };
    jclass objectClass { nullptr };
    jclass fieldClass { nullptr };    // java/lang/reflect/Field（静态字段读写用）
};

JavaClasses& javaClasses()
{
    static NeverDestroyed<JavaClasses> classes;
    return classes.get();
}

jclass cachedClass(JNIEnv* env, const char* name, jclass& cacheSlot)
{
    if (!cacheSlot) {
        jclass local = env->FindClass(name);
        if (local) {
            cacheSlot = static_cast<jclass>(env->NewGlobalRef(local));
            env->DeleteLocalRef(local);
        }
    }
    return cacheSlot;
}

bool isInstanceOf(JNIEnv* env, jobject object, jclass clazz)
{
    return clazz && env->IsInstanceOf(object, clazz);
}

// Java 侧抛异常后：打印并清除，避免污染后续 JNI 调用。
// （方案 §6.6 第 3 条：任何 Java 调用返回后先 ExceptionCheck + ExceptionClear 再碰 JSC。）
void clearJavaException(JNIEnv* env, const char* where)
{
    if (!env->ExceptionCheck())
        return;
    if (env->ExceptionOccurred())
        env->ExceptionDescribe();
    env->ExceptionClear();
    dataLogLn("JSC JNI: cleared pending Java exception in ", where);
}

} // namespace

// ---------------------------------------------------------------------------
// wrapper：Java 对象 ↔ JS 对象同一性 + 回收
//（4.1 建立，4.3 弱引用模型，4.4 补全改造为 JSAPIWrapperObject cell 复用）
//
// 模型：双向表 + holder + JSAPIWrapperObject cell + sweep
//   · JS 侧对象现在是 **JSCallbackObject<JSAPIWrapperObject>**（真实 JSC cell，实现见
//     API/jni/JSAPIWrapperObjectJni.cpp），不再是 JSObjectMake 的匿名普通对象：
//       - cell 带 wrappedObject 槽（存 JniWrapperHolder*）
//       - finishCreation 里 WeakSet::allocate → **JS GC 能通知其死亡**
//       - owner.finalize（GC 内部）只做：holder->jsCollected = true + 归还 weak slot
//         （无锁、不做 JNI）
//   · Java 侧持 NewWeakGlobalRef（在 holder 里）—— 不阻止 JVM GC
//   · JS 侧用 JSValueProtect 保护（Java 持有期间不被回收）；releaseWrapperProtection()
//     撤销保护后，JS 侧无引用时 GC 即可回收它 → finalize 置 jsCollected → sweep 摘表
//   · sweep 的死因有两个，都会摘表：
//       a) JVM 已回收（jweak 满足 IsSameObject(weak, nullptr)，JNI 规范语义）
//       b) JS 侧已回收（holder->jsCollected，由 JS GC 驱动 —— cell 复用的新增能力）
//   · 释放顺序恒为：先 JSValueUnprotect（仅当未被 JS 回收时）再释放 holder（内含
//     DeleteWeakGlobalRef），且都在表锁之外执行（§6.6 第 1 条，避免与 apiLock 锁序倒置）
//
// 语义降级（§6.6 第 5 条）：JSC 无法扫描 JVM 堆，ObjC 的 "external object graph" 不可复现
//（详见 JSAPIWrapperObjectJni.cpp 头部）；需要跨 GC 存活的 Java 对象须显式注册 managed reference。
// ---------------------------------------------------------------------------

// 由 JSAPIWrapperObjectJni.cpp 提供：复用 JSAPIWrapperObject cell 创建包装对象
//（jsClass 为空用通用 wrapper class；privateData 经 JSObjectGetPrivate 供 JSClass 访问器取用）。
// 该入口内部会取 apiLock —— cell 分配必须在锁内。
extern "C" JSObjectRef JSCJniCreateAPIWrapperObjectWithClass(JSContextRef context, void* wrappedObject,
    JSClassRef jsClass, void* privateData);

namespace {

// JS GC 回收包装对象（cell 析构）时由 JSClass.finalize 调用：只做无锁记录。
void recordCollectedWrapper(JSObjectRef object);

// --- JS 侧回收记录（无锁环缓冲）---------------------------------------------
// 用途：导出实例的 instanceClass 挂了 JSClass.finalize，在 GC 析构链上被调用 —— 这是
// "JS 侧已回收"的第二条通道（第一条是 JSAPIWrapperObject cell 的 WeakHandleOwner::finalize）。
// 记录只写无锁环缓冲，在安全点（sweep / GC 入口）排空并回标包装表。
constexpr size_t collectedWrapperCapacity = 8192;
std::atomic<JSObjectRef> collectedWrapperRing[collectedWrapperCapacity];
std::atomic<size_t> collectedWrapperWriteIndex { 0 };
std::atomic<size_t> collectedWrapperReadIndex { 0 };

void recordCollectedWrapper(JSObjectRef object)
{
    size_t index = collectedWrapperWriteIndex.fetch_add(1, std::memory_order_relaxed);
    collectedWrapperRing[index % collectedWrapperCapacity].store(object, std::memory_order_release);
}

// 注：drainCollectedWrappers（消费上述记录）定义在包装表之后（需要 wrapperLock/wrapperTables）。

} // namespace

namespace {

// holder：cell 与表之间的共享状态。
//   · cell 的 wrappedObject 槽存本指针；JS GC 经 owner.finalize 置 jsCollected
//   · 表项持有本指针并负责（与 cell 一起）最终释放
// refCount 的两个持有者：表（摘除时释放）与 cell（finalize 时释放）；归零即 delete。
// 两个释放路径都保证 jweak 已先释放（表摘除时释放；finalize 释放意味着表已摘除），
// 因此归零点的 delete 不需要 JNI 环境。
struct JniClassData;   // 前向声明（导出类元数据，定义见下文）

struct JniWrapperHolder {
    std::atomic<int> refCount { 2 };         // 1 = 表项，1 = cell（finalize 时释放）
    jobject javaRef { nullptr };             // 目标 Java 对象（**强全局引用**，对齐 ObjC 的 [id retain]）
    uint32_t identityHash { 0 };             // System.identityHashCode（分桶键 + opaque root 身份）
    JniClassData* classData { nullptr };     // 非空 = 该包装是导出类的实例（供属性访问器取元数据）
    std::atomic<bool> jsCollected { false }; // JS 侧已回收（finalize 记录 → 安全点回标）
};

struct JavaWrapper {
    JniWrapperHolder* holder { nullptr };
    // JS 侧对象：**不再 JSValueProtect**。存活完全由「JS 侧可达性 + 外部对象图的 opaque root 传播」
    // 决定（见 JSAPIWrapperObjectJni.cpp），与原版 ObjC 一致。
    JSObjectRef jsObject { nullptr };
};

// 表 + 两个索引（下标指向 entries）。
// 引入索引的原因：10 万次往返压测中，纯线性查找（每次遍历全表做 IsSameObject）无法完成 ——
// 分桶后正查与反查都降到均摊 O(1)。
// 注意：sweep 会重建 entries，下标随之失效，因此必须整体重建索引（rebuildIndex）。
struct WrapperTable {
    Vector<JavaWrapper> entries;
    HashMap<uint32_t, Vector<size_t>> byIdentityHash;   // identityHash → entries 下标
    HashMap<JSObjectRef, size_t> byJSObject;            // JS 对象 → entries 下标

    void rebuildIndex()
    {
        byIdentityHash.clear();
        byJSObject.clear();
        for (size_t i = 0; i < entries.size(); ++i) {
            byIdentityHash.ensure(entries[i].holder->identityHash, [] { return Vector<size_t> { }; }).iterator->value.append(i);
            byJSObject.set(entries[i].jsObject, i);
        }
    }
};

// holder 分配/释放计数（泄漏观测；context 释放路径的正确性由此证明）
std::atomic<long> g_holderAllocCount { 0 };
std::atomic<long> g_holderFreeCount { 0 };

JniWrapperHolder* allocateWrapperHolder()
{
    g_holderAllocCount.fetch_add(1, std::memory_order_relaxed);
    return new JniWrapperHolder;
}

void freeWrapperHolder(JniWrapperHolder* holder)
{
    if (!holder)
        return;
    g_holderFreeCount.fetch_add(1, std::memory_order_relaxed);
    delete holder;
}

// 释放 holder 的一个引用；归零则删除。
// 约定：**表摘除路径**（有 JNI 环境的安全点）先释放 jweak，再调用本函数；因此归零点
// （可能位于 GC 线程的 finalize 路径）不再需要 JNI 环境 —— 只做一次 delete。
void releaseWrapperHolderRef(JniWrapperHolder* holder)
{
    if (!holder)
        return;
    if (holder->refCount.fetch_sub(1, std::memory_order_acq_rel) == 1)
        freeWrapperHolder(holder);
}

// System.identityHashCode(obj)：与对象身份绑定的稳定哈希，不受 hashCode 覆写影响。
uint32_t identityHashOf(JNIEnv* env, jobject object)
{
    static jclass systemClass = nullptr;   // 进程内全局缓存（JNI class 引用本身是全局的）
    if (!systemClass) {
        jclass local = env->FindClass("java/lang/System");
        if (!local)
            return 0;
        systemClass = static_cast<jclass>(env->NewGlobalRef(local));
        env->DeleteLocalRef(local);
    }
    jmethodID method = env->GetStaticMethodID(systemClass, "identityHashCode", "(Ljava/lang/Object;)I");
    if (!method) {
        if (env->ExceptionCheck())
            env->ExceptionClear();
        return 0;
    }
    jint result = env->CallStaticIntMethod(systemClass, method, object);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return 0;
    }
    return static_cast<uint32_t>(result);
}

Lock& wrapperLock()
{
    static NeverDestroyed<Lock> lock;
    return lock.get();
}

HashMap<JSContextRef, WrapperTable>& wrapperTables()
{
    static NeverDestroyed<HashMap<JSContextRef, WrapperTable>> tables;
    return tables.get();
}

// 排空回收记录：把匹配的表项标记为 jsCollected（由随后的 sweep 摘除）。
// 必须在安全点调用（可拿表锁；不解引用 JSObjectRef —— 仅作指针值比对）。
size_t drainCollectedWrappers()
{
    size_t readIndex = collectedWrapperReadIndex.load(std::memory_order_relaxed);
    size_t writeIndex = collectedWrapperWriteIndex.load(std::memory_order_acquire);
    if (readIndex == writeIndex)
        return 0;
    if (writeIndex - readIndex > collectedWrapperCapacity)
        readIndex = writeIndex - collectedWrapperCapacity;   // 记录溢出：丢弃最旧（表项最终仍会因 JVM 侧判死被摘除）

    Vector<JSObjectRef> collected;
    collected.reserveInitialCapacity(static_cast<size_t>(writeIndex - readIndex));
    for (size_t i = readIndex; i < writeIndex; ++i)
        collected.append(collectedWrapperRing[i % collectedWrapperCapacity].load(std::memory_order_acquire));
    collectedWrapperReadIndex.store(writeIndex, std::memory_order_release);

    size_t marked = 0;
    Locker locker { wrapperLock() };
    for (auto object : collected) {
        for (auto& tableEntry : wrapperTables()) {
            auto& table = tableEntry.value;
            auto it = table.byJSObject.find(object);
            if (it == table.byJSObject.end())
                continue;
            auto& entry = table.entries[it->value];
            if (!entry.holder->jsCollected.load(std::memory_order_acquire)) {
                entry.holder->jsCollected.store(true, std::memory_order_release);
                marked++;
            }
            break;
        }
    }
    return marked;
}

// 清扫单个 context：把已回收（JS 侧）的条目移出表，并在锁外释放 Java 强引用与 holder。
// 唯一死因：JS GC 已回收 wrapper（holder->jsCollected）—— 持有关系为强引用后，
// Java 对象不再"自己死"（对齐 ObjC 的 [id retain]）。
size_t sweepWrappers(JNIEnv* env, JSContextRef context)
{
    // 先把 finalize 记录的"JS 侧已回收"回标到表（holder->jsCollected），再统一摘除。
    drainCollectedWrappers();

    Vector<JavaWrapper> dead;
    {
        Locker locker { wrapperLock() };
        auto it = wrapperTables().find(context);
        if (it == wrapperTables().end())
            return 0;

        Vector<JavaWrapper> survivors;
        for (auto& entry : it->value.entries) {
            // 唯一摘除条件：JS 侧已回收（holder->jsCollected）。
            // Java 对象不再会"自己死" —— 它由 holder 强引用保活（对齐 ObjC 的 [id retain]）。
            if (entry.holder->jsCollected.load(std::memory_order_acquire))
                dead.append(entry);
            else
                survivors.append(entry);
        }
        it->value.entries = WTF::move(survivors);
        it->value.rebuildIndex();   // entries 下标已变，索引必须整体重建
    }

    // 锁外释放：DeleteGlobalRef 需要 JNI 环境（此处是安全点；finalize 路径没有）。
    for (auto& entry : dead) {
        JniWrapperHolder* holder = entry.holder;
        if (holder->javaRef) {
            env->DeleteGlobalRef(holder->javaRef);
            holder->javaRef = nullptr;
        }
        // 表放弃对 holder 的引用；cell 侧已在 finalize 释放，归零即删除。
        releaseWrapperHolderRef(holder);
    }
    return dead.size();
}

// 清扫所有 context 的表（nativeGarbageCollect 与显式 sweep 入口使用）。
size_t sweepAllWrappers(JNIEnv* env)
{
    Vector<JSContextRef> contexts;
    {
        Locker locker { wrapperLock() };
        for (auto& entry : wrapperTables())
            contexts.append(entry.key);
    }
    size_t total = 0;
    for (auto context : contexts)
        total += sweepWrappers(env, context);
    return total;
}

// 查：同一 Java 对象（IsSameObject 语义）已登记的 JS wrapper。已死条目返回 nullptr。
// 先按 identityHash 分桶，再在桶内用 IsSameObject 精确判定（不同对象可能哈希碰撞）。
JSObjectRef lookupJSWrapper(JNIEnv* env, JSContextRef context, jobject javaObject)
{
    uint32_t hash = identityHashOf(env, javaObject);
    Locker locker { wrapperLock() };
    auto it = wrapperTables().find(context);
    if (it == wrapperTables().end())
        return nullptr;
    auto& table = it->value;
    auto bucketIt = table.byIdentityHash.find(hash);
    if (bucketIt == table.byIdentityHash.end())
        return nullptr;
    for (size_t index : bucketIt->value) {
        auto& entry = table.entries[index];
        if (entry.holder->jsCollected.load(std::memory_order_acquire))
            continue;
        if (!entry.holder->javaRef)
            continue;   // 已被显式解绑（releaseWrapper）
        if (env->IsSameObject(entry.holder->javaRef, javaObject))
            return entry.jsObject;
    }
    return nullptr;
}

// 建：为 Java 对象创建 JS wrapper 并登记（cell + holder）。
// jsClass 非空时用于导出实例（带 @JSProperty 访问器）；为空时用通用 wrapper class。
// 语义对齐原版：holder **强持有** Java 对象（= ObjC 的 [id retain]），wrapper 不用 JSValueProtect
// —— JS 不可达时由 cell 的 finalize 标记，安全点由 sweep 释放强引用。
JSObjectRef createWrapperWithClass(JNIEnv* env, JSContextRef context, jobject javaObject,
    JSClassRef jsClass, void* privateData)
{
    uint32_t hash = identityHashOf(env, javaObject);
    jobject globalRef = env->NewGlobalRef(javaObject);
    if (!globalRef)
        return nullptr;

    auto* holder = allocateWrapperHolder();
    holder->javaRef = globalRef;
    holder->identityHash = hash;
    holder->classData = static_cast<JniClassData*>(privateData);   // 导出实例的类元数据（普通 wrapper 为 null）

    // 包装对象 = JSAPIWrapperObject cell（复用 JSC 的 API wrapper cell 类）：
    // 带 WeakSet 生命周期 —— JS GC 回收该 cell 时 owner.finalize 置 holder->jsCollected。
    // privateData 经 JSObjectGetPrivate 供 JSClass 访问器取用（导出实例传 JniClassData*）。
    JSObjectRef jsObject = JSCJniCreateAPIWrapperObjectWithClass(context, holder, jsClass, privateData);
    if (!jsObject) {
        freeWrapperHolder(holder);   // cell 未创建：wrappedObject 未被接管
        env->DeleteGlobalRef(globalRef);
        return nullptr;
    }

    Locker locker { wrapperLock() };
    auto result = wrapperTables().ensure(context, [] { return WrapperTable { }; });
    auto& table = result.iterator->value;
    size_t index = table.entries.size();
    table.entries.append(JavaWrapper { holder, jsObject });
    table.byIdentityHash.ensure(hash, [] { return Vector<size_t> { }; }).iterator->value.append(index);
    table.byJSObject.set(jsObject, index);
    return jsObject;
}

JSObjectRef createJSWrapper(JNIEnv* env, JSContextRef context, jobject javaObject)
{
    return createWrapperWithClass(env, context, javaObject, nullptr, nullptr);
}

// 显式解绑某 Java 对象的包装：摘除表项、释放 Java 强引用。
// （原实现用于撤销 JSValueProtect；现已无保护可撤 —— wrapper 本就随 JS 可达性回收，
//   本 API 现在的意义是"主动断开 holder 对 Java 对象的强引用"。）
// holder 的释放仍走 refCount（表侧 −1；cell 侧由 finalize −1），因此不会出现悬空。
bool releaseWrapperProtection(JNIEnv* env, JSContextRef context, jobject javaObject)
{
    JniWrapperHolder* holderToRelease = nullptr;
    {
        Locker locker { wrapperLock() };
        auto it = wrapperTables().find(context);
        if (it == wrapperTables().end())
            return false;
        auto& table = it->value;
        for (size_t i = 0; i < table.entries.size(); ++i) {
            auto& entry = table.entries[i];
            if (entry.holder->jsCollected.load(std::memory_order_acquire) || !entry.holder->javaRef)
                continue;
            if (env->IsSameObject(entry.holder->javaRef, javaObject)) {
                holderToRelease = entry.holder;
                table.entries.removeAt(i);
                table.rebuildIndex();
                break;
            }
        }
    }
    if (!holderToRelease)
        return false;
    if (holderToRelease->javaRef) {
        env->DeleteGlobalRef(holderToRelease->javaRef);
        holderToRelease->javaRef = nullptr;
    }
    releaseWrapperHolderRef(holderToRelease);
    return true;
}

// 反查：JS 对象对应的 Java 对象；返回 local ref，调用方负责 DeleteLocalRef。
// Java 对象已被回收、或 JS 侧包装已被回收时返回 nullptr（调用方回退为普通对象转换）。
// 走 byJSObject 索引，均摊 O(1)。
jobject lookupJavaWrapper(JNIEnv* env, JSContextRef context, JSObjectRef jsObject)
{
    Locker locker { wrapperLock() };
    auto it = wrapperTables().find(context);
    if (it == wrapperTables().end())
        return nullptr;
    auto& table = it->value;
    auto indexIt = table.byJSObject.find(jsObject);
    if (indexIt == table.byJSObject.end())
        return nullptr;
    auto& entry = table.entries[indexIt->value];
    if (entry.holder->jsCollected.load(std::memory_order_acquire))
        return nullptr;
    if (!entry.holder->javaRef) {
        return nullptr;
    }
    return env->NewLocalRef(entry.holder->javaRef);
}

size_t wrapperCount(JSContextRef context)
{
    Locker locker { wrapperLock() };
    auto it = wrapperTables().find(context);
    return it == wrapperTables().end() ? 0 : it->value.entries.size();
}

size_t totalWrapperCount()
{
    Locker locker { wrapperLock() };
    size_t total = 0;
    for (auto& entry : wrapperTables())
        total += entry.value.entries.size();
    return total;
}

// ---------------------------------------------------------------------------
// managed references 与 external object graph
//
// 1) **永久根**（`addManagedReference(object)`，单参）：
//    与 owner 无关，直接 NewGlobalRef 保活，注销时释放（成对，避免泄漏）。
//
// 2) **外部对象图**（`addManagedReference(object, owner)`）—— 对齐 ObjC 的
//    `JSVirtualMachineExternalObjectGraph`（`API/JSVirtualMachine.mm:90/166/317`）：
//    只登记「owner 持有 object」这一关系，**不新增任何 JNI 引用**。真正的存活判定由
//    wrapper 侧的 opaque root 传播完成（见 `JSAPIWrapperObjectJni.cpp`）：
//      · wrapper 的 `visitChildrenImpl` 沿本图遍历，把每个到达者的身份登记为 GC 的 opaque root；
//      · wrapper 的 `isReachableFromOpaqueRoots` 据此判定是否存活。
//    ⇒ 「JS 可达的 owner ⇒ 其持有的 object 的 wrapper 一并保活」，与原版语义一致。
//
// 身份 key 用 System.identityHashCode（与 wrapper 表同源）。哈希碰撞只会导致**保守保活**
//（该释放的没释放），不会误释放 —— 安全性方向正确。
// ---------------------------------------------------------------------------

struct ManagedReference {
    jobject javaGlobalRef { nullptr };
    jweak javaWeakRef { nullptr };   // 仅用于同一性比对
};

Vector<ManagedReference>& managedReferences()
{
    static NeverDestroyed<Vector<ManagedReference>> refs;
    return refs.get();
}

bool addManagedReference(JNIEnv* env, jobject object)
{
    jobject globalRef = env->NewGlobalRef(object);
    if (!globalRef)
        return false;
    jweak weakRef = env->NewWeakGlobalRef(object);
    if (!weakRef) {
        env->DeleteGlobalRef(globalRef);
        return false;
    }
    Locker locker { wrapperLock() };
    managedReferences().append(ManagedReference { globalRef, weakRef });
    return true;
}

bool removeManagedReference(JNIEnv* env, jobject object)
{
    jobject globalRefToDelete = nullptr;
    jweak weakRefToDelete = nullptr;
    {
        Locker locker { wrapperLock() };
        auto& refs = managedReferences();
        for (size_t i = 0; i < refs.size(); ++i) {
            if (env->IsSameObject(refs[i].javaWeakRef, object)) {
                globalRefToDelete = refs[i].javaGlobalRef;
                weakRefToDelete = refs[i].javaWeakRef;
                refs.removeAt(i);
                break;
            }
        }
    }
    if (globalRefToDelete)
        env->DeleteGlobalRef(globalRefToDelete);
    if (weakRefToDelete)
        env->DeleteWeakGlobalRef(weakRefToDelete);
    return globalRefToDelete != nullptr;
}

size_t managedReferenceCount()
{
    Locker locker { wrapperLock() };
    return managedReferences().size();
}

// --- external object graph（owner → owned）-------------------------------
// 只登记持有关系；native 对象的实际存活由 wrapper 的 opaque root 传播决定。
// 锁的持有时长必须极短：本锁会在 GC 期间被 visitChildren 侧获取（与原版同风险）。
Lock& externalObjectGraphLock()
{
    static NeverDestroyed<Lock> lock;
    return lock.get();
}

HashMap<uint32_t, Vector<uint32_t>>& externalObjectGraph()
{
    static NeverDestroyed<HashMap<uint32_t, Vector<uint32_t>>> graph;
    return graph.get();
}

// external remembered set（分代 GC 的补扫）—— 对齐原版的 m_externalRememberedSet。
// 为什么需要：分代（Eden）GC 只扫"新 → 老"引用。若 owner 的包装已是**老对象**，
// 它在 Eden GC 中不会被 visitChildren → 新登记的「老 owner → 新 object」这条边
// 永远不会把 object 登记为 opaque root → object 的包装被误回收。
// 原版做法：登记关系时若「owner 老 && object 新」就把 owner 记入 remembered set，
// 由 GC 标记约束补扫（Heap.cpp 的 ExternalRememberedSet 阶段）。
HashSet<uint32_t>& externalRememberedSet()
{
    static NeverDestroyed<HashSet<uint32_t>> set;
    return set.get();
}

// 「老对象」= 已在 opaque root 集合中（此前某轮 GC 被登记过）。对齐原版 isOldExternalObject。
bool isOldExternalObject(JSC::VM& vm, uint32_t hash)
{
    if (!hash)
        return false;
    return vm.heap.collectorSlotVisitor().containsOpaqueRoot(
        reinterpret_cast<void*>(static_cast<uintptr_t>(hash)));
}

void addOwnership(JNIEnv* env, JSC::VM& vm, jobject object, jobject owner)
{
    uint32_t objectHash = identityHashOf(env, object);
    uint32_t ownerHash = identityHashOf(env, owner);
    if (!objectHash || !ownerHash)
        return;

    bool needsRemembrance = isOldExternalObject(vm, ownerHash) && !isOldExternalObject(vm, objectHash);

    Locker locker { externalObjectGraphLock() };
    if (needsRemembrance)
        externalRememberedSet().add(ownerHash);
    auto& owned = externalObjectGraph().ensure(ownerHash, [] { return Vector<uint32_t> { }; }).iterator->value;
    if (!owned.contains(objectHash))
        owned.append(objectHash);
}

bool removeOwnership(JNIEnv* env, jobject object, jobject owner)
{
    uint32_t objectHash = identityHashOf(env, object);
    uint32_t ownerHash = identityHashOf(env, owner);
    if (!objectHash || !ownerHash)
        return false;
    Locker locker { externalObjectGraphLock() };
    auto it = externalObjectGraph().find(ownerHash);
    if (it == externalObjectGraph().end())
        return false;
    auto& owned = it->value;
    for (size_t i = 0; i < owned.size(); ++i) {
        if (owned[i] == objectHash) {
            owned.removeAt(i);
            if (owned.isEmpty())
                externalObjectGraph().remove(it);
            return true;
        }
    }
    return false;
}

size_t ownershipCount()
{
    Locker locker { externalObjectGraphLock() };
    size_t total = 0;
    for (auto& entry : externalObjectGraph())
        total += entry.value.size();
    return total;
}

} // namespace

// ---------------------------------------------------------------------------
// 跨 TU 接口（供 JSAPIWrapperObjectJni.cpp 的 GC 回调使用）：
//   · wrappedObject（= JniWrapperHolder*）→ 其 Java 对象身份哈希（无 JNI、无分配）
//   · 取某 owner 身份哈希对应的 owned 列表（调用方负责 fastFree）
// ---------------------------------------------------------------------------

extern "C" uint32_t JSCJniWrapperHolderIdentityHash(void* wrappedObject)
{
    auto* holder = static_cast<JniWrapperHolder*>(wrappedObject);
    return holder ? holder->identityHash : 0;
}

extern "C" uint32_t* JSCJniCopyOwnedHashes(uint32_t ownerHash, size_t* outCount)
{
    if (outCount)
        *outCount = 0;
    if (!ownerHash)
        return nullptr;
    Locker locker { externalObjectGraphLock() };
    auto it = externalObjectGraph().find(ownerHash);
    if (it == externalObjectGraph().end() || it->value.isEmpty())
        return nullptr;
    size_t count = it->value.size();
    auto* buffer = static_cast<uint32_t*>(fastMalloc(count * sizeof(uint32_t)));
    for (size_t i = 0; i < count; ++i)
        buffer[i] = it->value[i];
    if (outCount)
        *outCount = count;
    return buffer;
}

// 从某个身份哈希出发，沿 owner → owned 关系做 BFS，把每个到达者登记为 opaque root。
// 对齐原版 `scanExternalObjectGraph`（`JSVirtualMachine.mm:317-347`）：以 addOpaqueRoot
// 的返回值去重、每节点取一次短锁。**不做任何 JNI** —— GC 期间唯一安全的形式。
// `wrappedObject` 起点由 visitChildren 提供；remembered set 补扫时由 owned 起点提供。
extern "C" void JSCJniScanGraphFrom(JSC::AbstractSlotVisitor& visitor, uint32_t rootHash)
{
    if (!rootHash)
        return;
    Vector<uint32_t> stack;
    stack.append(rootHash);
    while (!stack.isEmpty()) {
        uint32_t hash = stack.takeLast();
        if (!visitor.addOpaqueRoot(reinterpret_cast<void*>(static_cast<uintptr_t>(hash))))
            continue;   // 已访问过（本轮已登记为 opaque root）
        size_t count = 0;
        uint32_t* owned = JSCJniCopyOwnedHashes(hash, &count);
        if (!owned)
            continue;
        for (size_t i = 0; i < count; ++i) {
            if (owned[i])
                stack.append(owned[i]);
        }
        fastFree(owned);
    }
}

// 分代 GC 的补扫（对齐原版 `scanExternalRememberedSet`，`JSVirtualMachine.mm:355-373`）：
// 对 remembered set 里的每个 owner，从其**每个 owned 对象**出发扫描图（原版同此 —— 起点是
// ownedObject 而非 owner），随后清空集合（每轮重建）。
// 由 Heap.cpp 的标记约束在 GC 内调用。
extern "C" void JSCJniScanRememberedSet(JSC::VM&, JSC::AbstractSlotVisitor& visitor)
{
    Vector<uint32_t> owners;
    {
        Locker locker { externalObjectGraphLock() };
        if (externalRememberedSet().isEmpty())
            return;
        for (uint32_t ownerHash : externalRememberedSet())
            owners.append(ownerHash);
        externalRememberedSet().clear();   // 对齐原版的 removeAllObjects（每轮重建）
    }
    for (uint32_t ownerHash : owners) {
        size_t count = 0;
        uint32_t* owned = JSCJniCopyOwnedHashes(ownerHash, &count);
        if (!owned)
            continue;
        for (size_t i = 0; i < count; ++i) {
            if (owned[i])
                JSCJniScanGraphFrom(visitor, owned[i]);
        }
        fastFree(owned);
    }
}

// ---------------------------------------------------------------------------
// 值转换：Java → JS
//   规则顺序对齐 ObjC 版 JSValue.mm：null→undefined、Boolean→bool、数值→number、
//   String→string、List→JS 数组（work-list 防环）、Map→对象、其余→wrapper（本批：对象→空对象）。
// ---------------------------------------------------------------------------

// 回调桥（阶段 4.2）的实现位于本文件后半；值转换需要先引用它们。
jclass javaCallbackInterfaceClass(JNIEnv* env);
JSValueRef createJSFunctionFromCallback(JNIEnv* env, JSContextRef context, jobject callback);
JSValueRef makeJSError(JSContextRef context, const char* message);

// 导出类（阶段 4.4 补全）的实现位于文件后半；值转换需要先引用它。
// 目的：**已注册导出类的** Java 对象在**任何** Java→JS 边界（工厂返回值、方法返回值、
// 参数转换）都包装成带 JSClass 的实例 —— 原型链方法与 @JSProperty 访问器同时可用，
// 而不是只得到一个通用 wrapper（这修掉了"工厂构造的实例不能与 @JSProperty 组合"的限制）。
// 注：声明处**不能**出现 JniClassData —— 它是匿名命名空间内的类型（无链接），
// Clang 会拒绝"类型无链接的返回/参数"的前向声明（cannot be defined in any other
// translation unit because its type does not have linkage）。故该判定整体封装于此函数，
// 返回类型 JSObjectRef 有链接，可安全声明后用。
static JSObjectRef createExportedInstanceIfRegistered(JNIEnv* env, JSContextRef context, jobject value);

static JSValueRef convertJavaToJS(JNIEnv* env, JSContextRef context, jobject value, JSValueRef* exception)
{
    if (!value)
        return JSValueMakeUndefined(context);

    auto& classes = javaClasses();

    if (isInstanceOf(env, value, cachedClass(env, "java/lang/Boolean", classes.booleanClass))) {
        jmethodID booleanValue = env->GetMethodID(classes.booleanClass, "booleanValue", "()Z");
        jboolean result = env->CallBooleanMethod(value, booleanValue);
        clearJavaException(env, "Boolean.booleanValue");
        return JSValueMakeBoolean(context, result);
    }

    if (isInstanceOf(env, value, cachedClass(env, "java/lang/Double", classes.doubleClass))
        || isInstanceOf(env, value, cachedClass(env, "java/lang/Number", classes.numberClass))) {
        jclass numberClass = cachedClass(env, "java/lang/Number", classes.numberClass);
        jmethodID doubleValue = env->GetMethodID(numberClass, "doubleValue", "()D");
        jdouble result = env->CallDoubleMethod(value, doubleValue);
        clearJavaException(env, "Number.doubleValue");
        return JSValueMakeNumber(context, result);
    }

    if (isInstanceOf(env, value, cachedClass(env, "java/lang/String", classes.stringClass))) {
        jstring string = static_cast<jstring>(value);
        const char* utf8 = env->GetStringUTFChars(string, nullptr);
        JSStringRef jsString = JSStringCreateWithUTF8CString(utf8 ? utf8 : "");
        if (utf8)
            env->ReleaseStringUTFChars(string, utf8);
        JSValueRef result = JSValueMakeString(context, jsString);
        JSStringRelease(jsString);
        return result;
    }

    // Java 回调接口 → JS 函数（阶段 4.2）。必须排在「其余 → wrapper」分支之前，
    // 否则回调对象会被包成普通 wrapper 而不是可调用函数。
    if (isInstanceOf(env, value, javaCallbackInterfaceClass(env))) {
        JSValueRef result = createJSFunctionFromCallback(env, context, value);
        if (!result)
            return makeJSError(context, "failed to create JS function from Java callback");
        return result;
    }

    // List → JS 数组（本批只做一层；深层嵌套与环检测在 wrapper 批次用 work-list 补齐）
    if (isInstanceOf(env, value, cachedClass(env, "java/util/List", classes.listInterfaceClass))) {
        jmethodID sizeMethod = env->GetMethodID(classes.listInterfaceClass, "size", "()I");
        jmethodID getMethod = env->GetMethodID(classes.listInterfaceClass, "get", "(I)Ljava/lang/Object;");
        jint count = env->CallIntMethod(value, sizeMethod);
        clearJavaException(env, "List.size");
        if (count < 0)
            count = 0;

        WTF::Vector<JSValueRef> storage;
        storage.reserveInitialCapacity(static_cast<size_t>(count > 0 ? count : 1));
        for (jint i = 0; i < count; ++i) {
            jobject element = env->CallObjectMethod(value, getMethod, i);
            if (env->ExceptionCheck()) {
                clearJavaException(env, "List.get");
                element = nullptr;
            }
            storage.append(convertJavaToJS(env, context, element, exception));
            if (element)
                env->DeleteLocalRef(element);
        }
        JSValueRef result = JSObjectMakeArray(context, static_cast<size_t>(count), storage.span().data(), exception);
        return result;
    }

    // Map → JS 对象（同上，单层）
    if (isInstanceOf(env, value, cachedClass(env, "java/util/Map", classes.mapInterfaceClass))) {
        jmethodID keySetMethod = env->GetMethodID(classes.mapInterfaceClass, "keySet", "()Ljava/util/Set;");
        jmethodID getMethod = env->GetMethodID(classes.mapInterfaceClass, "get", "(Ljava/lang/Object;)Ljava/lang/Object;");
        jobject keySet = env->CallObjectMethod(value, keySetMethod);
        clearJavaException(env, "Map.keySet");

        JSObjectRef object = JSObjectMake(context, nullptr, nullptr);
        if (keySet) {
            jclass setClass = env->FindClass("java/util/Set");
            jmethodID toArrayMethod = env->GetMethodID(setClass, "toArray", "()[Ljava/lang/Object;");
            jobjectArray keys = static_cast<jobjectArray>(env->CallObjectMethod(keySet, toArrayMethod));
            clearJavaException(env, "Set.toArray");
            env->DeleteLocalRef(setClass);

            if (keys) {
                jsize count = env->GetArrayLength(keys);
                for (jsize i = 0; i < count; ++i) {
                    jobject key = env->GetObjectArrayElement(keys, i);
                    jobject mappedValue = env->CallObjectMethod(value, getMethod, key);
                    clearJavaException(env, "Map.get");

                    JSValueRef jsValue = convertJavaToJS(env, context, mappedValue, exception);

                    // 键按字符串处理（与 ObjC 版把 Map 映射为普通对象一致）
                    jclass objectClass = cachedClass(env, "java/lang/Object", classes.objectClass);
                    jmethodID toStringMethod = env->GetMethodID(objectClass, "toString", "()Ljava/lang/String;");
                    jstring keyString = static_cast<jstring>(env->CallObjectMethod(key, toStringMethod));
                    clearJavaException(env, "Object.toString");
                    if (keyString) {
                        const char* keyUtf8 = env->GetStringUTFChars(keyString, nullptr);
                        JSStringRef jsKey = JSStringCreateWithUTF8CString(keyUtf8 ? keyUtf8 : "");
                        if (keyUtf8)
                            env->ReleaseStringUTFChars(keyString, keyUtf8);
                        JSObjectSetProperty(context, object, jsKey, jsValue, kJSPropertyAttributeNone, exception);
                        JSStringRelease(jsKey);
                        env->DeleteLocalRef(keyString);
                    }
                    if (key)
                        env->DeleteLocalRef(key);
                    if (mappedValue)
                        env->DeleteLocalRef(mappedValue);
                }
                env->DeleteLocalRef(keys);
            }
            env->DeleteLocalRef(keySet);
        }
        return object;
    }

    // 已注册的导出类（阶段 4.4 补全）：优先按「带 JSClass 的实例」包装，使该类对象在
    // **任何** Java→JS 边界（工厂返回值、方法返回值、参数转换）都带上原型链方法与
    // @JSProperty 访问器，而不只是 `new` 出来的实例。
    //（此前工厂路径只能事后设 [[Prototype]]，JSClass 无法更换 → 属性访问器缺失。）
    if (JSObjectRef exportedIfAny = createExportedInstanceIfRegistered(env, context, value))
        return exportedIfAny;

    // 其余：走 wrapper —— 同一 Java 对象必须映射到同一 JS 对象（4.1 第二批次的核心语义）
    if (JSObjectRef existing = lookupJSWrapper(env, context, value))
        return existing;
    if (JSObjectRef created = createJSWrapper(env, context, value))
        return created;
    return JSObjectMake(context, nullptr, nullptr);
}

// ---------------------------------------------------------------------------
// 值转换：JS → Java
//   Boolean→Boolean、Number→Double、String→String、Array→ArrayList、
//   Object→HashMap、undefined/null→null、其它→toString 兜底。
// ---------------------------------------------------------------------------

static jobject convertJSToJava(JNIEnv* env, JSContextRef context, JSValueRef value)
{
    if (!value || JSValueIsUndefined(context, value) || JSValueIsNull(context, value))
        return nullptr;

    auto& classes = javaClasses();

    if (JSValueIsBoolean(context, value)) {
        bool result = JSValueToBoolean(context, value);
        jclass booleanClass = cachedClass(env, "java/lang/Boolean", classes.booleanClass);
        jmethodID valueOf = env->GetStaticMethodID(booleanClass, "valueOf", "(Z)Ljava/lang/Boolean;");
        return env->CallStaticObjectMethod(booleanClass, valueOf, static_cast<jboolean>(result));
    }

    if (JSValueIsNumber(context, value)) {
        double result = JSValueToNumber(context, value, nullptr);
        jclass doubleClass = cachedClass(env, "java/lang/Double", classes.doubleClass);
        jmethodID valueOf = env->GetStaticMethodID(doubleClass, "valueOf", "(D)Ljava/lang/Double;");
        return env->CallStaticObjectMethod(doubleClass, valueOf, static_cast<jdouble>(result));
    }

    if (JSValueIsString(context, value)) {
        JSStringRef jsString = JSValueToStringCopy(context, value, nullptr);
        size_t maxSize = JSStringGetMaximumUTF8CStringSize(jsString);
        char* buffer = static_cast<char*>(fastMalloc(maxSize));
        JSStringGetUTF8CString(jsString, buffer, maxSize);
        JSStringRelease(jsString);
        jstring result = env->NewStringUTF(buffer);
        fastFree(buffer);
        return result;
    }

    if (JSValueIsArray(context, value)) {
        JSObjectRef object = JSValueToObject(context, value, nullptr);
        JSStringRef lengthString = JSStringCreateWithUTF8CString("length");
        JSValueRef lengthValue = JSObjectGetProperty(context, object, lengthString, nullptr);
        JSStringRelease(lengthString);
        double lengthNumber = JSValueToNumber(context, lengthValue, nullptr);
        int count = lengthNumber > 0 ? static_cast<int>(lengthNumber) : 0;

        jclass listClass = cachedClass(env, "java/util/ArrayList", classes.listClass);
        jmethodID constructor = listClass ? env->GetMethodID(listClass, "<init>", "(I)V") : nullptr;
        jmethodID addMethod = listClass ? env->GetMethodID(listClass, "add", "(Ljava/lang/Object;)Z") : nullptr;
        if (!constructor || !addMethod) {
            // 防御：解析失败时降级返回 null，绝不把 null methodID 交给 NewObject
            // （CheckJNI 会直接 abort 整个进程）。
            clearJavaException(env, "ArrayList ctor/add");
            return nullptr;
        }
        jobject list = env->NewObject(listClass, constructor, static_cast<jint>(count));
        clearJavaException(env, "new ArrayList");

        for (int i = 0; i < count; ++i) {
            JSValueRef element = JSObjectGetPropertyAtIndex(context, object, static_cast<unsigned>(i), nullptr);
            jobject javaElement = convertJSToJava(env, context, element);
            env->CallBooleanMethod(list, addMethod, javaElement);
            if (env->ExceptionCheck())
                clearJavaException(env, "ArrayList.add");
            if (javaElement)
                env->DeleteLocalRef(javaElement);
        }
        return list;
    }

    if (JSValueIsObject(context, value)) {
        JSObjectRef object = JSValueToObject(context, value, nullptr);

        // 先查 wrapper：该 JS 对象若是某个 Java 对象的包装，直接取回原对象（保证往返同一性）
        if (jobject wrapped = lookupJavaWrapper(env, context, object))
            return wrapped;

        // 取自有属性名（不含原型链）
        JSPropertyNameArrayRef names = JSObjectCopyPropertyNames(context, object);
        size_t count = JSPropertyNameArrayGetCount(names);

        jclass mapClass = cachedClass(env, "java/util/HashMap", classes.mapClass);
        jmethodID constructor = mapClass ? env->GetMethodID(mapClass, "<init>", "(I)V") : nullptr;
        jmethodID putMethod = mapClass
            ? env->GetMethodID(mapClass, "put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;")
            : nullptr;
        if (!constructor || !putMethod) {
            // 同上：null methodID 会让 CheckJNI abort("mid == null")，这里降级为 null。
            clearJavaException(env, "HashMap ctor/put");
            JSPropertyNameArrayRelease(names);
            return nullptr;
        }
        jobject map = env->NewObject(mapClass, constructor, static_cast<jint>(count));
        clearJavaException(env, "new HashMap");

        for (size_t i = 0; i < count; ++i) {
            JSStringRef name = JSPropertyNameArrayGetNameAtIndex(names, i);
            size_t maxSize = JSStringGetMaximumUTF8CStringSize(name);
            char* buffer = static_cast<char*>(fastMalloc(maxSize));
            JSStringGetUTF8CString(name, buffer, maxSize);
            jstring key = env->NewStringUTF(buffer);
            fastFree(buffer);

            JSValueRef propertyValue = JSObjectGetProperty(context, object, name, nullptr);
            jobject mappedValue = convertJSToJava(env, context, propertyValue);
            env->CallObjectMethod(map, putMethod, key, mappedValue);
            if (env->ExceptionCheck())
                clearJavaException(env, "HashMap.put");

            env->DeleteLocalRef(key);
            if (mappedValue)
                env->DeleteLocalRef(mappedValue);
        }
        JSPropertyNameArrayRelease(names);
        return map;
    }

    // 兜底：toString
    JSStringRef jsString = JSValueToStringCopy(context, value, nullptr);
    size_t maxSize = JSStringGetMaximumUTF8CStringSize(jsString);
    char* buffer = static_cast<char*>(fastMalloc(maxSize));
    JSStringGetUTF8CString(jsString, buffer, maxSize);
    JSStringRelease(jsString);
    jstring result = env->NewStringUTF(buffer);
    fastFree(buffer);
    return result;
}

// ---------------------------------------------------------------------------
// native 方法
//   Java 侧：org.webkit.jni.JSCContext / JSCVirtualMachine
// ---------------------------------------------------------------------------

extern "C" JNIEXPORT jlong JNICALL Java_org_webkit_jni_JSCVirtualMachine_nativeCreate(JNIEnv*, jobject)
{
    JSContextGroupRef group = JSContextGroupCreate();
    return reinterpret_cast<jlong>(group);
}

extern "C" JNIEXPORT void JNICALL Java_org_webkit_jni_JSCVirtualMachine_nativeRelease(JNIEnv*, jobject, jlong handle)
{
    if (handle)
        JSContextGroupRelease(reinterpret_cast<JSContextGroupRef>(handle));
}

extern "C" JNIEXPORT jlong JNICALL Java_org_webkit_jni_JSCContext_nativeCreate(JNIEnv*, jobject, jlong groupHandle)
{
    JSContextGroupRef group = groupHandle ? reinterpret_cast<JSContextGroupRef>(groupHandle) : nullptr;
    JSGlobalContextRef context = JSGlobalContextCreateInGroup(group, nullptr);
    return reinterpret_cast<jlong>(context);
}

// 释放本 context 的全部桥接资源（定义见文件尾部；必须在 JSGlobalContextRelease 之前调用）
void releaseContextResources(JNIEnv* env, JSContextRef context);

extern "C" JNIEXPORT void JNICALL Java_org_webkit_jni_JSCContext_nativeRelease(JNIEnv* env, jobject, jlong handle)
{
    if (!handle)
        return;
    JSContextRef context = reinterpret_cast<JSContextRef>(handle);
    // 顺序：先回收桥接资源（期间 context 与 apiLock 仍有效），再释放 context 本身
    releaseContextResources(env, context);
    JSGlobalContextRelease(reinterpret_cast<JSGlobalContextRef>(handle));
}

extern "C" JNIEXPORT jobject JNICALL Java_org_webkit_jni_JSCContext_nativeEvaluateScript(JNIEnv* env, jobject, jlong handle, jstring code)
{
    if (!handle || !code)
        return nullptr;

    JSGlobalContextRef context = reinterpret_cast<JSGlobalContextRef>(handle);

    const char* utf8 = env->GetStringUTFChars(code, nullptr);
    JSStringRef script = JSStringCreateWithUTF8CString(utf8 ? utf8 : "");
    if (utf8)
        env->ReleaseStringUTFChars(code, utf8);

    JSValueRef exception = nullptr;
    JSValueRef result = JSEvaluateScript(context, script, nullptr, nullptr, 0, &exception);
    JSStringRelease(script);

    if (exception) {
        // 4.1：JS 异常转成 Java 的 RuntimeException(message)。
        // 注意：必须显式取字符串 —— JS 的 Error 是对象，convertJSToJava 会把它转成 HashMap，
        // 直接当作 jstring 传给 RuntimeException 构造器会造成 ART 类型检查失败（进程被杀）。
        // 结构化的 JSException（name/message/line/stack）在 4.2 补齐。
        JSStringRef messageString = JSValueToStringCopy(context, exception, nullptr);
        size_t maxSize = JSStringGetMaximumUTF8CStringSize(messageString);
        char* buffer = static_cast<char*>(fastMalloc(maxSize));
        JSStringGetUTF8CString(messageString, buffer, maxSize);
        JSStringRelease(messageString);

        jstring javaMessage = env->NewStringUTF(buffer);
        fastFree(buffer);

        jclass exceptionClass = env->FindClass("java/lang/RuntimeException");
        if (exceptionClass) {
            jmethodID constructor = env->GetMethodID(exceptionClass, "<init>", "(Ljava/lang/String;)V");
            if (constructor) {
                jobject thrown = env->NewObject(exceptionClass, constructor, javaMessage);
                if (thrown)
                    env->Throw(static_cast<jthrowable>(thrown));
            }
            env->DeleteLocalRef(exceptionClass);
        }
        if (javaMessage)
            env->DeleteLocalRef(javaMessage);
        return nullptr;
    }

    return convertJSToJava(env, context, result);
}

// 把 Java 参数转成 JS 值注入为全局 __jscArgs 后求值；脚本以 __jscArgs[i] 访问。
// 这是 4.1 第二批次验证"对象同一性"（Java 对象传入 JS 再取回仍是同一对象）的最小通道；
// 正式的回调与参数编组在 4.2。
extern "C" JNIEXPORT jobject JNICALL Java_org_webkit_jni_JSCContext_nativeEvaluateScriptWithArguments(JNIEnv* env, jobject, jlong handle, jstring code, jobjectArray arguments)
{
    if (!handle || !code)
        return nullptr;

    JSGlobalContextRef context = reinterpret_cast<JSGlobalContextRef>(handle);

    // **全程持 apiLock**：本函数是「注入 `__jscArgs` → 求值 → 清理」的多步序列，而 `__jscArgs`
    // 是**全局属性**。若各步各自取锁（原实现如此），多线程共享同一 context 时会交错 ——
    // 一个线程注入的实参被另一线程覆盖 → 脚本里读到 `undefined`（并发用例实测踩到：
    // 3 线程共享 context 时只有 1 个线程成功）。`JSLock` 可重入，内层各步自行取锁无碍。
    JSC::JSGlobalObject* jscGlobalObject = toJS(context);
    if (!jscGlobalObject)
        return nullptr;
    JSC::VM& vm = jscGlobalObject->vm();
    JSC::JSLockHolder locker(vm);

    JSValueRef exception = nullptr;

    jsize argumentCount = arguments ? env->GetArrayLength(arguments) : 0;
    if (argumentCount > 0) {
        WTF::Vector<JSValueRef> converted;
        converted.reserveInitialCapacity(static_cast<size_t>(argumentCount));
        for (jsize i = 0; i < argumentCount; ++i) {
            jobject argument = env->GetObjectArrayElement(arguments, i);
            converted.append(convertJavaToJS(env, context, argument, &exception));
            if (argument)
                env->DeleteLocalRef(argument);
        }
        JSObjectRef array = JSObjectMakeArray(context, static_cast<size_t>(argumentCount), converted.span().data(), &exception);
        JSObjectRef globalObject = JSContextGetGlobalObject(context);
        JSStringRef name = JSStringCreateWithUTF8CString("__jscArgs");
        JSObjectSetProperty(context, globalObject, name, array, kJSPropertyAttributeDontEnum, &exception);
        JSStringRelease(name);
    }

    const char* utf8 = env->GetStringUTFChars(code, nullptr);
    JSStringRef script = JSStringCreateWithUTF8CString(utf8 ? utf8 : "");
    if (utf8)
        env->ReleaseStringUTFChars(code, utf8);

    JSValueRef result = JSEvaluateScript(context, script, nullptr, nullptr, 0, &exception);
    JSStringRelease(script);

    // 清理注入的全局，避免污染后续脚本
    if (argumentCount > 0) {
        JSObjectRef globalObject = JSContextGetGlobalObject(context);
        JSStringRef name = JSStringCreateWithUTF8CString("__jscArgs");
        JSValueRef undefined = JSValueMakeUndefined(context);
        JSObjectSetProperty(context, globalObject, name, undefined, kJSPropertyAttributeDontEnum, nullptr);
        JSStringRelease(name);
    }

    if (exception) {
        JSStringRef messageString = JSValueToStringCopy(context, exception, nullptr);
        size_t maxSize = JSStringGetMaximumUTF8CStringSize(messageString);
        char* buffer = static_cast<char*>(fastMalloc(maxSize));
        JSStringGetUTF8CString(messageString, buffer, maxSize);
        JSStringRelease(messageString);

        jstring javaMessage = env->NewStringUTF(buffer);
        fastFree(buffer);

        jclass exceptionClass = env->FindClass("java/lang/RuntimeException");
        if (exceptionClass) {
            jmethodID constructor = env->GetMethodID(exceptionClass, "<init>", "(Ljava/lang/String;)V");
            if (constructor) {
                jobject thrown = env->NewObject(exceptionClass, constructor, javaMessage);
                if (thrown)
                    env->Throw(static_cast<jthrowable>(thrown));
            }
            env->DeleteLocalRef(exceptionClass);
        }
        if (javaMessage)
            env->DeleteLocalRef(javaMessage);
        return nullptr;
    }

    return convertJSToJava(env, context, result);
}

// 弱值通知排空（定义见文件尾部；GC 内部只入队，JNI 调用统一在安全点做）
namespace {
void drainWeakValueNotifications();
}

extern "C" JNIEXPORT void JNICALL Java_org_webkit_jni_JSCContext_nativeGarbageCollect(JNIEnv* env, jobject, jlong handle)
{
    if (!handle)
        return;
    JSContextRef context = reinterpret_cast<JSContextRef>(handle);
    JSC::JSGlobalObject* globalObject = toJS(context);
    if (!globalObject)
        return;
    JSC::VM& vm = globalObject->vm();
    JSC::JSLockHolder locker(vm);

    // 顺序刻意如此：
    //   1) 先摘除"JS 侧已回收"的条目 → 释放其 Java 强引用
    //   2) 跑**同步 Full GC** → 本轮回收的包装由 finalize 记录
    //   3) 回标 + 立即摘除它们
    //
    // 为何不用 C API 的 `JSGarbageCollect`：它只调 `heap.reportAbandonedObjectGraph()`
    //（`API/JSBase.cpp:125-140`）—— **既不保证同步、也不指定 scope**；而
    // `Collector::m_opaqueRoots` **只在 Full GC 时 clear**（`Collector.cpp:382-385`）。
    // 走 Eden 路径时，上一轮登记的 opaque root 不会被清 → wrapper 的 weak handle 被永久保活
    // → 表项连同 Java 强引用一起泄漏（本轮实测踩到：10 轮压力后仍不回收）。
    JSC::GCRequest request;
    request.scope = JSC::CollectionScope::Full;

    sweepWrappers(env, context);
    vm.heap.collectSync(request);
    drainCollectedWrappers();
    sweepWrappers(env, context);
    vm.heap.collectSync(request);
    drainCollectedWrappers();

    // JS GC 之后派发弱值的 cleared 通知（GC 内部只入队，不做 JNI）
    drainWeakValueNotifications();
}

// 显式清扫 wrapper 表，返回清理掉的条目数（宿主可在内存压力下主动调用）
extern "C" JNIEXPORT jlong JNICALL Java_org_webkit_jni_JSCContext_nativeSweepWrappers(JNIEnv* env, jobject, jlong handle)
{
    if (!handle)
        return 0;
    return static_cast<jlong>(sweepWrappers(env, reinterpret_cast<JSContextRef>(handle)));
}

// 当前 context 的 wrapper 表规模（泄漏/回收观测用）
extern "C" JNIEXPORT jlong JNICALL Java_org_webkit_jni_JSCContext_nativeWrapperCount(JNIEnv*, jobject, jlong handle)
{
    if (!handle)
        return 0;
    return static_cast<jlong>(wrapperCount(reinterpret_cast<JSContextRef>(handle)));
}

// 撤销某 Java 对象包装的 JS 侧保护：释放后 JS 侧无引用时，JS GC 即可回收该包装
//（cell 复用的能力；表项随后由 sweep 摘除）
extern "C" JNIEXPORT jboolean JNICALL Java_org_webkit_jni_JSCContext_nativeReleaseWrapper(JNIEnv* env, jobject, jlong handle, jobject object)
{
    if (!handle || !object)
        return JNI_FALSE;
    return releaseWrapperProtection(env, reinterpret_cast<JSContextRef>(handle), object) ? JNI_TRUE : JNI_FALSE;
}

// 全部 context 的 wrapper 表规模之和
extern "C" JNIEXPORT jlong JNICALL Java_org_webkit_jni_JSCContext_nativeTotalWrapperCount(JNIEnv*, jobject)
{
    return static_cast<jlong>(totalWrapperCount());
}

// managed references：永久根（单参）—— 与 owner 无关的直接保活
extern "C" JNIEXPORT jboolean JNICALL Java_org_webkit_jni_JSCContext_nativeAddManagedReference(JNIEnv* env, jobject, jobject object)
{
    if (!object)
        return JNI_FALSE;
    return addManagedReference(env, object) ? JNI_TRUE : JNI_FALSE;
}

// external object graph：登记 owner 持有 object 的关系（不新增引用；存活由 opaque root 传播决定）
extern "C" JNIEXPORT jboolean JNICALL Java_org_webkit_jni_JSCContext_nativeAddManagedReferenceWithOwner(JNIEnv* env, jobject, jlong handle, jobject object, jobject owner)
{
    if (!handle || !object || !owner)
        return JNI_FALSE;
    JSContextRef context = reinterpret_cast<JSContextRef>(handle);
    JSC::JSGlobalObject* globalObject = toJS(context);
    if (!globalObject)
        return JNI_FALSE;
    // 需要 VM：老/新判定要查该 VM 的 opaque root 集合（对齐原版 isOldExternalObject）。
    JSC::VM& vm = globalObject->vm();
    JSC::JSLockHolder locker(vm);
    addOwnership(env, vm, object, owner);
    return JNI_TRUE;
}

extern "C" JNIEXPORT jboolean JNICALL Java_org_webkit_jni_JSCContext_nativeRemoveManagedReferenceWithOwner(JNIEnv* env, jobject, jobject object, jobject owner)
{
    if (!object || !owner)
        return JNI_FALSE;
    return removeOwnership(env, object, owner) ? JNI_TRUE : JNI_FALSE;
}

// 观测：外部对象图中的关系总数（跨 context；泄漏与回收观测用）
extern "C" JNIEXPORT jlong JNICALL Java_org_webkit_jni_JSCContext_nativeOwnershipCount(JNIEnv*, jobject)
{
    return static_cast<jlong>(ownershipCount());
}

extern "C" JNIEXPORT jboolean JNICALL Java_org_webkit_jni_JSCContext_nativeRemoveManagedReference(JNIEnv* env, jobject, jobject object)
{
    if (!object)
        return JNI_FALSE;
    return removeManagedReference(env, object) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jlong JNICALL Java_org_webkit_jni_JSCContext_nativeManagedReferenceCount(JNIEnv*, jobject)
{
    return static_cast<jlong>(managedReferenceCount());
}

// ---------------------------------------------------------------------------
// 回调桥：Java 回调对象 → JS 函数（阶段 4.2）
//
// 实现路径（与方案的偏差，同 4.1/4.3 的取舍）：方案设想 `JSCJniCallbackFunction : InternalFunction`
// （复刻 ObjCCallbackFunction.mm），需要给 JSGlobalObject 加 jniCallbackFunctionStructure 并触碰
// Heap 的 cell 类。本批次改用 C API 的 JSClassDefinition（callAsFunction + finalize）：
// 产出物是可调用的 JSCallbackObject（JS 侧 `typeof === "function"`），不触碰 JSC 核心。
//
// apiLock：ObjC 版本在 callAsFunction 首行取 JSLockHolder（ObjCCallbackFunction.mm:480-484），
// 因为那里直接构造内部 JSValue 对象；C API 路径下 apiLock 已由 JSC 内部持有
// （JSObjectCallAsFunction / JSEvaluateScript 等入口都自行取锁，且 JSLock 可重入），故不重复取锁。
//
// 异常分层（方案 §6.6 第 3 条）：
//   · Java 回调抛异常 → ExceptionCheck + ExceptionClear → Throwable.toString() → JS *exception（可 catch）
//   · 参数/返回值转换失败 → 同上，转为 JS 侧可 catch 的 Error
//
// 保活：私有数据持 jobject 强引用（NewGlobalRef），finalize 时释放。finalize 可能运行在非 Java
// 线程，故用 attachAndGetEnv() 取 env（只做 DeleteGlobalRef，不进 Java 代码，不会死锁）。
// ---------------------------------------------------------------------------

namespace {

struct JniCallbackData {
    jobject javaGlobalRef { nullptr };   // 回调对象（强引用：JS 侧函数存活期间保活）
    jmethodID invokeMethod { nullptr };  // JSCJniCallbackFunction.invoke(Object[])
};

} // namespace

// 前置声明（定义见本块之后）。用 static 保持内部链接，避免符号污染。
static void jniCallbackFinalize(JSObjectRef object);
static JSValueRef jniCallbackCallAsFunction(JSContextRef context, JSObjectRef function, JSObjectRef thisObject,
    size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception);
static JSClassRef callbackFunctionClass();

// 进程级 JSClass（callAsFunction + finalize）
static JSClassRef callbackFunctionClass()
{
    static JSClassRef jsClass = nullptr;
    if (jsClass)
        return jsClass;
    JSClassDefinition definition = kJSClassDefinitionEmpty;
    definition.className = "JSCJniCallbackFunction";
    definition.callAsFunction = jniCallbackCallAsFunction;
    definition.finalize = jniCallbackFinalize;
    jsClass = JSClassCreate(&definition);
    return jsClass;
}

static void jniCallbackFinalize(JSObjectRef object)
{
    auto* data = static_cast<JniCallbackData*>(JSObjectGetPrivate(object));
    if (!data)
        return;
    if (data->javaGlobalRef) {
        JNIEnv* env = attachAndGetEnv();
        if (env)
            env->DeleteGlobalRef(data->javaGlobalRef);
    }
    delete data;
}

// 回调接口类（惰性缓存；FindClass 需要 Java 线程上下文，故不放在 JNI_OnLoad）
jclass javaCallbackInterfaceClass(JNIEnv* env)
{
    static jclass cachedInterfaceClass = nullptr;
    if (cachedInterfaceClass)
        return cachedInterfaceClass;
    jclass local = env->FindClass("org/webkit/jni/JSCJniCallbackFunction");
    if (!local) {
        if (env->ExceptionCheck())
            env->ExceptionClear();
        return nullptr;
    }
    cachedInterfaceClass = static_cast<jclass>(env->NewGlobalRef(local));
    env->DeleteLocalRef(local);
    return cachedInterfaceClass;
}

// 构造 JS 侧 Error（可被 JS catch）
JSValueRef makeJSError(JSContextRef context, const char* message)
{
    JSStringRef jsMessage = JSStringCreateWithUTF8CString(message ? message : "unknown error");
    JSValueRef argument = JSValueMakeString(context, jsMessage);
    JSStringRelease(jsMessage);
    return JSObjectMakeError(context, 1, &argument, nullptr);
}

namespace {

// Throwable → JS Error（message 取 "类名: message"）。
//
// 编码路径刻意走 JNI 的 UTF-16（GetStringChars + JSStringCreateWithCharacters），全程不经过
// WTF::String 的 UTF-8 转换 —— 实测本版本 WTF 的 String::fromUTF8(const char*) 没有按 UTF-8
// 解码（产出的正是把 UTF-8 字节两两配对的乱码，如 "java" → "慪慶"）。
static JSValueRef makeJSErrorFromThrowable(JSContextRef context, JNIEnv* env, jthrowable throwable)
{
    JSValueRef result = nullptr;
    jclass throwableClass = env->FindClass("java/lang/Throwable");
    if (!throwableClass) {
        if (env->ExceptionCheck())
            env->ExceptionClear();
        return makeJSError(context, "java exception");
    }
    jmethodID toStringMethod = env->GetMethodID(throwableClass, "toString", "()Ljava/lang/String;");
    if (!toStringMethod) {
        if (env->ExceptionCheck())
            env->ExceptionClear();
        env->DeleteLocalRef(throwableClass);
        return makeJSError(context, "java exception");
    }
    auto* string = static_cast<jstring>(env->CallObjectMethod(throwable, toStringMethod));
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
    } else if (string) {
        const jchar* chars = env->GetStringChars(string, nullptr);
        if (chars) {
            jsize length = env->GetStringLength(string);
            JSStringRef jsMessage = JSStringCreateWithCharacters(
                reinterpret_cast<const JSChar*>(chars), static_cast<size_t>(length));
            JSValueRef argument = JSValueMakeString(context, jsMessage);
            JSStringRelease(jsMessage);
            result = JSObjectMakeError(context, 1, &argument, nullptr);
            env->ReleaseStringChars(string, chars);
        }
        env->DeleteLocalRef(string);
    }
    env->DeleteLocalRef(throwableClass);
    return result ? result : makeJSError(context, "java exception");
}

} // namespace

// 由 Java 回调对象创建 JS 函数。
JSValueRef createJSFunctionFromCallback(JNIEnv* env, JSContextRef context, jobject callback)
{
    jclass interfaceClass = javaCallbackInterfaceClass(env);
    if (!interfaceClass)
        return nullptr;

    jmethodID invokeMethod = env->GetMethodID(interfaceClass, "invoke", "([Ljava/lang/Object;)Ljava/lang/Object;");
    if (!invokeMethod) {
        if (env->ExceptionCheck())
            env->ExceptionClear();
        return nullptr;
    }

    JSClassRef jsClass = callbackFunctionClass();
    if (!jsClass)
        return nullptr;

    jobject globalRef = env->NewGlobalRef(callback);
    if (!globalRef)
        return nullptr;

    auto* data = new JniCallbackData;
    data->javaGlobalRef = globalRef;
    data->invokeMethod = invokeMethod;

    JSObjectRef function = JSObjectMake(context, jsClass, data);
    if (!function) {
        // JSObjectMake 失败时不会回调 finalize，需自行释放
        env->DeleteGlobalRef(globalRef);
        delete data;
        return nullptr;
    }
    return function;
}

// JS 侧调用入口（前置声明见本块之前）
static JSValueRef jniCallbackCallAsFunction(JSContextRef context, JSObjectRef function, JSObjectRef thisObject,
    size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception)
{
    UNUSED_PARAM(thisObject);

    auto* data = static_cast<JniCallbackData*>(JSObjectGetPrivate(function));
    if (!data || !data->javaGlobalRef) {
        if (exception)
            *exception = makeJSError(context, "JNI callback has been detached");
        return JSValueMakeUndefined(context);
    }

    JNIEnv* env = attachAndGetEnv();
    if (!env) {
        if (exception)
            *exception = makeJSError(context, "cannot attach current thread to JVM");
        return JSValueMakeUndefined(context);
    }

    auto& classes = javaClasses();
    jclass objectClass = cachedClass(env, "java/lang/Object", classes.objectClass);
    jobjectArray javaArguments = env->NewObjectArray(static_cast<jsize>(argumentCount), objectClass, nullptr);
    if (!javaArguments) {
        if (env->ExceptionCheck())
            env->ExceptionClear();
        if (exception)
            *exception = makeJSError(context, "cannot allocate JNI argument array");
        return JSValueMakeUndefined(context);
    }

    // 参数编组：JS → Java
    for (size_t i = 0; i < argumentCount; ++i) {
        jobject javaArgument = convertJSToJava(env, context, arguments[i]);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            env->DeleteLocalRef(javaArguments);
            if (exception)
                *exception = makeJSError(context, "cannot convert JS argument to Java value");
            return JSValueMakeUndefined(context);
        }
        env->SetObjectArrayElement(javaArguments, static_cast<jsize>(i), javaArgument);
        if (env->ExceptionCheck())
            env->ExceptionClear();
        if (javaArgument)
            env->DeleteLocalRef(javaArgument);
    }

    jobject javaResult = env->CallObjectMethod(data->javaGlobalRef, data->invokeMethod, javaArguments);
    env->DeleteLocalRef(javaArguments);

    // Java 异常 → JS Error（方案 §6.3「Java 异常 → ExceptionClear → JS Error」）
    if (env->ExceptionCheck()) {
        jthrowable throwable = env->ExceptionOccurred();
        env->ExceptionClear();
        JSValueRef error = throwable ? makeJSErrorFromThrowable(context, env, throwable)
                                     : makeJSError(context, "java exception");
        if (throwable)
            env->DeleteLocalRef(throwable);
        if (exception)
            *exception = error;
        return JSValueMakeUndefined(context);
    }

    if (!javaResult)
        return JSValueMakeUndefined(context);

    JSValueRef jsResult = convertJavaToJS(env, context, javaResult, exception);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        env->DeleteLocalRef(javaResult);
        if (exception)
            *exception = makeJSError(context, "cannot convert Java return value to JS value");
        return JSValueMakeUndefined(context);
    }
    env->DeleteLocalRef(javaResult);
    return jsResult;
}

// ---------------------------------------------------------------------------
// 声明式导出：Java 类 → JS 构造器（阶段 4.4）
//
// 实现路径（沿用 4.1–4.3 的取舍）：用 C API 的 JSClassDefinition（callAsConstructor /
// callAsFunction）+ JSObjectSetPrototype 构造「构造器 / prototype / 方法」三层结构，
// 不触碰 JSC 核心（方案设想的 JSCJniClass 与 GLib/ObjC 的 cell 类路径未采用）。
//
// 结构（每个已注册的 Java 类，per-context）：
//   constructor            —— JS 函数（callAsConstructor），并挂到 globalThis[simpleName]
//     .prototype           —— prototype 对象，其属性是各个导出方法的函数
//   继承：注册子类时把 B.prototype 的 [[Prototype]] 设为 A.prototype（逐级 JSObjectSetPrototype）
//
// 实例：构造时复用 4.3 的 wrapper 表（createJSWrapper）→ 同一 Java 对象映射同一 JS 对象，
// 再把实例原型指向该类的 prototype，从而获得方法查找能力。
//
// 方法签名约定：导出方法的参数与返回值都是 Object（见 @JSExport 文档）；参数经 jvalue[]
// 按引用传入（CallObjectMethodA），返回值经 convertJavaToJS 转换。
//
// 已知简化（记录在文档）：注册表随 context 常驻（不随 context 释放）；只导出实例方法；
// 不支持 @JSProperty 访问器与静态成员。
// ---------------------------------------------------------------------------

// 前置声明（定义在本块之后）。用 static 保持内部链接，避免符号污染。
static JSValueRef jniMethodCallAsFunction(JSContextRef context, JSObjectRef function, JSObjectRef thisObject,
    size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception);
static JSObjectRef jniConstructorCallAsConstructor(JSContextRef context, JSObjectRef constructor,
    size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception);
static JSValueRef jniInstanceGetProperty(JSContextRef context, JSObjectRef object, JSStringRef propertyName,
    JSValueRef* exception);
static bool jniInstanceSetProperty(JSContextRef context, JSObjectRef object, JSStringRef propertyName,
    JSValueRef value, JSValueRef* exception);
static JSValueRef jniConstructorGetProperty(JSContextRef context, JSObjectRef object, JSStringRef propertyName,
    JSValueRef* exception);
static bool jniConstructorSetProperty(JSContextRef context, JSObjectRef object, JSStringRef propertyName,
    JSValueRef value, JSValueRef* exception);

namespace {

// 属性访问器条目（@JSProperty）。getter / setter 至少有一个非空。
struct JniPropertyEntry {
    WTF::String name;
    jmethodID getter { nullptr };
    jmethodID setter { nullptr };
    bool setterIsVoid { false };
    bool isStatic { false };                  // 静态属性：用 CallStatic*Method 调用（jclass 取声明类）
};

// 静态字段（@JSExport 标注的 public static 字段）：走 java.lang.reflect.Field，
// 读/写统一为 Object（Field.get/set），避免 JNI 的字段类型分派。
struct JniStaticFieldEntry {
    WTF::String name;
    jobject javaField { nullptr };            // java.lang.reflect.Field（global ref）
    bool isFinal { false };                   // final 字段只读
};

struct JniClassData {
    jclass javaClass { nullptr };             // global ref
    jmethodID defaultConstructor { nullptr }; // public 无参构造（可为 null）
    JSObjectRef factory { nullptr };          // 工厂 JS 函数（来自回调桥，可为 null）
    JSObjectRef prototype { nullptr };        // 该类的 prototype 对象
    JSObjectRef constructor { nullptr };      // 构造器函数对象（静态成员挂在它上面；子类注册时连原型链）
    JSClassRef instanceClass { nullptr };     // 实例的 JSClass（带 getProperty/setProperty）
    Vector<JniPropertyEntry> properties;      // 实例属性表（线性查找；导出属性通常很少）
    Vector<JniPropertyEntry> staticProperties; // 静态属性（@JSProperty static 方法；访问器挂在构造器上）
    Vector<JniStaticFieldEntry> staticFields;   // 静态字段（访问器挂在构造器上）
    Vector<JSObjectRef> methodFunctions;        // 导出的方法函数对象（context 释放时回收其 JniMethodData 的 global ref）

    JniPropertyEntry* findProperty(const WTF::String& propertyName)
    {
        for (auto& entry : properties) {
            if (entry.name == propertyName)
                return &entry;
        }
        return nullptr;
    }

    JniPropertyEntry* findStaticProperty(const WTF::String& propertyName)
    {
        for (auto& entry : staticProperties) {
            if (entry.name == propertyName)
                return &entry;
        }
        return nullptr;
    }

    JniStaticFieldEntry* findStaticField(const WTF::String& propertyName)
    {
        for (auto& entry : staticFields) {
            if (entry.name == propertyName)
                return &entry;
        }
        return nullptr;
    }
};

struct JniMethodData {
    jmethodID methodID { nullptr };
    bool isStatic { false };
    jclass javaClass { nullptr };   // 静态方法的声明类（global ref；finalize 释放）
};

struct ContextClassTable {
    HashMap<jclass, JniClassData*> byJavaClass;
};

Lock& classTableLock()
{
    static NeverDestroyed<Lock> lock;
    return lock.get();
}

HashMap<JSContextRef, ContextClassTable>& classTables()
{
    static NeverDestroyed<HashMap<JSContextRef, ContextClassTable>> tables;
    return tables.get();
}

// 取已注册类（调用方需持 classTableLock）
JniClassData* findRegisteredClass(JSContextRef context, jclass javaClass)
{
    auto it = classTables().find(context);
    if (it == classTables().end())
        return nullptr;
    for (auto& entry : it->value.byJavaClass) {
        if (JNIEnv* env = attachAndGetEnv()) {
            if (env->IsSameObject(entry.key, javaClass))
                return entry.value;
        }
    }
    return nullptr;
}

// 查：某 Java 对象所属类是否为已注册的导出类（供 convertJavaToJS 判定）。
// 取运行时类 → 查类注册表；该 context 未注册任何类时快速返回（避免无谓的 attach）。
JniClassData* findRegisteredClassForObject(JSContextRef context, jobject javaObject)
{
    if (!javaObject)
        return nullptr;
    JNIEnv* env = attachAndGetEnv();
    if (!env)
        return nullptr;
    Locker locker { classTableLock() };
    if (classTables().find(context) == classTables().end())
        return nullptr;
    jclass objectClass = env->GetObjectClass(javaObject);
    if (!objectClass)
        return nullptr;
    JniClassData* result = findRegisteredClass(context, objectClass);
    env->DeleteLocalRef(objectClass);
    return result;
}

// --- 导出方法 -------------------------------------------------------------

static void jniMethodFinalize(JSObjectRef object)
{
    auto* data = static_cast<JniMethodData*>(JSObjectGetPrivate(object));
    if (!data)
        return;
    // 静态方法的函数对象持声明类的 global ref；finalize 可能跑在非 Java 线程
    // （只做 DeleteGlobalRef，不进 Java 代码，不会死锁）。
    if (data->javaClass) {
        if (JNIEnv* env = attachAndGetEnv())
            env->DeleteGlobalRef(data->javaClass);
    }
    delete data;
}

static JSClassRef methodFunctionClass()
{
    static JSClassRef jsClass = nullptr;
    if (jsClass)
        return jsClass;
    JSClassDefinition definition = kJSClassDefinitionEmpty;
    definition.className = "JSCJniExportedMethod";
    definition.callAsFunction = jniMethodCallAsFunction;
    definition.finalize = jniMethodFinalize;
    jsClass = JSClassCreate(&definition);
    return jsClass;
}

// --- 构造器 ---------------------------------------------------------------

static JSValueRef jniConstructorCallAsFunction(JSContextRef context, JSObjectRef function, JSObjectRef thisObject,
    size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception)
{
    // JS 侧 `new X()` 走 callAsConstructor；不带 new 直接调用时给出明确错误
    UNUSED_PARAM(function);
    UNUSED_PARAM(thisObject);
    UNUSED_PARAM(argumentCount);
    UNUSED_PARAM(arguments);
    if (exception)
        *exception = makeJSError(context, "exported class must be called with 'new'");
    return JSValueMakeUndefined(context);
}

static JSClassRef constructorFunctionClass()
{
    static JSClassRef jsClass = nullptr;
    if (jsClass)
        return jsClass;
    JSClassDefinition definition = kJSClassDefinitionEmpty;
    definition.className = "JSCJniExportedClass";
    definition.callAsConstructor = jniConstructorCallAsConstructor;
    definition.callAsFunction = jniConstructorCallAsFunction;
    // 静态成员（静态方法挂属性、@JSProperty static 与静态字段挂访问器）都在构造器函数对象上。
    // 访问器与实例同一套路：未命中返回 nullptr/false 交回默认行为 —— 于是能继续沿
    // 构造器的 [[Prototype]]（父类构造器）查到父类的静态成员。
    definition.getProperty = jniConstructorGetProperty;
    definition.setProperty = jniConstructorSetProperty;
    jsClass = JSClassCreate(&definition);
    return jsClass;
}

} // namespace

// ---------------------------------------------------------------------------
// 属性访问器（@JSProperty）：实例的 JSClass 上挂 getProperty / setProperty
//
// 设计要点：
//   · 每个注册的 Java 类建一个 JSClass；实例用该 class 创建，私有数据即 JniClassData*，
//     因此访问器无需查表即可定位属性表
//   · getProperty 未命中返回 nullptr、setProperty 未命中返回 false —— 交回 JSC 默认行为，
//     从而继续沿原型链查找（导出方法、父类属性因此仍可访问）
//   · Java 实例经 4.3 的 wrapper 表反查（lookupJavaWrapper），对象同一性保持一致
// ---------------------------------------------------------------------------

// 取 JS 属性名（UTF-8 → WTF::String）
static WTF::String propertyNameToString(JSStringRef propertyName)
{
    if (!propertyName)
        return WTF::String();
    size_t maxBytes = JSStringGetMaximumUTF8CStringSize(propertyName);
    if (!maxBytes)
        return WTF::String();
    char stackBuffer[128];
    if (maxBytes <= sizeof(stackBuffer)) {
        JSStringGetUTF8CString(propertyName, stackBuffer, sizeof(stackBuffer));
        return WTF::String::fromUTF8(stackBuffer);
    }
    char* heapBuffer = static_cast<char*>(::malloc(maxBytes));
    if (!heapBuffer)
        return WTF::String();
    JSStringGetUTF8CString(propertyName, heapBuffer, maxBytes);
    WTF::String result = WTF::String::fromUTF8(heapBuffer);
    ::free(heapBuffer);
    return result;
}

// 取导出实例的类元数据。
// 注意：实例现在是 JSAPIWrapperObject cell（wrappedObject 槽存 holder），不再是
// JSCallbackObject<JSNonFinalObject> —— 故不能用 JSObjectGetPrivate（它只认后者），
// 改为从 cell 的 wrappedObject 槽取 holder，再取 holder->classData。
static JniClassData* instanceClassData(JSObjectRef object)
{
    if (!object)
        return nullptr;
    JSC::JSObject* jsObject = toJS(object);
    if (!jsObject->inherits<JSC::JSAPIWrapperObject>())
        return nullptr;
    auto* holder = static_cast<JniWrapperHolder*>(static_cast<JSC::JSAPIWrapperObject*>(jsObject)->wrappedObject());
    return holder ? holder->classData : nullptr;
}

static JSValueRef jniInstanceGetProperty(JSContextRef context, JSObjectRef object, JSStringRef propertyName,
    JSValueRef* exception)
{
    auto* classData = instanceClassData(object);
    if (!classData || classData->properties.isEmpty())
        return nullptr;   // 非导出实例 → 默认行为

    JniPropertyEntry* property = classData->findProperty(propertyNameToString(propertyName));
    if (!property || !property->getter)
        return nullptr;   // 未命中或只写属性 → 默认行为（继续沿原型链）

    JNIEnv* env = attachAndGetEnv();
    if (!env) {
        if (exception)
            *exception = makeJSError(context, "cannot attach current thread to JVM");
        return nullptr;
    }

    jobject instance = lookupJavaWrapper(env, context, object);
    if (!instance) {
        if (exception)
            *exception = makeJSError(context, "exported property read on a non-Java receiver");
        return nullptr;
    }

    jobject javaResult = env->CallObjectMethod(instance, property->getter);
    env->DeleteLocalRef(instance);

    if (env->ExceptionCheck()) {
        jthrowable throwable = env->ExceptionOccurred();
        env->ExceptionClear();
        if (exception)
            *exception = throwable ? makeJSErrorFromThrowable(context, env, throwable)
                                   : makeJSError(context, "java exception");
        if (throwable)
            env->DeleteLocalRef(throwable);
        return nullptr;
    }

    if (!javaResult)
        return JSValueMakeUndefined(context);

    JSValueRef jsResult = convertJavaToJS(env, context, javaResult, exception);
    env->DeleteLocalRef(javaResult);
    return jsResult;
}

static bool jniInstanceSetProperty(JSContextRef context, JSObjectRef object, JSStringRef propertyName,
    JSValueRef value, JSValueRef* exception)
{
    auto* classData = instanceClassData(object);
    if (!classData || classData->properties.isEmpty())
        return false;

    JniPropertyEntry* property = classData->findProperty(propertyNameToString(propertyName));
    if (!property)
        return false;   // 未命中 → 默认行为（在对象自身建属性 / 交给原型链）
    if (!property->setter)
        return true;    // 只读属性：吞掉写入，避免在实例上生成遮蔽同名 getter 的自身属性

    JNIEnv* env = attachAndGetEnv();
    if (!env) {
        if (exception)
            *exception = makeJSError(context, "cannot attach current thread to JVM");
        return true;
    }

    jobject instance = lookupJavaWrapper(env, context, object);
    if (!instance) {
        if (exception)
            *exception = makeJSError(context, "exported property write on a non-Java receiver");
        return true;
    }

    jobject javaValue = convertJSToJava(env, context, value);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        env->DeleteLocalRef(instance);
        if (exception)
            *exception = makeJSError(context, "cannot convert JS value to Java value");
        return true;
    }

    if (property->setterIsVoid)
        env->CallVoidMethod(instance, property->setter, javaValue);
    else
        env->CallObjectMethod(instance, property->setter, javaValue);

    if (javaValue)
        env->DeleteLocalRef(javaValue);
    env->DeleteLocalRef(instance);

    if (env->ExceptionCheck()) {
        jthrowable throwable = env->ExceptionOccurred();
        env->ExceptionClear();
        if (exception)
            *exception = throwable ? makeJSErrorFromThrowable(context, env, throwable)
                                   : makeJSError(context, "java exception");
        if (throwable)
            env->DeleteLocalRef(throwable);
    }
    return true;   // 已处理：即使出错也不再走默认行为
}

// ---------------------------------------------------------------------------
// 静态成员访问器：挂在「构造器函数对象」的 JSClass 上（见 constructorFunctionClass）
//
// 数据来源：
//   · staticProperties —— @JSProperty 标注的 static getter/setter 方法（走 CallStatic*Method）
//   · staticFields     —— @JSExport 标注的 public static 字段（反射 Field.get/set，统一 Object）
// 未命中返回 nullptr/false → 交回默认行为，于是能沿构造器的 [[Prototype]]
// （父类构造器）查到父类的静态成员 —— 与 ES6 `Object.setPrototypeOf(B, A)` 的静态继承一致。
// ---------------------------------------------------------------------------

// java.lang.reflect.Field.get(Object) 的 methodID（进程级缓存）
static jmethodID fieldGetMethodID(JNIEnv* env)
{
    static jmethodID cached = nullptr;
    if (!cached) {
        jclass fieldClass = cachedClass(env, "java/lang/reflect/Field", javaClasses().fieldClass);
        if (fieldClass)
            cached = env->GetMethodID(fieldClass, "get", "(Ljava/lang/Object;)Ljava/lang/Object;");
        if (!cached && env->ExceptionCheck())
            env->ExceptionClear();
    }
    return cached;
}

// java.lang.reflect.Field.set(Object, Object) 的 methodID
static jmethodID fieldSetMethodID(JNIEnv* env)
{
    static jmethodID cached = nullptr;
    if (!cached) {
        jclass fieldClass = cachedClass(env, "java/lang/reflect/Field", javaClasses().fieldClass);
        if (fieldClass)
            cached = env->GetMethodID(fieldClass, "set", "(Ljava/lang/Object;Ljava/lang/Object;)V");
        if (!cached && env->ExceptionCheck())
            env->ExceptionClear();
    }
    return cached;
}

static JSValueRef jniConstructorGetProperty(JSContextRef context, JSObjectRef object, JSStringRef propertyName,
    JSValueRef* exception)
{
    auto* classData = static_cast<JniClassData*>(JSObjectGetPrivate(object));
    if (!classData)
        return nullptr;

    WTF::String name = propertyNameToString(propertyName);

    // ① 静态属性（@JSProperty 的 static getter）
    if (JniPropertyEntry* property = classData->findStaticProperty(name)) {
        if (!property->getter)
            return nullptr;   // 只写属性 → 默认行为
        JNIEnv* env = attachAndGetEnv();
        if (!env) {
            if (exception)
                *exception = makeJSError(context, "cannot attach current thread to JVM");
            return nullptr;
        }
        jobject javaResult = env->CallStaticObjectMethod(classData->javaClass, property->getter);
        if (env->ExceptionCheck()) {
            jthrowable throwable = env->ExceptionOccurred();
            env->ExceptionClear();
            if (exception)
                *exception = throwable ? makeJSErrorFromThrowable(context, env, throwable)
                                       : makeJSError(context, "java exception");
            if (throwable)
                env->DeleteLocalRef(throwable);
            return nullptr;
        }
        if (!javaResult)
            return JSValueMakeUndefined(context);
        JSValueRef jsResult = convertJavaToJS(env, context, javaResult, exception);
        env->DeleteLocalRef(javaResult);
        return jsResult;
    }

    // ② 静态字段（反射 Field.get(null)）
    if (JniStaticFieldEntry* field = classData->findStaticField(name)) {
        JNIEnv* env = attachAndGetEnv();
        if (!env) {
            if (exception)
                *exception = makeJSError(context, "cannot attach current thread to JVM");
            return nullptr;
        }
        jmethodID getMethod = fieldGetMethodID(env);
        if (!getMethod) {
            if (exception)
                *exception = makeJSError(context, "cannot resolve java.lang.reflect.Field.get");
            return nullptr;
        }
        jobject javaResult = env->CallObjectMethod(field->javaField, getMethod, nullptr);
        if (env->ExceptionCheck()) {
            jthrowable throwable = env->ExceptionOccurred();
            env->ExceptionClear();
            if (exception)
                *exception = throwable ? makeJSErrorFromThrowable(context, env, throwable)
                                       : makeJSError(context, "java exception");
            if (throwable)
                env->DeleteLocalRef(throwable);
            return nullptr;
        }
        if (!javaResult)
            return JSValueMakeUndefined(context);
        JSValueRef jsResult = convertJavaToJS(env, context, javaResult, exception);
        env->DeleteLocalRef(javaResult);
        return jsResult;
    }

    return nullptr;   // 未命中 → 默认行为（含沿构造器原型链查父类静态成员）
}

static bool jniConstructorSetProperty(JSContextRef context, JSObjectRef object, JSStringRef propertyName,
    JSValueRef value, JSValueRef* exception)
{
    auto* classData = static_cast<JniClassData*>(JSObjectGetPrivate(object));
    if (!classData)
        return false;

    WTF::String name = propertyNameToString(propertyName);

    // ① 静态属性（@JSProperty 的 static setter）
    if (JniPropertyEntry* property = classData->findStaticProperty(name)) {
        if (!property->setter)
            return true;   // 只读属性：吞掉写入
        JNIEnv* env = attachAndGetEnv();
        if (!env) {
            if (exception)
                *exception = makeJSError(context, "cannot attach current thread to JVM");
            return true;
        }
        jobject javaValue = convertJSToJava(env, context, value);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            if (exception)
                *exception = makeJSError(context, "cannot convert JS value to Java value");
            return true;
        }
        if (property->setterIsVoid)
            env->CallStaticVoidMethod(classData->javaClass, property->setter, javaValue);
        else
            env->CallStaticObjectMethod(classData->javaClass, property->setter, javaValue);
        if (javaValue)
            env->DeleteLocalRef(javaValue);
        if (env->ExceptionCheck()) {
            jthrowable throwable = env->ExceptionOccurred();
            env->ExceptionClear();
            if (exception)
                *exception = throwable ? makeJSErrorFromThrowable(context, env, throwable)
                                       : makeJSError(context, "java exception");
            if (throwable)
                env->DeleteLocalRef(throwable);
        }
        return true;
    }

    // ② 静态字段（反射 Field.set(null, value)）
    if (JniStaticFieldEntry* field = classData->findStaticField(name)) {
        if (field->isFinal)
            return true;   // final 字段：只读，吞掉写入
        JNIEnv* env = attachAndGetEnv();
        if (!env) {
            if (exception)
                *exception = makeJSError(context, "cannot attach current thread to JVM");
            return true;
        }
        jmethodID setMethod = fieldSetMethodID(env);
        if (!setMethod) {
            if (exception)
                *exception = makeJSError(context, "cannot resolve java.lang.reflect.Field.set");
            return true;
        }
        jobject javaValue = convertJSToJava(env, context, value);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            if (exception)
                *exception = makeJSError(context, "cannot convert JS value to Java value");
            return true;
        }
        env->CallVoidMethod(field->javaField, setMethod, nullptr, javaValue);
        if (javaValue)
            env->DeleteLocalRef(javaValue);
        if (env->ExceptionCheck()) {
            jthrowable throwable = env->ExceptionOccurred();
            env->ExceptionClear();
            if (exception)
                *exception = throwable ? makeJSErrorFromThrowable(context, env, throwable)
                                       : makeJSError(context, "java exception");
            if (throwable)
                env->DeleteLocalRef(throwable);
        }
        return true;
    }

    return false;   // 未命中 → 默认行为
}

// JSAPIWrapperObjectJni.cpp 的回调（GC 内部调用）：标记 holder 的"JS 侧已回收"。
// 无锁、无 JNI —— 只置一个原子标志；holder 的最终释放由引用计数在两个安全点路径完成。
// 调试用：标记"正在释放 context"（确认 owner.finalize 是否在该阶段被调用 —— 决定 holder 的删除策略）
std::atomic<bool> g_releasingContext { false };
std::atomic<long> g_finalizeDuringRelease { 0 };

extern "C" void JSCJniWrapperHolderMarkCollected(void* holderPointer)
{
    auto* holder = static_cast<JniWrapperHolder*>(holderPointer);
    if (!holder)
        return;
    if (g_releasingContext.load(std::memory_order_acquire))
        g_finalizeDuringRelease.fetch_add(1, std::memory_order_relaxed);
    holder->jsCollected.store(true, std::memory_order_release);
    // cell 侧放弃对 holder 的引用（表若已摘除则归零即删除；jweak 已由表摘除路径释放）
    releaseWrapperHolderRef(holder);
}

// 为导出类的实例建 JS 对象：复用 createWrapperWithClass（JS 侧对象是 JSAPIWrapperObject cell；
// instanceClass 提供 @JSProperty 访问器，privateData = classData 供访问器经 JSObjectGetPrivate 取用），
// 并把 [[Prototype]] 指向注册的 prototype。
static JSObjectRef createExportedInstance(JNIEnv* env, JSContextRef context, jobject javaObject, JniClassData* classData)
{
    JSObjectRef jsObject = createWrapperWithClass(env, context, javaObject, classData->instanceClass, classData);
    if (!jsObject)
        return nullptr;
    JSObjectSetPrototype(context, jsObject, classData->prototype);
    return jsObject;
}

// 若 value 属于已注册的导出类，返回其带 JSClass 的实例（同一性优先），否则 nullptr。
// 供 convertJavaToJS 在**所有** Java→JS 边界统一走"完整实例"（4.4 补全）。
static JSObjectRef createExportedInstanceIfRegistered(JNIEnv* env, JSContextRef context, jobject value)
{
    JniClassData* classData = findRegisteredClassForObject(context, value);
    if (!classData)
        return nullptr;
    if (JSObjectRef already = lookupJSWrapper(env, context, value))
        return already;   // 已包装过：同一性优先（不重复建 wrapper）
    return createExportedInstance(env, context, value, classData);
}

static JSValueRef jniMethodCallAsFunction(JSContextRef context, JSObjectRef function, JSObjectRef thisObject,
    size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception)
{
    auto* data = static_cast<JniMethodData*>(JSObjectGetPrivate(function));
    if (!data || !data->methodID) {
        if (exception)
            *exception = makeJSError(context, "JNI exported method is detached");
        return JSValueMakeUndefined(context);
    }

    JNIEnv* env = attachAndGetEnv();
    if (!env) {
        if (exception)
            *exception = makeJSError(context, "cannot attach current thread to JVM");
        return JSValueMakeUndefined(context);
    }

    // this → Java 实例（复用 4.3 的 wrapper 反查）。静态方法不需要 this，
    // 走声明类的 CallStaticObjectMethodA（jclass 在创建函数对象时固定）。
    jobject instance = nullptr;
    if (!data->isStatic) {
        instance = lookupJavaWrapper(env, context, thisObject);
        if (!instance) {
            if (exception)
                *exception = makeJSError(context, "exported method called on a non-Java receiver");
            return JSValueMakeUndefined(context);
        }
    }

    // 参数编组：JS → Java，按引用放进 jvalue[]
    // 注：jvalue 用 malloc 而非 WTF::Vector —— 本版本的 Vector::data() 不是公开接口。
    size_t argumentSlots = argumentCount ? argumentCount : 1;
    jvalue* jargs = static_cast<jvalue*>(::malloc(sizeof(jvalue) * argumentSlots));
    WTF::Vector<jobject> locals;
    locals.reserveInitialCapacity(argumentSlots);
    if (!jargs) {
        env->DeleteLocalRef(instance);
        if (exception)
            *exception = makeJSError(context, "cannot allocate JNI argument array");
        return JSValueMakeUndefined(context);
    }
    for (size_t i = 0; i < argumentCount; ++i) {
        jobject javaArgument = convertJSToJava(env, context, arguments[i]);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            ::free(jargs);
            for (auto local : locals) {
                if (local)
                    env->DeleteLocalRef(local);
            }
            env->DeleteLocalRef(instance);
            if (exception)
                *exception = makeJSError(context, "cannot convert JS argument to Java value");
            return JSValueMakeUndefined(context);
        }
        jargs[i].l = javaArgument;
        locals.append(javaArgument);
    }

    jobject javaResult = data->isStatic
        ? env->CallStaticObjectMethodA(data->javaClass, data->methodID, argumentCount ? jargs : nullptr)
        : env->CallObjectMethodA(instance, data->methodID, argumentCount ? jargs : nullptr);
    ::free(jargs);

    for (auto local : locals) {
        if (local)
            env->DeleteLocalRef(local);
    }
    env->DeleteLocalRef(instance);

    if (env->ExceptionCheck()) {
        jthrowable throwable = env->ExceptionOccurred();
        env->ExceptionClear();
        JSValueRef error = throwable ? makeJSErrorFromThrowable(context, env, throwable)
                                     : makeJSError(context, "java exception");
        if (throwable)
            env->DeleteLocalRef(throwable);
        if (exception)
            *exception = error;
        return JSValueMakeUndefined(context);
    }

    if (!javaResult)
        return JSValueMakeUndefined(context);

    JSValueRef jsResult = convertJavaToJS(env, context, javaResult, exception);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        env->DeleteLocalRef(javaResult);
        if (exception)
            *exception = makeJSError(context, "cannot convert Java return value to JS value");
        return JSValueMakeUndefined(context);
    }
    env->DeleteLocalRef(javaResult);
    return jsResult;
}

static JSObjectRef jniConstructorCallAsConstructor(JSContextRef context, JSObjectRef constructor,
    size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception)
{
    auto* data = static_cast<JniClassData*>(JSObjectGetPrivate(constructor));
    if (!data) {
        if (exception)
            *exception = makeJSError(context, "JNI exported class is detached");
        return nullptr;
    }

    JNIEnv* env = attachAndGetEnv();
    if (!env) {
        if (exception)
            *exception = makeJSError(context, "cannot attach current thread to JVM");
        return nullptr;
    }

    // 路径 1：工厂（复用 4.2 的回调桥 —— 工厂本身就是一个 JS 函数）
    if (data->factory) {
        JSValueRef factoryResult = JSObjectCallAsFunction(context, data->factory, nullptr, argumentCount, arguments, exception);
        if (!factoryResult || (exception && *exception))
            return nullptr;
        JSObjectRef object = JSValueToObject(context, factoryResult, exception);
        if (!object || (exception && *exception)) {
            if (exception && !*exception)
                *exception = makeJSError(context, "factory must return a Java object");
            return nullptr;
        }
        JSObjectSetPrototype(context, object, data->prototype);
        return object;
    }

    // 路径 2：public 无参构造
    if (!data->defaultConstructor) {
        if (exception)
            *exception = makeJSError(context, "class has no public no-arg constructor; pass a factory to registerClass()");
        return nullptr;
    }

    jobject instance = env->NewObject(data->javaClass, data->defaultConstructor);
    if (env->ExceptionCheck()) {
        jthrowable throwable = env->ExceptionOccurred();
        env->ExceptionClear();
        JSValueRef error = throwable ? makeJSErrorFromThrowable(context, env, throwable)
                                     : makeJSError(context, "constructor threw");
        if (throwable)
            env->DeleteLocalRef(throwable);
        if (exception)
            *exception = error;
        return nullptr;
    }
    if (!instance) {
        if (exception)
            *exception = makeJSError(context, "cannot construct Java instance");
        return nullptr;
    }

    // 用本类的 JSClass 创建实例（从而带上属性访问器）并登记进 wrapper 表；
    // createExportedInstance 内部已把 [[Prototype]] 指向注册的 prototype
    JSObjectRef object = createExportedInstance(env, context, instance, data);
    env->DeleteLocalRef(instance);
    if (!object) {
        if (exception)
            *exception = makeJSError(context, "cannot wrap Java instance");
        return nullptr;
    }
    return object;
}

namespace {

// 把「名 → 方法」平行数组装配为函数对象并挂到 target（prototype 或构造器）上。
// isStaticMethod：静态方法走声明类的 CallStaticObjectMethodA（类引用由 jniMethodFinalize 释放）。
// outFunctions 非空时记录所创建的方法函数对象 —— 供 context 释放时回收其 JniMethodData
// 持有的 global ref（静态方法会持声明类的 jclass）。
void installExportedMethods(JNIEnv* env, JSContextRef context, JSObjectRef target,
    jobjectArray namesArray, jobjectArray methodsArray, bool isStaticMethod, jclass declarationClass,
    Vector<JSObjectRef>* outFunctions = nullptr)
{
    jsize methodCount = methodsArray ? env->GetArrayLength(methodsArray) : 0;
    for (jsize i = 0; i < methodCount; ++i) {
        jobject method = env->GetObjectArrayElement(methodsArray, i);
        auto* nameString = static_cast<jstring>(env->GetObjectArrayElement(namesArray, i));
        if (!method || !nameString || env->ExceptionCheck()) {
            env->ExceptionClear();
            if (method)
                env->DeleteLocalRef(method);
            if (nameString)
                env->DeleteLocalRef(nameString);
            continue;
        }

        jmethodID methodID = env->FromReflectedMethod(method);   // java.lang.reflect.Method → jmethodID
        if (env->ExceptionCheck())
            env->ExceptionClear();

        if (methodID) {
            const char* nameUtf8 = env->GetStringUTFChars(nameString, nullptr);
            if (nameUtf8) {
                auto* methodData = new JniMethodData;
                methodData->methodID = methodID;
                methodData->isStatic = isStaticMethod;
                if (isStaticMethod && declarationClass)
                    methodData->javaClass = static_cast<jclass>(env->NewGlobalRef(declarationClass));
                JSObjectRef function = JSObjectMake(context, methodFunctionClass(), methodData);
                if (function) {
                    // 注册表（classData->methodFunctions）会一直持有该函数对象，直到 context 释放时
                    // 用它回收 JniMethodData 里的 global ref。故**必须保活**：函数对象虽然此刻挂在
                    // target（prototype / constructor）上，但 JS 侧一旦 `delete globalThis.X` 或断开
                    // 原型链，整条链就不可达 → 函数被 JS GC 回收 → jniMethodFinalize 释放
                    // JniMethodData → classData->methodFunctions 变成**悬垂 JSObjectRef**，
                    // context 收尾时 JSObjectGetPrivate 读到已释放内存（CheckJNI 报
                    // "use of invalid jobject"）。§6.17 的记忆同样成立。
                    JSValueProtect(context, function);
                    JSStringRef propertyName = JSStringCreateWithUTF8CString(nameUtf8);
                    JSObjectSetProperty(context, target, propertyName, function, kJSPropertyAttributeNone, nullptr);
                    JSStringRelease(propertyName);
                    if (outFunctions)
                        outFunctions->append(function);
                } else {
                    if (methodData->javaClass) {
                        if (JNIEnv* releaseEnv = attachAndGetEnv())
                            releaseEnv->DeleteGlobalRef(methodData->javaClass);
                    }
                    delete methodData;
                }
                env->ReleaseStringUTFChars(nameString, nameUtf8);
            }
        }

        env->DeleteLocalRef(method);
        env->DeleteLocalRef(nameString);
    }
}

// 装配「名 + getter + setter(+void 标记)」平行数组为属性表；isStatic 决定调用方式。
Vector<JniPropertyEntry> collectProperties(JNIEnv* env, jobjectArray propertyNames,
    jobjectArray getters, jobjectArray setters, jbooleanArray setterIsVoid, bool isStatic)
{
    Vector<JniPropertyEntry> result;
    jsize propertyCount = propertyNames ? env->GetArrayLength(propertyNames) : 0;
    for (jsize i = 0; i < propertyCount; ++i) {
        auto* nameString = static_cast<jstring>(env->GetObjectArrayElement(propertyNames, i));
        if (!nameString || env->ExceptionCheck()) {
            env->ExceptionClear();
            if (nameString)
                env->DeleteLocalRef(nameString);
            continue;
        }
        jobject getterMethod = getters ? env->GetObjectArrayElement(getters, i) : nullptr;
        jobject setterMethod = setters ? env->GetObjectArrayElement(setters, i) : nullptr;
        if (env->ExceptionCheck())
            env->ExceptionClear();

        const char* nameUtf8 = env->GetStringUTFChars(nameString, nullptr);
        if (nameUtf8) {
            JniPropertyEntry entry;
            entry.name = WTF::String::fromUTF8(nameUtf8);
            entry.isStatic = isStatic;
            if (getterMethod) {
                entry.getter = env->FromReflectedMethod(getterMethod);
                if (env->ExceptionCheck())
                    env->ExceptionClear();
            }
            if (setterMethod) {
                entry.setter = env->FromReflectedMethod(setterMethod);
                if (env->ExceptionCheck())
                    env->ExceptionClear();
            }
            if (setterIsVoid) {
                jboolean isVoid = JNI_FALSE;
                env->GetBooleanArrayRegion(setterIsVoid, i, 1, &isVoid);
                entry.setterIsVoid = (isVoid == JNI_TRUE);
            }
            if (entry.getter || entry.setter)
                result.append(WTF::move(entry));
            env->ReleaseStringUTFChars(nameString, nameUtf8);
        }

        if (getterMethod)
            env->DeleteLocalRef(getterMethod);
        if (setterMethod)
            env->DeleteLocalRef(setterMethod);
        env->DeleteLocalRef(nameString);
    }
    return result;
}

} // namespace

extern "C" JNIEXPORT void JNICALL Java_org_webkit_jni_JSCContext_nativeRegisterClass(JNIEnv* env, jobject,
    jlong handle, jclass javaClass, jstring jsName, jclass superClass, jobject factory,
    jobjectArray methodNames, jobjectArray methods, jobjectArray propertyNames,
    jobjectArray getters, jobjectArray setters, jbooleanArray setterIsVoid,
    jobjectArray staticMethodNames, jobjectArray staticMethods,
    jobjectArray staticPropertyNames, jobjectArray staticGetters, jobjectArray staticSetters,
    jbooleanArray staticSetterIsVoid,
    jobjectArray staticFieldNames, jobjectArray staticFields, jbooleanArray staticFieldFinal)
{
    if (!handle || !javaClass || !jsName)
        return;
    JSContextRef context = reinterpret_cast<JSContextRef>(handle);

    // 1) prototype 对象
    JSObjectRef prototype = JSObjectMake(context, nullptr, nullptr);
    if (!prototype)
        return;

    // 2) 把导出方法挂到 prototype 上（方法函数带 jmethodID 私有数据）
    Vector<JSObjectRef> methodFunctions;
    installExportedMethods(env, context, prototype, methodNames, methods, false, nullptr, &methodFunctions);

    // 3) 类数据 + 实例的 JSClass + 属性表
    auto* classData = new JniClassData;
    classData->methodFunctions = WTF::move(methodFunctions);
    classData->javaClass = static_cast<jclass>(env->NewGlobalRef(javaClass));
    classData->prototype = prototype;
    // 保活（收尾时成对 JSValueUnprotect）：prototype 通常经 globalThis[X].prototype 可达，
    // 但 JS 侧 `delete globalThis.X` 后即失去根 —— 届时它会被 GC 回收，而注册表仍持有其指针。
    JSValueProtect(context, prototype);
    classData->defaultConstructor = env->GetMethodID(javaClass, "<init>", "()V");
    if (!classData->defaultConstructor && env->ExceptionCheck())
        env->ExceptionClear();
    if (factory) {
        // createJSFunctionFromCallback 返回 JSValueRef（const 指针），这里需要非 const 的 JSObjectRef
        classData->factory = const_cast<JSObjectRef>(createJSFunctionFromCallback(env, context, factory));
        // 保活（关键）：工厂**只**存在注册表里，JS 侧没有任何引用指向它 ——
        // 不保活则 JS GC 随时可回收它，其 finalize 会 DeleteGlobalRef 并释放 JniCallbackData，
        // 而 classData->factory 仍指向已回收对象 → 收尾时读到垃圾 jobject。
        if (classData->factory)
            JSValueProtect(context, classData->factory);
    }

    // 实例用的 JSClass：属性访问器挂在类上（getProperty / setProperty），
    // 实例的私有数据即 classData —— 访问器因此无需查表。
    // 刻意不设 finalize：classData 由注册表持有，其生命周期与 context 绑定。
    {
        JSClassDefinition instanceDefinition = kJSClassDefinitionEmpty;
        instanceDefinition.className = "JSCJniExportedInstance";
        instanceDefinition.getProperty = jniInstanceGetProperty;
        instanceDefinition.setProperty = jniInstanceSetProperty;
        // 与普通 wrapper 同一回收通道：JS GC 回收导出实例时记录，让表由 JS GC 驱动清理
        instanceDefinition.finalize = recordCollectedWrapper;
        classData->instanceClass = JSClassCreate(&instanceDefinition);
    }

    // 属性表（@JSProperty）：实例属性 + 静态属性（静态的由构造器的访问器使用）
    classData->properties = collectProperties(env, propertyNames, getters, setters, setterIsVoid, false);
    classData->staticProperties = collectProperties(env, staticPropertyNames, staticGetters, staticSetters,
        staticSetterIsVoid, true);

    // 静态字段（@JSExport 的 public static 字段）：读/写走 java.lang.reflect.Field（统一 Object）
    jsize staticFieldCount = staticFieldNames ? env->GetArrayLength(staticFieldNames) : 0;
    for (jsize i = 0; i < staticFieldCount; ++i) {
        auto* nameString = static_cast<jstring>(env->GetObjectArrayElement(staticFieldNames, i));
        jobject field = staticFields ? env->GetObjectArrayElement(staticFields, i) : nullptr;
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            if (nameString)
                env->DeleteLocalRef(nameString);
            if (field)
                env->DeleteLocalRef(field);
            continue;
        }
        const char* nameUtf8 = nameString ? env->GetStringUTFChars(nameString, nullptr) : nullptr;
        if (nameUtf8 && field) {
            JniStaticFieldEntry entry;
            entry.name = WTF::String::fromUTF8(nameUtf8);
            entry.javaField = env->NewGlobalRef(field);
            if (staticFieldFinal) {
                jboolean isFinal = JNI_FALSE;
                env->GetBooleanArrayRegion(staticFieldFinal, i, 1, &isFinal);
                entry.isFinal = (isFinal == JNI_TRUE);
            }
            if (entry.javaField)
                classData->staticFields.append(WTF::move(entry));
        }
        if (nameUtf8)
            env->ReleaseStringUTFChars(nameString, nameUtf8);
        if (field)
            env->DeleteLocalRef(field);
        if (nameString)
            env->DeleteLocalRef(nameString);
    }

    JSObjectRef constructor = JSObjectMake(context, constructorFunctionClass(), classData);
    if (!constructor) {
        // 回滚：先撤销前面已建立的保活（否则保活计数永久残留），再释放 JNI 引用
        for (auto function : classData->methodFunctions) {
            if (function)
                JSValueUnprotect(context, function);
        }
        if (classData->factory)
            JSValueUnprotect(context, classData->factory);
        if (classData->prototype)
            JSValueUnprotect(context, classData->prototype);
        if (classData->javaClass)
            env->DeleteGlobalRef(classData->javaClass);
        delete classData;
        return;
    }

    // constructor.prototype = prototype（用例 Object.getPrototypeOf(B.prototype) === A.prototype 依赖它）
    JSStringRef prototypeName = JSStringCreateWithUTF8CString("prototype");
    JSObjectSetProperty(context, constructor, prototypeName, prototype, kJSPropertyAttributeNone, nullptr);
    classData->constructor = constructor;
    // 保活（收尾时成对 JSValueUnprotect）：构造器通常经 globalThis[X] 可达，但 JS 侧 delete 后失去根。
    JSValueProtect(context, constructor);

    // 静态方法挂到构造器上（JS 侧：ClassName.staticMethod(...)）
    installExportedMethods(env, context, constructor, staticMethodNames, staticMethods, true, javaClass,
        &classData->methodFunctions);

    // 4) globalThis[simpleName] = constructor
    const char* classNameUtf8 = env->GetStringUTFChars(jsName, nullptr);
    if (classNameUtf8) {
        JSStringRef globalName = JSStringCreateWithUTF8CString(classNameUtf8);
        JSObjectRef globalObject = JSContextGetGlobalObject(context);
        JSObjectSetProperty(context, globalObject, globalName, constructor, kJSPropertyAttributeNone, nullptr);
        JSStringRelease(globalName);
        env->ReleaseStringUTFChars(jsName, classNameUtf8);
    }
    JSStringRelease(prototypeName);

    // 5) 原型链：父类已注册则逐级连接，并把父类的属性访问器并入本类
    //（实例用「本类」的 JSClass 创建，属性访问器查的是本类的表 —— 若不合并，
    //  子类实例将读不到父类声明的 @JSProperty）
    if (superClass) {
        Locker locker { classTableLock() };
        if (JniClassData* superData = findRegisteredClass(context, superClass)) {
            JSObjectSetPrototype(context, prototype, superData->prototype);
            for (auto& superProperty : superData->properties) {
                if (!classData->findProperty(superProperty.name))
                    classData->properties.append(superProperty);
            }
            // 静态成员继承：B 的构造器 [[Prototype]] 指向 A 的构造器（对应 ES6 setPrototypeOf(B, A)），
            // 于是 B.staticMethod / B.staticField 能沿构造器原型链查到父类的静态成员。
            if (superData->constructor)
                JSObjectSetPrototype(context, constructor, superData->constructor);
        }
    }

    // 6) 登记（表随 context 常驻）
    {
        Locker locker { classTableLock() };
        classTables().ensure(context, [] { return ContextClassTable { }; })
            .iterator->value.byJavaClass.set(classData->javaClass, classData);
    }
}

#include <wtf/MainThread.h>               // WTF::initializeMainThread
#include <wtf/MemoryPressureHandler.h>   // 阶段 5：内存压力入口需要完整类型

// ---------------------------------------------------------------------------
// 内存压力入口（阶段 5 验证矩阵的「内存压力」项）
//
// 背景（本轮实测确认）：unix 实现的 `MemoryPressureHandler::install()` 只置一个标志、
// **不注册任何信号源**（`WTF/wtf/unix/MemoryPressureHandlerUnix.cpp:87-95`）。因此 Android 上
// 真实的内存压力必须由宿主转发 —— APK 场景在 `ComponentCallbacks2.onTrimMemory()` 里调用
// `JSCVirtualMachine.notifyMemoryPressure(critical)`。
//
// 为使链路可观测，首次调用时打开 WTF 的压力日志并装一个计数回调：
//   Java notifyMemoryPressure → triggerMemoryPressureEvent → 计数 + WTF 日志
//
// 注意：首次调用会先做 WTF 主线程初始化（见下），这是 install() 的前提。
// ---------------------------------------------------------------------------

namespace {

std::atomic<unsigned> g_memoryPressureEventCount { 0 };   // 压力回调可能由任意线程触发
bool g_memoryPressureObservabilityInstalled = false;

WTF::Lock& memoryPressureInstallLock()
{
    static NeverDestroyed<WTF::Lock> lock;
    return lock.get();
}

void ensureMemoryPressureObservability()
{
    // 幂等 + 线程安全：notify / isUnderMemoryPressure / eventCount 三个入口都可能首次触发。
    //
    // APK 集成实测（2026-10-11）：宿主是「先查状态、后转发压力」的顺序，而旧实现只在 notify
    // 路径做初始化 → 查状态直接走到未初始化的 MemoryPressureHandler::singleton()，
    // 构造期解引用空的 RunLoop::mainSingleton() → SIGSEGV（fault addr 0x8，tombstone 栈为
    // nativeIsUnderMemoryPressure → WTF::MemoryPressureHandler）。开发期 app_process 的
    // 验证程序恰好先调 notify，掩盖了该顺序依赖。
    Locker locker { memoryPressureInstallLock() };
    if (g_memoryPressureObservabilityInstalled)
        return;
    g_memoryPressureObservabilityInstalled = true;
    // install() 会构造 holdOffTimer（依赖 RunLoop::mainSingleton()），因此必须先确保 WTF
    // 主线程已初始化 —— 否则会解引用空的 s_mainRunLoop 而 SIGSEGV（本轮实测踩到）。
    // JSC 的 C API 路径不替调用方做这件事（jsc shell 自己会做）。
    WTF::initializeMainThread();
    WTF::MemoryPressureHandler::ReliefLogger::setLoggingEnabled(true);
    auto& handler = WTF::MemoryPressureHandler::singleton();
    handler.setMemoryPressureStatusChangedCallback([] {
        ++g_memoryPressureEventCount;
    });
    handler.install();
}

} // namespace

extern "C" JNIEXPORT void JNICALL Java_org_webkit_jni_JSCVirtualMachine_nativeNotifyMemoryPressure(JNIEnv*, jobject, jboolean critical)
{
    ensureMemoryPressureObservability();
    WTF::MemoryPressureHandler::singleton().triggerMemoryPressureEvent(critical == JNI_TRUE);
}

// 观测：压力状态变化回调被调用的次数（验证程序据此断言链路成立）
extern "C" JNIEXPORT jlong JNICALL Java_org_webkit_jni_JSCVirtualMachine_nativeMemoryPressureEventCount(JNIEnv*, jobject)
{
    ensureMemoryPressureObservability();   // 与 notify 同一前置：查询前先确保观测已安装
    return static_cast<jlong>(g_memoryPressureEventCount.load());
}

extern "C" JNIEXPORT jboolean JNICALL Java_org_webkit_jni_JSCVirtualMachine_nativeIsUnderMemoryPressure(JNIEnv*, jobject)
{
    // ← 此前缺失该守卫：APK 场景"先查状态"会踩未初始化的 singleton（SIGSEGV，见上）
    ensureMemoryPressureObservability();
    return WTF::MemoryPressureHandler::singleton().isUnderMemoryPressure() ? JNI_TRUE : JNI_FALSE;
}

// ---------------------------------------------------------------------------
// JSCJniWeakValue：JS 侧弱引用 + GC 通知（阶段 4.4 补全）
//
// 对应 GLib 的 JSCWeakValue（API/glib/JSCWeakValue.cpp）：
// 观察一个 JS 对象何时被 JS GC 回收，并把"已清除（cleared）"状态（可选回调）送回 Java。
//
// 机制（全部为上游现成机制；不新增 Heap 宏、不新增 cell 类）：
//   · Java 对象 → 建一个 JS 包装对象（**不做 JSValueProtect** —— 存活完全由 JS 侧引用决定），
//     并一步写入 globalThis[name]（避免"创建到暴露之间被 GC 回收"的窗口）
//   · `JSC::Weak<JSC::JSObject>` + 自定义 WeakHandleOwner：
//       JS GC 判定该包装不可达 → owner.finalize（GC 内部调用：持 apiLock、world-stopped）
//       finalize 里只做三件事：置 cleared、归还 weak slot、把 data 挂进无锁队列
//       —— **绝不做 JNI 调用**（GC 中做 JNI 有死锁/阻塞风险）
//   · 通知在"JNI 安全点"派发（garbageCollect / get / isCleared 入口）：排空队列 → 回调 Java
//
// 生命周期：data 由 Java 句柄持有；JS 侧被回收后 data 仍存活到通知派发完成且句柄关闭。
// close() 只置 released 标志：
//   · 通知已排空（Drained）→ 立即销毁（finalize 不会再来）
//   · 仍 Alive / Pending → 由后续 finalize / 排空路径销毁
// ---------------------------------------------------------------------------

namespace {

enum class WeakValueState : int { Alive = 0, Pending = 1, Drained = 2 };

struct JniWeakValueData {
    jobject targetWeak { nullptr };         // 目标 Java 对象（弱全局引用）
    jobject callbackGlobalRef { nullptr };  // 可选：cleared 通知回调（Runnable 的强引用）
    jmethodID callbackMethod { nullptr };
    JSContextRef context { nullptr };
    JSC::Weak<JSC::JSObject> jsWeak;        // JS 侧弱引用（slot 由 finalize 归还）
    std::atomic<bool> cleared { false };
    std::atomic<bool> released { false };   // Java 侧已 close
    std::atomic<WeakValueState> state { WeakValueState::Alive };
    std::atomic<JniWeakValueData*> pendingNext { nullptr };
};

std::atomic<JniWeakValueData*>& pendingWeakValueHead()
{
    static std::atomic<JniWeakValueData*> head { nullptr };
    return head;
}

// GC 线程调用（无锁 push 到单链表；不做任何 JNI）
void enqueueWeakValueNotification(JniWeakValueData* data)
{
    JniWeakValueData* oldHead = pendingWeakValueHead().load(std::memory_order_relaxed);
    do {
        data->pendingNext.store(oldHead, std::memory_order_relaxed);
    } while (!pendingWeakValueHead().compare_exchange_weak(oldHead, data,
        std::memory_order_release, std::memory_order_relaxed));
}

// 弱值 data 的 per-context 登记表：context 释放时兜底回收（宿主可能未 close 句柄）。
// 由 classTableLock() 保护（与类注册表共用同一把锁 —— 二者都属"per-context 桥接资源"）。
HashMap<JSContextRef, Vector<JniWeakValueData*>>& weakValueTables()
{
    static NeverDestroyed<HashMap<JSContextRef, Vector<JniWeakValueData*>>> tables;
    return tables.get();
}

// 销毁 data（必须在 JNI 安全点调用；调用后指针失效）
void destroyWeakValueData(JniWeakValueData* data)
{
    if (!data)
        return;

    // 从 per-context 登记表注销（列表很短，线性查找足够）
    if (JSContextRef context = data->context) {
        Locker locker { classTableLock() };
        auto it = weakValueTables().find(context);
        if (it != weakValueTables().end()) {
            auto& list = it->value;
            for (size_t i = 0; i < list.size(); ++i) {
                if (list[i] == data) {
                    list.removeAt(i);
                    break;
                }
            }
            if (list.isEmpty())
                weakValueTables().remove(it);
        }
    }

    if (JNIEnv* env = attachAndGetEnv()) {
        if (data->targetWeak) {
            env->DeleteWeakGlobalRef(data->targetWeak);
            data->targetWeak = nullptr;
        }
        if (data->callbackGlobalRef) {
            env->DeleteGlobalRef(data->callbackGlobalRef);
            data->callbackGlobalRef = nullptr;
        }
    }
    data->jsWeak.clear();   // 幂等：slot 已归还则为空操作
    delete data;
}

// 排空通知队列（各 JNI 安全点入口调用）：派发回调 → 释放回调引用 → 按需销毁 data
void drainWeakValueNotifications()
{
    JniWeakValueData* head = pendingWeakValueHead().exchange(nullptr, std::memory_order_acquire);
    if (!head)
        return;

    // 反转链表，恢复入队顺序
    JniWeakValueData* reversed = nullptr;
    while (head) {
        JniWeakValueData* next = head->pendingNext.load(std::memory_order_relaxed);
        head->pendingNext.store(reversed, std::memory_order_relaxed);
        reversed = head;
        head = next;
    }

    JNIEnv* env = attachAndGetEnv();
    for (JniWeakValueData* data = reversed; data; ) {
        JniWeakValueData* next = data->pendingNext.load(std::memory_order_relaxed);
        data->state.store(WeakValueState::Drained, std::memory_order_release);

        if (env && data->callbackGlobalRef && data->callbackMethod) {
            env->CallVoidMethod(data->callbackGlobalRef, data->callbackMethod);
            if (env->ExceptionCheck())
                env->ExceptionClear();
        }
        if (data->callbackGlobalRef) {
            if (env)
                env->DeleteGlobalRef(data->callbackGlobalRef);
            data->callbackGlobalRef = nullptr;
        }
        if (data->released.load(std::memory_order_acquire))
            destroyWeakValueData(data);
        data = next;
    }
}

} // namespace

// WeakHandleOwner：JS 包装被 GC 判定不可达时由 GC 调用（此回调内不可做 JNI）
class JniWeakValueHandleOwner final : public JSC::WeakHandleOwner {
public:
    void finalize(JSC::Handle<JSC::Unknown> handle, void* context) final
    {
        auto* data = static_cast<JniWeakValueData*>(context);
        if (!data) {
            JSC::WeakSet::deallocate(JSC::WeakImpl::asWeakImpl(handle.slot()));
            return;
        }
        data->cleared.store(true, std::memory_order_release);
        data->jsWeak.clear();   // 归还 weak slot（WeakImpl 的 deallocate 在此完成）
        if (data->state.exchange(WeakValueState::Pending, std::memory_order_acq_rel) == WeakValueState::Alive)
            enqueueWeakValueNotification(data);
    }

    bool isReachableFromOpaqueRoots(JSC::Handle<JSC::Unknown>, void*, JSC::AbstractSlotVisitor&, ASCIILiteral*) final
    {
        // JNI 桥没有 ObjC 式的"外部对象图"可扫描：JVM 侧持有不构成 JS GC 的 root。
        //（需要跨 GC 保活 Java 对象的场景由 managed reference 显式表达。）
        return false;
    }
};

namespace {

JniWeakValueHandleOwner& jniWeakValueHandleOwner()
{
    static NeverDestroyed<JniWeakValueHandleOwner> owner;
    return owner.get();
}

// 检测 JS 侧是否已回收 —— **不依赖 owner.finalize 的时机**（实测 finalize 可能被延迟到
// 很晚或下一轮 GC 之后；而 weak 的空值状态在回收当轮即生效）。若已回收则补记 cleared
// 并入队通知。必须在 JNI 安全点调用；内部取 apiLock，保证读取 WeakImpl 无竞态。
// 注意：不在此归还 weak slot —— slot 仍由 owner.finalize 释放（避免双重 deallocate）。
void ensureWeakValueCollected(JniWeakValueData* data)
{
    if (!data || data->cleared.load(std::memory_order_acquire))
        return;
    JSContextRef context = data->context;
    if (!context)
        return;
    JSC::JSGlobalObject* globalObject = toJS(context);
    if (!globalObject)
        return;

    bool collected = false;
    {
        JSC::JSLockHolder locker(globalObject->vm());
        collected = !data->jsWeak.get();
    }
    if (!collected)
        return;

    data->cleared.store(true, std::memory_order_release);
    if (data->state.exchange(WeakValueState::Pending, std::memory_order_acq_rel) == WeakValueState::Alive)
        enqueueWeakValueNotification(data);
}

} // namespace

extern "C" JNIEXPORT jlong JNICALL Java_org_webkit_jni_JSCJniWeakValue_nativeCreate(JNIEnv* env, jclass,
    jlong contextHandle, jstring globalName, jobject target, jobject clearedCallback)
{
    if (!contextHandle || !target)
        return 0;

    JSContextRef context = reinterpret_cast<JSContextRef>(contextHandle);
    JSC::JSGlobalObject* globalObject = toJS(context);
    if (!globalObject)
        return 0;

    JSC::JSLockHolder locker(globalObject->vm());

    // JS 包装对象：不受保护（生命周期完全由 JS 侧引用决定）
    JSObjectRef wrapper = JSObjectMake(context, nullptr, nullptr);
    if (!wrapper)
        return 0;

    // 一步暴露给 JS：globalThis[globalName] = wrapper
    if (globalName) {
        const char* nameUtf8 = env->GetStringUTFChars(globalName, nullptr);
        if (nameUtf8) {
            JSStringRef propertyName = JSStringCreateWithUTF8CString(nameUtf8);
            JSObjectRef globalThis = JSContextGetGlobalObject(context);
            JSObjectSetProperty(context, globalThis, propertyName, wrapper, kJSPropertyAttributeNone, nullptr);
            JSStringRelease(propertyName);
            env->ReleaseStringUTFChars(globalName, nameUtf8);
        }
    }

    auto* data = new JniWeakValueData;
    data->context = context;
    data->targetWeak = env->NewWeakGlobalRef(target);
    if (!data->targetWeak) {
        delete data;
        return 0;
    }
    if (clearedCallback) {
        data->callbackGlobalRef = env->NewGlobalRef(clearedCallback);
        jclass runnableClass = env->FindClass("java/lang/Runnable");
        if (runnableClass) {
            data->callbackMethod = env->GetMethodID(runnableClass, "run", "()V");
            env->DeleteLocalRef(runnableClass);
        }
        if (env->ExceptionCheck())
            env->ExceptionClear();
    }

    JSC::Weak<JSC::JSObject> weak(toJS(wrapper), &jniWeakValueHandleOwner(), data);
    data->jsWeak.swap(weak);

    // 登记到 per-context 表（context 释放时兜底回收）
    {
        Locker locker { classTableLock() };
        weakValueTables().ensure(context, [] { return Vector<JniWeakValueData*> { }; }).iterator->value.append(data);
    }
    return reinterpret_cast<jlong>(data);
}

extern "C" JNIEXPORT jobject JNICALL Java_org_webkit_jni_JSCJniWeakValue_nativeGet(JNIEnv* env, jobject, jlong handle)
{
    auto* data = reinterpret_cast<JniWeakValueData*>(handle);
    if (!data)
        return nullptr;
    ensureWeakValueCollected(data);  // 补检测（不依赖 finalize 时机）
    drainWeakValueNotifications();   // 顺手派发待处理通知（安全点）
    if (data->cleared.load(std::memory_order_acquire) || !data->targetWeak)
        return nullptr;
    return env->NewLocalRef(data->targetWeak);
}

extern "C" JNIEXPORT jboolean JNICALL Java_org_webkit_jni_JSCJniWeakValue_nativeIsCleared(JNIEnv*, jobject, jlong handle)
{
    auto* data = reinterpret_cast<JniWeakValueData*>(handle);
    if (!data)
        return JNI_TRUE;
    ensureWeakValueCollected(data);  // 补检测（不依赖 finalize 时机）
    drainWeakValueNotifications();   // 顺手派发待处理通知（安全点）
    return data->cleared.load(std::memory_order_acquire) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL Java_org_webkit_jni_JSCJniWeakValue_nativeRelease(JNIEnv*, jobject, jlong handle)
{
    auto* data = reinterpret_cast<JniWeakValueData*>(handle);
    if (!data)
        return;
    data->released.store(true, std::memory_order_release);
    ensureWeakValueCollected(data);   // 已回收则入队，可在本轮排空中被销毁
    drainWeakValueNotifications();
    // 若仍未回收（对象在 JS 侧仍被引用）：data 保留到 owner.finalize 之后再销毁
    //（finalize 会入队，由后续任一安全点的排空路径按 released 标志清理）。
    if (data->state.load(std::memory_order_acquire) == WeakValueState::Drained)
        destroyWeakValueData(data);
}

// ---------------------------------------------------------------------------
// context 释放时的桥接资源回收（由 nativeRelease 调用）
//
// 解决的问题：wrapper 表 / 类注册表 / 弱值登记表都是"按 context 常驻"的进程级结构，
// 此前 nativeRelease 只调 JSGlobalContextRelease —— 表条目、JniClassData、JSClassRef、
// jclass/jobject global ref、jweak 全部随 context 销毁而泄漏。
//
// 顺序：先在锁内把三张表中该 context 的条目"取出并移除"（避免与并发访问打架），
// 再在锁外逐项释放，且全部在 JSGlobalContextRelease **之前**完成。
// 另需注意：WeakImpl 归还（Weak::clear）需要 apiLock —— 与 cell 分配同一教训。
// ---------------------------------------------------------------------------
static void releaseContextResourcesLocked(JNIEnv* env, JSContextRef context)
{
    g_releasingContext.store(true, std::memory_order_release);
    struct ReleaseScopeGuard {
        ~ReleaseScopeGuard() { g_releasingContext.store(false, std::memory_order_release); }
    } releaseScopeGuard;

    Vector<JavaWrapper> wrappers;
    Vector<JniClassData*> classes;
    Vector<JniWeakValueData*> weakValues;

    {
        Locker locker { wrapperLock() };
        auto it = wrapperTables().find(context);
        if (it != wrapperTables().end()) {
            wrappers = WTF::move(it->value.entries);
            wrapperTables().remove(it);
        }
    }
    {
        Locker locker { classTableLock() };
        auto classIt = classTables().find(context);
        if (classIt != classTables().end()) {
            for (auto& entry : classIt->value.byJavaClass)
                classes.append(entry.value);
            classTables().remove(classIt);
        }
        auto weakIt = weakValueTables().find(context);
        if (weakIt != weakValueTables().end()) {
            weakValues = WTF::move(weakIt->value);
            weakValueTables().remove(weakIt);
        }
    }

    // 1) wrapper 表：释放 Java 强引用 → 归还 holder 的表侧引用
    for (auto& entry : wrappers) {
        JniWrapperHolder* holder = entry.holder;
        if (holder) {
            if (holder->javaRef) {
                env->DeleteGlobalRef(holder->javaRef);
                holder->javaRef = nullptr;
            }
            // context 释放阶段**不会**触发 owner.finalize（实测计数为 0，见 §6.17），
            // 且 cell 随 VM 一同销毁、不会再访问 wrappedObject —— 因此这里直接释放 holder。
            freeWrapperHolder(holder);
        }
    }

    // 2) 类注册表：方法函数（静态方法持声明类 global ref）、工厂回调、静态字段、JSClass、jclass
    //    注意：注册时对 methodFunctions / factory / prototype / constructor 都调了 JSValueProtect，
    //    此处必须**成对 JSValueUnprotect**（否则这些 JS 对象会随 context 一起被销毁时残留保活计数）。
    //    同时 JSObjectGetPrivate 也只有在对象仍存活时才安全 —— 保活正是其前提（见 §7.4.1 的缺陷记录）。
    for (auto* classData : classes) {
        if (!classData)
            continue;
        for (auto function : classData->methodFunctions) {
            if (!function)
                continue;
            // 只清 global ref（结构体由 jniMethodFinalize 释放，避免双重释放/UAF）
            if (auto* methodData = static_cast<JniMethodData*>(JSObjectGetPrivate(function))) {
                if (methodData->javaClass) {
                    env->DeleteGlobalRef(methodData->javaClass);
                    methodData->javaClass = nullptr;
                }
            }
            JSValueUnprotect(context, function);
        }
        if (classData->factory) {
            if (auto* callbackData = static_cast<JniCallbackData*>(JSObjectGetPrivate(classData->factory))) {
                if (callbackData->javaGlobalRef) {
                    env->DeleteGlobalRef(callbackData->javaGlobalRef);
                    callbackData->javaGlobalRef = nullptr;
                }
            }
            JSValueUnprotect(context, classData->factory);
            classData->factory = nullptr;
        }
        for (auto& field : classData->staticFields) {
            if (field.javaField) {
                env->DeleteGlobalRef(field.javaField);
                field.javaField = nullptr;
            }
        }
        if (classData->prototype) {
            JSValueUnprotect(context, classData->prototype);
            classData->prototype = nullptr;
        }
        if (classData->constructor) {
            JSValueUnprotect(context, classData->constructor);
            classData->constructor = nullptr;
        }
        if (classData->instanceClass)
            JSClassRelease(classData->instanceClass);
        if (classData->javaClass)
            env->DeleteGlobalRef(classData->javaClass);
        delete classData;
    }

    // 3) 弱值 data：先归还 weak slot（此后 owner.finalize 不会再被调用）→ 释放引用 → 删除
    for (auto* data : weakValues) {
        if (!data)
            continue;
        data->jsWeak.clear();
        if (data->targetWeak) {
            env->DeleteWeakGlobalRef(data->targetWeak);
            data->targetWeak = nullptr;
        }
        if (data->callbackGlobalRef) {
            env->DeleteGlobalRef(data->callbackGlobalRef);
            data->callbackGlobalRef = nullptr;
        }
        delete data;
    }
}

void releaseContextResources(JNIEnv* env, JSContextRef context)
{
    if (!env || !context)
        return;
    // WeakImpl 归还（Weak::clear）/ Unprotect 都要求 apiLock —— 见 §6.16 的教训
    if (JSC::JSGlobalObject* globalObject = toJS(context)) {
        JSC::JSLockHolder locker(globalObject->vm());
        releaseContextResourcesLocked(env, context);
    }
}

// 观测：进程内已注册类 / 存活弱值的数量（泄漏与回收观测用）
extern "C" JNIEXPORT jlong JNICALL Java_org_webkit_jni_JSCContext_nativeRegisteredClassCount(JNIEnv*, jclass)
{
    Locker locker { classTableLock() };
    size_t total = 0;
    for (auto& entry : classTables())
        total += entry.value.byJavaClass.size();
    return static_cast<jlong>(total);
}

extern "C" JNIEXPORT jlong JNICALL Java_org_webkit_jni_JSCContext_nativeLiveWeakValueCount(JNIEnv*, jclass)
{
    Locker locker { classTableLock() };
    size_t total = 0;
    for (auto& entry : weakValueTables())
        total += entry.value.size();
    return static_cast<jlong>(total);
}

// 调试观测：context 释放阶段出现的 owner.finalize 次数（0 = 该阶段不触发 finalize）
extern "C" JNIEXPORT jlong JNICALL Java_org_webkit_jni_JSCContext_nativeFinalizeDuringReleaseCount(JNIEnv*, jclass)
{
    return static_cast<jlong>(g_finalizeDuringRelease.load(std::memory_order_relaxed));
}

// 观测：当前存活的 wrapper holder 数（累计分配 − 累计释放）。context 释放后应回到基线 ——
// 这是"释放路径不泄漏 holder"的直接证据。
extern "C" JNIEXPORT jlong JNICALL Java_org_webkit_jni_JSCContext_nativeLiveWrapperHolderCount(JNIEnv*, jclass)
{
    long allocated = g_holderAllocCount.load(std::memory_order_relaxed);
    long freed = g_holderFreeCount.load(std::memory_order_relaxed);
    return static_cast<jlong>(allocated - freed);
}

#endif // defined(JSC_JNI_API_ENABLED)
