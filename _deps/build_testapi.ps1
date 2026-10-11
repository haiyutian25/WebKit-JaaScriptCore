# 后台构建上游 testapi（方案 §6.20.4）
#
# 为什么必须改 PATH：JavaScriptCore 的生成器脚本（如
# inspector/scripts/generate-inspector-protocol-bindings.py）以**裸名**调用外部程序：
#     subprocess.check_call(["perl", "codegen/preprocess.pl", ...])
# 因此 PATH 里必须有 perl，否则派生源生成阶段就失败（WinError 2）。
# 本机 perl 只有 Git for Windows 自带的一份；python / ruby 同理放进 PATH 以便脚本按裸名调用。
#
# 用法：powershell -ExecutionPolicy Bypass -File build_testapi.ps1 [-Jobs 8] [-Foreground]

param(
    [int]    $Jobs = 0,
    [switch] $Foreground
)

$ErrorActionPreference = 'Stop'

$MASTER = 'D:\WebKitJavaScriptCore'
$SRC    = "$MASTER\WebKit-JavaScriptCore"
$BUILD  = "$SRC\build-android-testapi"
$NINJA  = 'D:/VS2022BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe'

$env:PATH = "C:\Program Files\Git\usr\bin;$MASTER\_deps\ruby\bin;D:\PythonDev;$env:PATH"

$log    = "$BUILD\build.log"
$errlog = "$BUILD\build.err.log"

$args = @('-C', $BUILD)
if ($Jobs -gt 0) { $args += @('-j', "$Jobs") }
$args += 'testapi'

if ($Foreground) {
    & $NINJA @args
    exit $LASTEXITCODE
}

Remove-Item $log, $errlog -Force -ErrorAction SilentlyContinue
$p = Start-Process -FilePath $NINJA -ArgumentList $args -NoNewWindow `
     -RedirectStandardOutput $log -RedirectStandardError $errlog -PassThru
"ninja 已启动（后台），PID = $($p.Id)，日志: $log"
