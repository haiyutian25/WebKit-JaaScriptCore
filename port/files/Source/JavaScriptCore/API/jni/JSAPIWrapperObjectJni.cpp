/*
 * Copyright (C) 2026 The WebKit Android JSC port authors.
 *
 * JNI 桥接层 —— 复用 JSAPIWrapperObject cell 类（阶段 4.4 补全）。
 *
 * 本文件是 JSAPIWrapperObject 的第三个后端实现（另两个为 ObjC 的 JSAPIWrapperObject.mm
 * 与 GLib 的 JSAPIWrapperObjectGLib.cpp），结构照 GLib 版：
 *   · 同一 cell 类（JSAPIWrapperObject，含 wrappedObject 槽）
 *   · 同一模板载体（JSCallbackObject<JSAPIWrapperObject>，其 s_info/needsDestruction/
 *     四个 host function 特化、createStructure、subspaceForImpl 在此定义）
 *   · 生命周期由 JS GC 驱动：finishCreation 里 WeakSet::allocate，owner.finalize 归还 slot
 *
 * 与 GLib/ObjC 的差异（Android 无对应物，属预期语义降级）：
 *   · ObjC 的 external object graph（scanExternalObjectGraph，JS 可达 ⇒ native 对象保活）依赖
 *     ObjC runtime 的对象图扫描，Android 不可移植 —— visitChildren 只做 Base 版本（同 GLib）。
 *   · ObjC 的 setWrappedObject 做 [id retain]、finalize 做 releaseSoon(adoptNS(...))；本后端
 *     wrappedObject 存的是 JSCJni.cpp 管理的 holder 指针，finalize 里只调用
 *     JSCJniWrapperHolderMarkCollected()（置一个原子标志，无锁、无 JNI）——
 *     holder 的实际释放（DeleteWeakGlobalRef 等）由 JSCJni.cpp 在 JNI 安全点完成。
 *   · releaseSoon 通道未复用（那需要再改 Heap.h/HeapInlines.h/Heap.cpp 三处）；改用
 *     "原子标志 + 安全点统一清理"，避免在 GC 内部做任何 JNI 调用。
 *
 * 构建登记见 JNI.cmake；三后端（OBJC/GLIB/JNI）在配置上必须互斥（模板特化不可重复定义）。
 */

#include "config.h"
#include "JSAPIWrapperObject.h"

#if defined(JSC_JNI_API_ENABLED)

#include "AbstractSlotVisitor.h"
#include "APICast.h"
#include "JavaScript.h"
#include "JSCInlines.h"
#include "JSCallbackObject.h"
#include "JSLock.h"
#include "Structure.h"
#include <wtf/NeverDestroyed.h>

// 定义在 JSCJni.cpp：标记 holder 的"JS 侧已回收"标志（无锁、无 JNI —— GC 线程安全）。
extern "C" void JSCJniWrapperHolderMarkCollected(void* holder);

// 定义在 JSCJni.cpp：外部对象图查询（**只读身份哈希，不做 JNI** —— GC 期间唯一安全的形式）。
// 语义对齐 ObjC 的 `scanExternalObjectGraph`（API/JSVirtualMachine.mm:317）。
extern "C" uint32_t JSCJniWrapperHolderIdentityHash(void* wrappedObject);
extern "C" void JSCJniScanGraphFrom(JSC::AbstractSlotVisitor& visitor, uint32_t rootHash);

namespace {

// 身份哈希 ↔ opaque root 指针的互换（opaque root 接受任意指针，用哈希值充当标识）。
inline void* opaqueRootForHash(uint32_t hash)
{
    return reinterpret_cast<void*>(static_cast<uintptr_t>(hash));
}

} // namespace

class JniAPIWrapperObjectHandleOwner final : public JSC::WeakHandleOwner {
public:
    void finalize(JSC::Handle<JSC::Unknown>, void*) final;
    bool isReachableFromOpaqueRoots(JSC::Handle<JSC::Unknown>, void*, JSC::AbstractSlotVisitor&, ASCIILiteral*) final;
};

static JniAPIWrapperObjectHandleOwner* jniAPIWrapperObjectHandleOwner()
{
    static NeverDestroyed<JniAPIWrapperObjectHandleOwner> handleOwner;
    return &handleOwner.get();
}

