<#
.SYNOPSIS
    L3 measurements for the Android JSC port: startup, RSS, forced-GC pause, streaming
    latency, and package/lib size breakdown.

.DESCRIPTION
    Device-side parts (startup loop, probe) run on the phone; size parts are computed on the
    host from the release APK and the shipped .so.  Output is written to _deps\l3\out\.

    NOTE: ASCII-only on purpose (PowerShell 5.1 reads .ps1 as ANSI without a BOM).
#>
param(
    [int]$StartupIterations = 20,
    [string]$Tag = "release"
)

$ErrorActionPreference = "Continue"
$root   = "d:\WebKitJavaScriptCore"
$adb    = "D:\AndroidDev\android-sdk\platform-tools\adb.exe"
$l3     = "$root\_deps\l3"
$outDir = "$l3\out"
$nm     = "D:\AndroidDev\android-sdk\ndk\28.2.13676358\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-size.exe"
$libSo  = "$root\MyApplication\app\src\main\jniLibs\arm64-v8a\libJavaScriptCore.so"
$apkRel = "$root\MyApplication\app\build\outputs\apk\release\app-release.apk"
$apkDbg = "$root\MyApplication\app\build\outputs\apk\debug\app-debug.apk"

New-Item -ItemType Directory -Path $outDir -Force | Out-Null
$report = New-Object System.Collections.Generic.List[string]
function Say($s) { Write-Host $s; $report.Add($s) }

function Adb([string[]]$AdbArgs) { $o = & $adb @AdbArgs 2>&1; return $o }

Say "=== L3 report ($Tag) ==="

# ---------- startup ----------
& $adb push "$l3\l3_probe.js" "$l3\l3_startup.sh" "$l3\l3_baseline.sh" "$l3\l3_rss.sh" `
    "$l3\l3_idle.js" "$l3\l3_peak.js" "$l3\l3_gc.js" "/data/local/tmp/" 2>&1 | Out-Null
$baseOut = Adb @("shell", "cd /data/local/tmp; time sh l3_baseline.sh $StartupIterations")
$jscOut  = Adb @("shell", "cd /data/local/tmp; time sh l3_startup.sh $StartupIterations")
function ParseReal($lines) {
    foreach ($l in $lines) {
        if ($l -match '([0-9]+)m([0-9.]+)s real') { return [double]$matches[1] * 60 + [double]$matches[2] }
    }
    return -1
}
$tBase = ParseReal $baseOut
$tJsc  = ParseReal $jscOut
if ($tJsc -gt 0 -and $tBase -ge 0) {
    $perStartup = ($tJsc - $tBase) / $StartupIterations
    Say ("startup: baseline {0:N2}s + jsc {1:N2}s over {2} runs -> per-startup {3:N1} ms (incl. fork/exec)" -f `
        $tBase, $tJsc, $StartupIterations, ($perStartup * 1000))
} else {
    Say "startup: measurement failed (device busy?)"
}

# ---------- probe: forced-GC pause / streaming ----------
$probe = Adb @("shell", "cd /data/local/tmp; export LD_LIBRARY_PATH=/data/local/tmp; ./jsc l3_probe.js")
$probeOk = $false
foreach ($l in $probe) {
    $line = $l.ToString()
    if ($line.StartsWith("L3 ")) { Say ("probe " + $line.Substring(3)); $probeOk = $true }
}
if (-not $probeOk) { Say "probe: no output (see raw below)"; $probe | ForEach-Object { Say ("  raw " + $_) } }

# ---------- RSS (host-side sampler; jsc cannot read /proc in-process) ----------
foreach ($case in @(@("l3_idle.js", "4", "rss_idle"),
                    @("l3_peak.js", "6", "rss_peak"),
                    @("l3_gc.js", "8", "rss_after_gc"))) {
    $r = Adb @("shell", "sh /data/local/tmp/l3_rss.sh $($case[0]) $($case[1]) $($case[2])")
    foreach ($l in $r) { if ($l.ToString() -match 'rss_kb=') { Say ("  " + $l.ToString().Trim()) } }
}

# ---------- size breakdown ----------
if (Test-Path $apkRel) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $z = [System.IO.Compression.ZipFile]::OpenRead($apkRel)
    $tot = ($z.Entries | Measure-Object -Property Length -Sum).Sum
    Say ("apk release: {0:N2} MB on disk, {1:N1} MB uncompressed" -f `
        ((Get-Item $apkRel).Length / 1MB), ($tot / 1MB))
    $z.Entries | Where-Object { $_.Length -gt 1MB } | Sort-Object Length -Descending |
        ForEach-Object { Say ("  apk entry {0,-40} {1,7:N1} MB" -f $_.FullName, ($_.Length / 1MB)) }
    $z.Dispose()
}
if (Test-Path $libSo) {
    Say ("libJavaScriptCore.so: {0:N2} MB" -f ((Get-Item $libSo).Length / 1MB))
    if (Test-Path $nm) {
        & $nm -A $libSo 2>&1 | Where-Object { $_ -match '^\s*\.\w' } | ForEach-Object {
            $p = ($_.ToString().Trim() -split '\s+')
            if ($p.Count -ge 2 -and $p[1] -match '^\d+$') {
                $b = [int64]$p[1]
                if ($b -gt 1MB) { Say ("  section {0,-14} {1,7:N1} MB" -f $p[0], ($b / 1MB)) }
            }
        }
    }
}

$report | Set-Content -Path "$outDir\l3_report.txt" -Encoding UTF8
Say "written: $outDir\l3_report.txt"
