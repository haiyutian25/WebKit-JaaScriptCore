<#
.SYNOPSIS
    Run test262 on a real Android device (one work dir per tag, for multi-config comparison).

.DESCRIPTION
    host: generate command list -> pack (test/ + harness/ + includes/ + commands) -> push
    device: run everything locally in parallel (avoids one adb round-trip per test)
    host: pull results -> analyze.

    Semantics mirror WebKit's Tools/Scripts/test262/runner.py
    (jsc --strict-file/--module-file/--exception/--test262-async + harness/sta.js+assert.js;
     pass = exit code 0 and no output).

    NOTE: this file is intentionally ASCII-only: Windows PowerShell 5.1 reads .ps1 as ANSI
    (GBK) unless a BOM is present, which corrupts non-ASCII string literals and breaks parsing.
    Also: avoid PS $(...) subexpressions and & inside strings passed to adb; the device-side
    logic lives in t262_launch.sh / t262_progress.sh instead.
#>
param(
    [string]$Tag = "release",            # run label (work dir name / results dir)
    [string]$EnvPrefix = "",             # e.g. "JSC_useJIT=0"
    [string]$Only = "",                  # only tests whose rel path matches this regex
    [int]$Limit = 0,                     # max number of test files to generate (0 = all)
    [int]$Sample = 0,                    # take every Nth test file (0 = no sampling)
    [int]$Chunks = 4,                    # device-side parallel workers
    [int]$PerTestTimeout = 30,           # per-test timeout in seconds
    [switch]$ReuseTree,                  # reuse already-extracted tree on device, push commands only
    [switch]$AnalyzeOnly                 # only pull results and analyze
)

# adb 会把传输进度/摘要写到 stderr；PS 5.1 会把它包装成 NativeCommandError。
# 用 Continue + 显式 2>&1 处理，避免把正常的 adb 输出当成致命错误。
$ErrorActionPreference = "Continue"
$root     = "d:\WebKitJavaScriptCore"
$adb      = "D:\AndroidDev\android-sdk\platform-tools\adb.exe"
$python   = "D:\PythonDev\python.exe"
$t262     = "$root\_deps\test262"
$config   = "$root\WebKit-JavaScriptCore\JSTests\test262\config.yaml"
$tools    = "$root\_deps\t262"
$workRoot = "$tools\work"
$work     = "$workRoot\$Tag"
$stage    = "$workRoot\stage"
$device   = "/data/local/tmp/t262"

# 注意：不能用 `| Select-Object -First 1` 截断 adb push/pull 的输出流 —— PowerShell 会据此
# 提前终止管道并杀掉 adb，导致大文件传输被中断（表现为“push 报成功但文件不存在”）。
function Invoke-Adb {
    param([string[]]$AdbArgs, [int]$KeepLines = 0)
    $out = & $adb @AdbArgs 2>&1
    if ($KeepLines -gt 0) {
        $sel = $out | Select-String 'pushed|pulled|skipped|error|failed|No such' | Select-Object -Last $KeepLines
        foreach ($l in $sel) { Write-Host ("  " + $l.ToString().Trim()) }
    }
    return $LASTEXITCODE
}