void JniAPIWrapperObjectHandleOwner::finalize(JSC::Handle<JSC::Unknown> handle, void*)
{
    auto* wrapperObject = static_cast<JSC::JSAPIWrapperObject*>(handle.get().asCell());
    if (wrapperObject->wrappedObject())
        JSCJniWrapperHolderMarkCollected(wrapperObject->wrappedObject());
    JSC::WeakSet::deallocate(JSC::WeakImpl::asWeakImpl(handle.slot()));
}

bool JniAPIWrapperObjectHandleOwner::isReachableFromOpaqueRoots(JSC::Handle<JSC::Unknown> handle, void*, JSC::AbstractSlotVisitor& visitor, ASCIILiteral*)
{
    // 对齐 ObjC（`JSAPIWrapperObject.mm:59-67`）：realm 被标记 **且** 其 native 对象是
    // opaque root（由 visitChildren 沿外部对象图传播登记）。realm 检查可避免"同一个 Java 对象
    // 在多个 global object 中"导致互相保活。
    auto* wrapperObject = uncheckedDowncast<JSC::JSAPIWrapperObject>(handle.get().asCell());
    void* wrappedObject = wrapperObject->wrappedObject();
    if (!wrappedObject)
        return false;
    uint32_t hash = JSCJniWrapperHolderIdentityHash(wrappedObject);
    if (!hash)
        return false;
    return visitor.isMarked(wrapperObject->realm())
        && visitor.containsOpaqueRoot(opaqueRootForHash(hash));
}

namespace JSC {

static JSC_DECLARE_HOST_FUNCTION(callJSAPIWrapperObjectCallbackObject);
static JSC_DECLARE_HOST_FUNCTION(constructJSAPIWrapperObjectCallbackObject);
static JSC_DECLARE_CUSTOM_GETTER(callbackGetterJSAPIWrapperObjectCallbackObject);
static JSC_DECLARE_CUSTOM_GETTER(staticFunctionGetterJSAPIWrapperObjectCallbackObject);

DEFINE_VISIT_CHILDREN_WITH_MODIFIER(template<>, JSCallbackObject<JSAPIWrapperObject>);

template <> const ClassInfo JSCallbackObject<JSAPIWrapperObject>::s_info = { "JSAPIWrapperObject"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(JSCallbackObject) };
template<> const DestructionMode JSCallbackObject<JSAPIWrapperObject>::needsDestruction = NeedsDestruction;

template <>
NativeFunction::Ptr JSCallbackObject<JSAPIWrapperObject>::getCallFunction()
{
    return callJSAPIWrapperObjectCallbackObject;
}

template <>
NativeFunction::Ptr JSCallbackObject<JSAPIWrapperObject>::getConstructFunction()
{
    return constructJSAPIWrapperObjectCallbackObject;
}

template <>
GetValueFunc JSCallbackObject<JSAPIWrapperObject>::getCallbackGetter()
{
    return callbackGetterJSAPIWrapperObjectCallbackObject;
}

template <>
GetValueFunc JSCallbackObject<JSAPIWrapperObject>::getStaticFunctionGetter()
{
    return staticFunctionGetterJSAPIWrapperObjectCallbackObject;
}

template <>
GCClient::IsoSubspace* JSCallbackObject<JSAPIWrapperObject>::subspaceForImpl(VM& vm, SubspaceAccess mode)
{
    switch (mode) {
    case SubspaceAccess::OnMainThread:
        return vm.jniAPIWrapperObjectSpace<SubspaceAccess::OnMainThread>();
    case SubspaceAccess::Concurrently:
        return vm.jniAPIWrapperObjectSpace<SubspaceAccess::Concurrently>();
    }
    RELEASE_ASSERT_NOT_REACHED();
    return nullptr;
}

template <>
Structure* JSCallbackObject<JSAPIWrapperObject>::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue proto)
{
    return Structure::create(vm, globalObject, proto, TypeInfo(ObjectType, StructureFlags), &s_info);
}

JSC_DEFINE_HOST_FUNCTION(callJSAPIWrapperObjectCallbackObject, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    return JSCallbackObject<JSAPIWrapperObject>::callImpl(globalObject, callFrame);
}

JSC_DEFINE_HOST_FUNCTION(constructJSAPIWrapperObjectCallbackObject, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    return JSCallbackObject<JSAPIWrapperObject>::constructImpl(globalObject, callFrame);
}

JSC_DEFINE_CUSTOM_GETTER(callbackGetterJSAPIWrapperObjectCallbackObject, (JSGlobalObject* globalObject, EncodedJSValue thisValue, PropertyName propertyName))
{
    return JSCallbackObject<JSAPIWrapperObject>::callbackGetterImpl(globalObject, thisValue, propertyName);
}

