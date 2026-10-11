# 配置 Android 上的「上游 API 测试基线」构建（移植方案 §6.20.4）
#
# 与 build-android 的唯一差异（三个开关）：
#   -DDEVELOPER_MODE=ON       -> 声明上游 testapi 目标（shell/CMakeLists.txt:49-71）
#   -DENABLE_API_TESTS=ON     -> 拉入 ThirdParty/gtest|gmock，并允许 API 测试（需 OptionsJSCOnly.cmake 配合）
#   -DENABLE_STATIC_JSC=ON    -> JavaScriptCore/WTF/bmalloc 全为 STATIC
#                                （OptionsJSCOnly.cmake:65-69 在静态时不再覆盖，取 WebKitCommon.cmake:241-243 的 STATIC 默认）
#                                静态库携带全部内部符号 -> testapi 可与 JSC「同库」链接，与上游一致
#
# 其余参数与 build-android 逐项对齐（取自其 CMakeCache 的 :UNINITIALIZED= 项）。

$ErrorActionPreference = 'Stop'

$MASTER = 'D:\WebKitJavaScriptCore'
$SRC    = "$MASTER\WebKit-JavaScriptCore"
$BUILD  = "$SRC\build-android-testapi"
$CMAKE  = 'D:/VS2022BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
$NINJA  = 'D:/VS2022BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe'
$NDK    = 'D:/AndroidDev/android-sdk/ndk/28.2.13676358'
$ICU    = "$MASTER/_deps/icu-install"
# Perl：本机唯一可用的一份是 Git for Windows 自带的（原构建即由 PATH 发现，此处显式指定）
$PERL   = 'C:/Program Files/Git/usr/bin/perl.exe'
$PY     = 'D:/PythonDev/python.exe'
$RUBY   = "$MASTER/_deps/ruby/bin/ruby.exe"

$cargs = @(
    '-S', $SRC, '-B', $BUILD, '-G', 'Ninja',

    "-DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake",
    '-DCMAKE_BUILD_TYPE=Release',
    '-DPORT=JSCOnly', '-DEVENT_LOOP_TYPE=Generic',
    '-DANDROID_ABI=arm64-v8a', '-DANDROID_PLATFORM=android-28', '-DANDROID_STL=c++_shared',

    "-DCMAKE_FIND_ROOT_PATH=$ICU",
    '-DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=BOTH',
    '-DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=BOTH',
    '-DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER',

    "-DPERL_EXECUTABLE=$PERL",
    "-DPython_EXECUTABLE=$PY",
    "-DRUBY_EXECUTABLE=$RUBY",
    "-DICU_ROOT=$ICU",

    '-DENABLE_JIT=ON', '-DENABLE_DFG_JIT=ON', '-DENABLE_FTL_JIT=ON', '-DENABLE_C_LOOP=OFF',
    '-DENABLE_WEBASSEMBLY=ON', '-DENABLE_WEBASSEMBLY_BBQJIT=ON', '-DENABLE_WEBASSEMBLY_OMGJIT=ON',
    '-DENABLE_MPROTECT_RX_TO_RWX=ON',
    '-DENABLE_JSC_JNI_API=ON', '-DENABLE_JSC_GLIB_API=OFF',
    '-DENABLE_REMOTE_INSPECTOR=OFF', '-DENABLE_FUZZILLI=OFF',
    '-DENABLE_TOOLS=OFF',
    '-DUSE_LIBBACKTRACE=OFF', '-DUSE_SYSTEM_MALLOC=OFF',
    '-DCLANGD_AUTO_SETUP=OFF',
    "-DCMAKE_MAKE_PROGRAM=$NINJA",

    # —— 与 build-android 的三处差异 ——
    '-DDEVELOPER_MODE=ON',
    '-DENABLE_API_TESTS=ON',
    '-DENABLE_STATIC_JSC=ON',
    # DEVELOPER_MODE 默认会把警告当错误（WebKitCompilerFlags.cmake:166-168），而上游
    # WTF/StdLibExtras.h 的 [[clang::noescape]] 用法会被 NDK 的 clang 判为
    # -Wignored-attributes（'noescape' attribute only applies to pointer arguments）。
    # 该分支的条件是 NOT DEFINED CMAKE_COMPILE_WARNING_AS_ERROR —— 显式传入即跳过，
    # 无需改动上游源码。（上游自身也对 Windows 排除了这一项。）
    '-DCMAKE_COMPILE_WARNING_AS_ERROR=OFF',

    "-DCMAKE_INSTALL_PREFIX=$SRC/install-android-testapi"
)

& $CMAKE @cargs
exit $LASTEXITCODE