if (-not $AnalyzeOnly) {
    # (1) generate command list
    $genArgs = @("$tools\t262_gen.py", "--test262", $t262, "--config", $config, "--out", $work)
    if ($EnvPrefix)   { $genArgs += @("--envprefix", $EnvPrefix) }
    if ($Only)        { $genArgs += @("--only", $Only) }
    if ($Limit -gt 0) { $genArgs += @("--limit", "$Limit") }
    if ($Sample -gt 1) { $genArgs += @("--sample", "$Sample") }
    Write-Host "=== (1) generate command list ===" -ForegroundColor Cyan
    & $python @genArgs
    if ($LASTEXITCODE -ne 0) { throw "generation failed" }

    if ($ReuseTree) {
        Write-Host "=== (2) push commands only (reuse device tree) ===" -ForegroundColor Cyan
        Invoke-Adb @("push", "$work\commands.txt", "$device/commands.txt") 1 | Out-Null
        & $adb shell "rm -rf $device/includes; mkdir -p $device/includes" 2>&1 | Out-Null
        Invoke-Adb @("push", "$work\includes\.", "$device/includes/") 1 | Out-Null
        Invoke-Adb @("push", "$tools\t262_run.sh", "$tools\t262_launch.sh", "$tools\t262_progress.sh", "$device/") 1 | Out-Null
    }
    else {
        # (2) pack (test/ + harness/ prepared once and cached)
        Write-Host "=== (2) pack ===" -ForegroundColor Cyan
        New-Item -ItemType Directory -Path $stage -Force | Out-Null
        if (-not (Test-Path "$stage\test")) {
            Write-Host "  first-time tree staging (~54k files, done once)..."
            & robocopy "$t262\test" "$stage\test" /E /NFL /NDL /NJH /NJS /NP | Out-Null
            & robocopy "$t262\harness" "$stage\harness" /E /NFL /NDL /NJH /NJS /NP | Out-Null
        }
        Copy-Item "$work\commands.txt","$work\map.tsv","$work\skipped.tsv" $stage -Force
        if (Test-Path "$stage\includes") { Remove-Item "$stage\includes" -Recurse -Force }
        Copy-Item "$work\includes" "$stage\includes" -Recurse
        Copy-Item "$tools\t262_run.sh","$tools\t262_launch.sh","$tools\t262_progress.sh" $stage -Force
        $tar = "$workRoot\payload.tar.gz"
        Remove-Item $tar -Force -ErrorAction SilentlyContinue
        & tar -czf $tar -C $stage .
        Write-Host ("  payload: {0} MB" -f [math]::Round((Get-Item $tar).Length / 1MB, 1))

        # (3) push and extract
        Write-Host "=== (3) push and extract ===" -ForegroundColor Cyan
        & $adb shell "rm -rf $device; mkdir -p $device" 2>&1 | Out-Null
        $sw = [Diagnostics.Stopwatch]::StartNew()
        Invoke-Adb @("push", $tar, "$device/payload.tar.gz") 1 | Out-Null
        & $adb shell "cd $device; tar xzf payload.tar.gz; rm -f payload.tar.gz; cp /data/local/tmp/jsc /data/local/tmp/libJavaScriptCore.so /data/local/tmp/libc++_shared.so $device/ 2>/dev/null; chmod 755 $device/jsc $device/t262_run.sh $device/t262_launch.sh $device/t262_progress.sh; echo EXTRACT_OK; ls $device | head -12" 2>&1 | ForEach-Object { "  " + $_ }
        $sw.Stop()
        Write-Host ("  push+extract {0:N0} s" -f $sw.Elapsed.TotalSeconds)
    }
}

# (4) launch workers
if (-not $AnalyzeOnly) {
    Write-Host "=== (4) device-side parallel run ($Chunks workers) ===" -ForegroundColor Cyan
    & $adb shell "sh $device/t262_launch.sh $Chunks $PerTestTimeout" 2>&1 | ForEach-Object { "  " + $_ }
}
$total = (& $adb shell "wc -l < $device/commands.txt" 2>&1 | Select-Object -First 1).Trim()
Write-Host "  total commands: $total"

# (5) poll progress
$sw = [Diagnostics.Stopwatch]::StartNew()
while ($true) {
    Start-Sleep -Seconds 30
    $prog = & $adb shell "sh $device/t262_progress.sh" 2>&1
    $done = ($prog | Select-Object -First 1).Trim()
    $fin  = ($prog | Select-Object -Skip 1 -First 1).Trim()
    $pct  = 0
    if ([int]$total -gt 0) { $pct = [math]::Round(100.0 * [int]$done / [int]$total, 1) }
    Write-Host ("  [{0,6:N0}s] done {1}/{2} ({3}%), workers finished {4}/$Chunks" -f $sw.Elapsed.TotalSeconds, $done, $total, $pct, $fin)
    if ([int]$fin -ge $Chunks) { break }
    if ($sw.Elapsed.TotalMinutes -gt 240) { Write-Warning "polling exceeded 240 min, stop"; break }
}

# (6) pull results and analyze
Write-Host "=== (6) pull results and analyze ===" -ForegroundColor Cyan
$resDir = "$work\results"
if (Test-Path $resDir) { Remove-Item $resDir -Recurse -Force }
New-Item -ItemType Directory -Path $resDir | Out-Null
for ($i = 0; $i -lt $Chunks; $i++) {
    Invoke-Adb @("pull", "$device/results_$i.out", "$resDir/results_$i.out") 1 | Out-Null
}
& $adb shell "cd $device; rm -f results_*.out done.* state.* run_*.log" 2>&1 | Out-Null
& $python "$tools\t262_analyze.py" --work $work --results $resDir --out $work
