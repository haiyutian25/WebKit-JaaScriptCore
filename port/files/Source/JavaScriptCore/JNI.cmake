# JNI 桥接层构建登记（移植方案 §6.4）
#
# 与 GLib.cmake 同构：把 API/jni/*.cpp 加入 JavaScriptCore_SOURCES，并把 API/jni 加入
# 私有 include 目录（JSCJni.cpp 需要 APICast.h / JSContextRefPrivate.h 等内部头）。
#
# 注意（方案 §6.4）：
#   · 不要把 API/jni/* 加进 Sources.txt —— 那是全平台编译清单。
#   · JSC_JNI_API_ENABLED 不得与 JSC_OBJC_API_ENABLED / JSC_GLIB_API_ENABLED 同时定义
#     （heap/Heap.h 的 apiWrapperObjectSpace 会重复定义，两个 JSAPIWrapperObject* TU 会重复符号）。
#     当前 JNI.cmake 只在独立开关 ENABLE_JSC_JNI_API 下被 include，且 OptionsJSCOnly.cmake
#     保证三者互斥。

list(APPEND JavaScriptCore_SOURCES
    API/jni/JSCJni.cpp
    API/jni/JSAPIWrapperObjectJni.cpp
)

list(APPEND JavaScriptCore_PRIVATE_INCLUDE_DIRECTORIES
    "${JAVASCRIPTCORE_DIR}/API/jni"
)

# 与 GLib 的 add_definitions(-DJSC_GLIB_API_ENABLED) 同构（OptionsJSCOnly.cmake:104）。
# 由于本文件由 Source/JavaScriptCore/PlatformJSCOnly.cmake 在 JavaScriptCore 目录内 include，
# 作用域恰好覆盖 JSCJni.cpp 与需要该守卫的内部头（API/JSAPIWrapperObject.h、heap/Heap.h）。
add_definitions(-DJSC_JNI_API_ENABLED)

# Java 侧源码随构建产出（便于 javac/d8 打包），不做编译，仅登记位置。
set(JavaScriptCore_JNI_JAVA_DIR "${JAVASCRIPTCORE_DIR}/API/jni/java" CACHE INTERNAL "JNI Java sources")
