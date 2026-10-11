# 阶段 4：Java 侧编译与打包（javac → jar → d8 → 含 classes.dex 的 jar）
#
# 产物：D:\WebKitJavaScriptCore\_deps\jni-java\jsc-jni-test.jar
# 运行（设备）：CLASSPATH=/data/local/tmp/jsc-jni-test.jar app_process -Djava.library.path=/data/local/tmp /system/bin org.webkit.jni.JSCJniTest
$ErrorActionPreference = 'Stop'

$Jdk      = 'D:\ReverseTools\jdk21'
$BuildTools = 'D:\AndroidDev\android-sdk\build-tools\34.0.0'
$JavaSrc  = 'd:\WebKitJavaScriptCore\WebKit-JavaScriptCore\Source\JavaScriptCore\API\jni\java'
$OutRoot  = 'D:\WebKitJavaScriptCore\_deps\jni-java'

$Classes  = Join-Path $OutRoot 'classes'
$ClassesJar = Join-Path $OutRoot 'classes.jar'
$D8Out    = Join-Path $OutRoot 'd8out'
$FinalJar = Join-Path $OutRoot 'jsc-jni-test.jar'

foreach ($dir in @($OutRoot, $Classes, $D8Out)) {
    if (Test-Path $dir) { Remove-Item $dir -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
}

# 1) javac：--release 8 生成 Android 兼容字节码
$sources = Get-ChildItem $JavaSrc -Recurse -Filter '*.java' | ForEach-Object { $_.FullName }
Write-Host "javac: $($sources.Count) 个源文件"
& "$Jdk\bin\javac.exe" --release 8 -Xlint:all -d $Classes @sources
if ($LASTEXITCODE -ne 0) { throw "javac 失败" }

# 2) 打成中间 jar（d8 直接吃 jar 比吃长文件列表可靠）
& "$Jdk\bin\jar.exe" cf $ClassesJar -C $Classes .
if ($LASTEXITCODE -ne 0) { throw "jar 失败" }

# 3) d8 → classes.dex（min-api 28 与本方案 minSdk 一致）
& "$BuildTools\d8.bat" --min-api 28 --output $D8Out $ClassesJar
if ($LASTEXITCODE -ne 0) { throw "d8 失败" }

# 4) 最终 jar（含 classes.dex，便于 app_process 直接使用）
& "$Jdk\bin\jar.exe" cf $FinalJar -C $D8Out classes.dex
if ($LASTEXITCODE -ne 0) { throw "打包失败" }

Write-Host ""
Write-Host "产物：$FinalJar  ($([math]::Round((Get-Item $FinalJar).Length/1KB,1)) KB)" -ForegroundColor Green
