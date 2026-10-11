<#
.SYNOPSIS
    L2 - JIT configuration matrix: run a sampled test262 sweep for each JIT tier and report
    whether the pass/fail set matches the release baseline.

.DESCRIPTION
    Tiers (EnvPrefix is spliced into the device command line, so multiple assignments work):
      jit-off        JSC_useJIT=0                                  (LLInt only)
      jit-baseline   JSC_useJIT=1 JSC_useDFGJIT=0                  (LLInt + Baseline)
      jit-dfg        JSC_useJIT=1 JSC_useDFGJIT=1 JSC_useFTLJIT=0  (LLInt + Baseline + DFG)
    The 4th tier (full FTL) is the L1 full run itself (tag: release).

    Sampling: -Sample 20 (every 20th test file) keeps each tier ~5k commands (~10-15 min).

    NOTE: ASCII-only (PowerShell 5.1 reads .ps1 as ANSI without a BOM).
#>
param(
    [int]$Sample = 20,
    [int]$Chunks = 6,
    [string[]]$Tags = @("jit-off", "jit-baseline", "jit-dfg")
)

$ErrorActionPreference = "Continue"
$tools = "d:\WebKitJavaScriptCore\_deps\t262"

$map = @{
    "jit-off"      = "JSC_useJIT=0"
    "jit-baseline" = "JSC_useJIT=1 JSC_useDFGJIT=0"
    "jit-dfg"      = "JSC_useJIT=1 JSC_useDFGJIT=1 JSC_useFTLJIT=0"
}

foreach ($tag in $Tags) {
    $env = $map[$tag]
    Write-Host ""
    Write-Host "================ L2 tier: $tag  (env: $env) ================" -ForegroundColor Yellow
    & powershell -ExecutionPolicy Bypass -File "$tools\run_test262.ps1" `
        -Tag $tag -EnvPrefix $env -Sample $Sample -Chunks $Chunks -ReuseTree
    Write-Host "---- tier $tag done ----" -ForegroundColor Yellow
}

Write-Host ""
Write-Host "================ L2 comparison vs release baseline ================" -ForegroundColor Green
& D:\PythonDev\python.exe "$tools\t262_compare.py" `
    "d:\WebKitJavaScriptCore\_deps\t262\work\release\results.json" `
    "d:\WebKitJavaScriptCore\_deps\t262\work\jit-off\results.json" `
    "d:\WebKitJavaScriptCore\_deps\t262\work\jit-baseline\results.json" `
    "d:\WebKitJavaScriptCore\_deps\t262\work\jit-dfg\results.json"