JSC_DEFINE_CUSTOM_GETTER(staticFunctionGetterJSAPIWrapperObjectCallbackObject, (JSGlobalObject* globalObject, EncodedJSValue thisValue, PropertyName propertyName))
{
    return JSCallbackObject<JSAPIWrapperObject>::staticFunctionGetterImpl(globalObject, thisValue, propertyName);
}

JSAPIWrapperObject::JSAPIWrapperObject(VM& vm, Structure* structure)
    : Base(vm, structure)
{
}

void JSAPIWrapperObject::finishCreation(VM& vm)
{
    Base::finishCreation(vm);
    WeakSet::allocate(this, jniAPIWrapperObjectHandleOwner(), 0); // Balanced in JniAPIWrapperObjectHandleOwner::finalize.
}

void JSAPIWrapperObject::setWrappedObject(void* wrappedObject)
{
    ASSERT(!m_wrappedObject);
    m_wrappedObject = wrappedObject;
}

template<typename Visitor>
void JSAPIWrapperObject::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    Base::visitChildren(cell, visitor);

    // 外部对象图传播（对齐 ObjC `JSAPIWrapperObject.mm:128-137` 的 scanExternalObjectGraph）：
    // 从本 wrapper 的 native 对象出发，沿 `addManagedReference(object, owner)` 登记的
    // owner → owned 关系做 BFS，把每个到达者的身份登记为 GC 的 opaque root
    // ⇒ 其 wrapper 随本 wrapper 一并存活。**不做任何 JNI**。
    //（分代 GC 下「老 owner → 新 object」这条边不会走到这里，由 remembered set 补扫 —— 见 JSCJni.cpp。）
    auto* thisObject = uncheckedDowncast<JSAPIWrapperObject>(cell);
    void* wrappedObject = thisObject->wrappedObject();
    if (!wrappedObject)
        return;

    uint32_t rootHash = JSCJniWrapperHolderIdentityHash(wrappedObject);
    if (!rootHash)
        return;
    JSCJniScanGraphFrom(visitor, rootHash);
}

DEFINE_VISIT_CHILDREN_WITH_MODIFIER(JS_EXPORT_PRIVATE, JSAPIWrapperObject);

} // namespace JSC

namespace {

// wrapper 的通用 JSClass（JSCallbackObjectData 会 JSClassRetain 它，必须非空；
// 全空回调 = 普通对象语义）。
JSClassRef jniWrapperObjectClass()
{
    static JSClassRef jsClass = nullptr;
    if (!jsClass) {
        JSClassDefinition definition = kJSClassDefinitionEmpty;
        definition.className = "JSCJniWrapperObject";
        jsClass = JSClassCreate(&definition);
    }
    return jsClass;
}

} // namespace

// 由 JSCJni.cpp 调用：创建 JSAPIWrapperObject cell（包装对象本体）。
//   · wrappedObject：JSCJni.cpp 管理的 holder 指针（"JS 侧已回收"由 owner.finalize 标记）
//   · jsClass：JSClass（导出实例传带 @JSProperty 访问器的 instanceClass；为空用通用类）
//   · privateData：JSObjectGetPrivate 的返回值（导出实例传 JniClassData*）
//
// **必须在 apiLock 下调用**：cell 分配（allocateCell → IsoSubspace）属 JSC 的分配路径，
// 未持锁时行为未定义 —— 实测表现为无任何日志的 SIGABRT（根因分析见文档 §6.16）。
extern "C" JSObjectRef JSCJniCreateAPIWrapperObjectWithClass(JSContextRef context, void* wrappedObject,
    JSClassRef jsClass, void* privateData)
{
    if (!context)
        return nullptr;
    JSC::JSGlobalObject* globalObject = toJS(context);
    if (!globalObject)
        return nullptr;

    JSC::JSLockHolder locker(globalObject->vm());

    auto* wrapper = JSC::JSCallbackObject<JSC::JSAPIWrapperObject>::create(
        globalObject, globalObject->jniWrapperObjectStructure(),
        jsClass ? jsClass : jniWrapperObjectClass(), privateData);
    if (!wrapper)
        return nullptr;
    wrapper->setWrappedObject(wrappedObject);
    return toRef(wrapper);
}

#endif // defined(JSC_JNI_API_ENABLED)
