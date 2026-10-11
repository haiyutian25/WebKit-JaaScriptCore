<#
.SYNOPSIS
    Runs the L4 host-contract suites on the device and collects their output.

.DESCRIPTION
    Suites:
      L4ContractTest      - added for the test plan's L4 section (microtask timing, exception
                            propagation/isolation, reentrancy, lifetime across GC, deep nesting)
      L4Probe <case>      - minimal reproducer for the JS-object-to-Java crash investigation
      JSCJniTest          - the port's main bridge regression (value conversion, wrappers,
                            identity, GC, weak values, object graph, concurrency)
      Stage5Verification  - stage 5 matrix (JIT, maps, memory pressure, stability)

    Results are written to _deps\l4\out\<suite>.out and summarised on stdout.

    NOTE: ASCII-only (PowerShell 5.1 reads .ps1 as ANSI without a BOM).
#>
param(
    [string]$LibPath = "/data/local/tmp",      # java.library.path (use /data/local/tmp/dbg for a candidate build)
    [string]$Jar     = "d:\WebKitJavaScriptCore\_deps\jni-java\jsc-jni-test.jar",
    [switch]$SkipPush
)

$ErrorActionPreference = "Continue"
$adb    = "D:\AndroidDev\android-sdk\platform-tools\adb.exe"
$outDir = "d:\WebKitJavaScriptCore\_deps\l4\out"
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

if (-not $SkipPush) {
    $o = & $adb push $Jar "/data/local/tmp/jsc-jni-test.jar" 2>&1
    ($o | Select-String 'pushed|error') | Select-Object -Last 1 | ForEach-Object { Write-Host ("  " + $_.ToString().Trim()) }
}

function Run-Suite([string]$cls, [string]$extra, [string]$file, [int]$timeoutSec = 600) {
    Write-Host "=== $cls ===" -ForegroundColor Cyan
    $inner = "cd /data/local/tmp; CLASSPATH=/data/local/tmp/jsc-jni-test.jar timeout $timeoutSec " +
             "app_process -Djava.library.path=$LibPath /system/bin org.webkit.jni.$cls $extra > /data/local/tmp/$file 2>&1"
    & $adb shell $inner 2>&1 | Out-Null
    $o = & $adb pull "/data/local/tmp/$file" "$outDir\$file" 2>&1
    ($o | Select-String 'pulled|error') | Select-Object -Last 1 | ForEach-Object { Write-Host ("  " + $_.ToString().Trim()) }
    $lines = Get-Content "$outDir\$file" -Encoding UTF8 -ErrorAction SilentlyContinue |
                Where-Object { $_ -notmatch 'unused DT entry' -and $_.Trim() -ne '' }
    $lines | Select-Object -Last 12 | ForEach-Object { Write-Host ("  " + $_) }
    return $lines
}

$summary = New-Object System.Collections.Generic.List[string]

# L4 contract suite
$r = Run-Suite "L4ContractTest" "" "l4contract.out"
$pass = ($r | Select-String '^PASS').Count
$fail = ($r | Select-String '^FAIL').Count
$summary.Add(("L4ContractTest      pass={0} fail={1}" -f $pass, $fail))

# Minimal reproducer cases (each in its own process)
foreach ($c in 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H') {
    $lines = Run-Suite "L4Probe" $c "l4probe_$c.out" 120
    $ok = ($lines | Select-String "case=$c OK").Count -gt 0
    $summary.Add(("L4Probe $c           " + $(if ($ok) { "OK" } else { "FAILED/CRASH" })))
}

# Port regression suites
$r = Run-Suite "JSCJniTest" "" "jscjnitest.out" 900
$summary.Add(("JSCJniTest          " + (($r | Select-String 'passed=|PASS' | Select-Object -Last 1).ToString().Trim())))
$r = Run-Suite "Stage5Verification" "" "stage5.out" 900
$summary.Add(("Stage5Verification  " + (($r | Select-String 'passed=|PASS' | Select-Object -Last 1).ToString().Trim())))

Write-Host ""
Write-Host "=== L4 summary ===" -ForegroundColor Green
$summary | ForEach-Object { Write-Host ("  " + $_) }
$summary | Set-Content -Path "$outDir\l4_summary.txt" -Encoding UTF8
