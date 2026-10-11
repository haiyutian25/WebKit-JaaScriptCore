# =============================================================================
# Android 版 JavaScriptCore 移植 - 阶段 0 环境配置（前置补齐产物）
# 用法（PowerShell）:  . D:\WebKitJavaScriptCore\_deps\env.ps1
# 说明：本脚本只设置当前会话的环境变量，不修改系统配置。
# =============================================================================

$Deps      = 'D:\WebKitJavaScriptCore\_deps'
$VsCmake   = 'D:\VS2022BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin'
$VsNinja   = 'D:\VS2022BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'
$Ndk       = 'D:\AndroidDev\android-sdk\ndk\28.2.13676358'
$GitUsrBin = 'C:\Program Files\Git\usr\bin'          # 提供 perl 5.42（含 English/FindBin/JSON::PP）

# 工具链进入 PATH：ruby（本次自建）→ cmake/ninja（VS 自带）→ perl（Git 自带）
#
# 重要：WebKit 的生成器脚本会直接调用**裸命令** `perl` / `ruby`
#       （例如 Inspector codegen 的 preprocess.pl），因此 perl 与 ruby
#       必须真实存在于 PATH 中 —— 编译（cmake --build）前务必先 dot-source 本脚本。
$env:PATH = "$Deps\ruby\bin;$VsCmake;$VsNinja;$GitUsrBin;$env:PATH"

# 阶段 1 配置 WebKit 时会用到的显式变量
$env:JSC_ANDROID_CMAKE     = "$VsCmake\cmake.exe"
$env:JSC_ANDROID_NINJA     = "$VsNinja\ninja.exe"
$env:JSC_ANDROID_NDK       = $Ndk
$env:JSC_ANDROID_ICU_ROOT  = "$Deps\icu-install"
$env:JSC_ANDROID_RUBY      = "$Deps\ruby\bin\ruby.exe"
$env:JSC_ANDROID_PERL      = "$GitUsrBin\perl.exe"
$env:JSC_ANDROID_PYTHON    = 'D:\PythonDev\python.exe'
$env:JSC_ANDROID_ABI       = 'arm64-v8a'
$env:JSC_ANDROID_PLATFORM  = 'android-28'            # minSdk 28（见方案 §5.5）

# 提示：访问国内镜像（清华/中科大）时不要让 git/curl 走境外代理；
#       使用 git 时可用 -c http.proxy= -c https.proxy= 覆盖环境变量。
# $env:NO_PROXY = "localhost,127.0.0.1,::1,mirrors.ustc.edu.cn,mirrors.tuna.tsinghua.edu.cn"

Write-Host ''
Write-Host 'JSC Android (stage 0) environment ready:' -ForegroundColor Green
Write-Host "  cmake    : $env:JSC_ANDROID_CMAKE"
Write-Host "  ninja    : $env:JSC_ANDROID_NINJA"
Write-Host "  ndk      : $env:JSC_ANDROID_NDK"
Write-Host "  icu      : $env:JSC_ANDROID_ICU_ROOT"
Write-Host "  ruby     : $env:JSC_ANDROID_RUBY"
Write-Host "  perl     : $env:JSC_ANDROID_PERL"
Write-Host "  python   : $env:JSC_ANDROID_PYTHON"
Write-Host "  abi      : $env:JSC_ANDROID_ABI   platform: $env:JSC_ANDROID_PLATFORM"
Write-Host ''
