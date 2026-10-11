<#
.SYNOPSIS
    JSC Android port - regression scorecard (one command).

.DESCRIPTION
    Aggregates the results produced by the four layers into the graded scorecard described in
    "Android JSC 移植版完整测试方案.md":

      L1  semantics      _deps\t262\work\<tag>\results.json + summary.txt   (threshold >= 97%)
      L2  JIT consistency_deps\t262\work\<tag2>\results.json vs the release tag's results
      L3  performance    _deps\l3\out\l3_report.txt
      L4  host contract  _deps\l4\out\l4_summary.txt

    Layers that have not been run yet are reported as NOT RUN (the script never fails hard).

    NOTE: ASCII-only (PowerShell 5.1 reads .ps1 as ANSI without a BOM).
#>
param(
    [string]$L1Tag = "release",
    [string[]]$L2Tags = @("jit-off", "jit-baseline", "jit-dfg", "jit-full"),
    [string]$OutFile = "d:\WebKitJavaScriptCore\_deps\scorecard.txt"
)

$ErrorActionPreference = "Continue"
$root = "d:\WebKitJavaScriptCore"
$py   = "D:\PythonDev\python.exe"
$lines = New-Object System.Collections.Generic.List[string]
function Say($s) { Write-Host $s; $lines.Add($s) }

Say ("JSC Android port - regression scorecard ({0:yyyy-MM-dd HH:mm})" -f (Get-Date))
Say "======================================================"

# ---------------- L1 ----------------
$l1 = "$root\_deps\t262\work\$L1Tag"
$l1Json = "$l1\results.json"
Say ""
Say "[L1] semantics - test262 conformance"
if (Test-Path $l1Json) {
    $data = Get-Content $l1Json -Raw -Encoding UTF8 | ConvertFrom-Json
    $pass = ($data | Where-Object { $_.status -eq 'pass' }).Count
    $fail = ($data | Where-Object { $_.status -eq 'fail' }).Count
    $denom = $pass + $fail
    $rate = if ($denom -gt 0) { 100.0 * $pass / $denom } else { 0 }
    $verdict = if ($rate -ge 97) { "PASS" } else { "FAIL" }
    Say ("  pass {0}/{1} = {2:N2}%  (threshold >= 97%)  {3}" -f $pass, $denom, $rate, $verdict)
    $byDir = @{}
    foreach ($r in ($data | Where-Object { $_.status -eq 'fail' })) {
        $p = $r.test -split '/'
        $k = ($p[0..1] -join '/')
        if (-not $byDir.ContainsKey($k)) { $byDir[$k] = 0 }
        $byDir[$k]++
    }
    $byDir.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 6 |
        ForEach-Object { Say ("    failures {0,-18} {1}" -f $_.Key, $_.Value) }
    $gen = "$l1\gen.json"
    if (Test-Path $gen) {
        $g = Get-Content $gen -Raw -Encoding UTF8 | ConvertFrom-Json
        Say ("    test262 revision {0} | files {1} | commands {2} | skipped {3}" -f `
            $g.test262_revision.Substring(0, [Math]::Min(12, $g.test262_revision.Length)), $g.files, $g.commands, $g.skipped)
    }
} else {
    Say "  NOT RUN (no $l1Json)"
}

# ---------------- L2 ----------------
Say ""
Say "[L2] JIT configuration consistency (sampled test262 across tiers)"
$baseJson = $l1Json
$found = @()
foreach ($t in $L2Tags) {
    $p = "$root\_deps\t262\work\$t\results.json"
    if (Test-Path $p) { $found += $p }
}
if ($found.Count -gt 0 -and (Test-Path $baseJson)) {
    $cmp = & $py "$root\_deps\t262\t262_compare.py" $baseJson @found 2>&1
    $cmp | ForEach-Object { Say ("  " + $_) }
} else {
    Say "  NOT RUN (needs sample runs: run_test262.ps1 -Tag jit-* -Sample 20 -EnvPrefix ...)"
}

# ---------------- L3 ----------------
Say ""
Say "[L3] performance and footprint"
$l3 = "$root\_deps\l3\out\l3_report.txt"
if (Test-Path $l3) {
    Get-Content $l3 -Encoding UTF8 | Where-Object { $_.Trim() -ne '' -and $_ -notmatch '^=== ' } |
        ForEach-Object { Say ("  " + $_) }
} else {
    Say "  NOT RUN (run _deps\l3\run_l3.ps1)"
}

# ---------------- L4 ----------------
Say ""
Say "[L4] host embedding contract"
$l4 = "$root\_deps\l4\out\l4_summary.txt"
if (Test-Path $l4) {
    Get-Content $l4 -Encoding UTF8 | Where-Object { $_.Trim() -ne '' } | ForEach-Object { Say ("  " + $_) }
} else {
    Say "  NOT RUN (run _deps\l4\run_l4.ps1)"
}

Say ""
$lines | Set-Content -Path $OutFile -Encoding UTF8
Write-Host ""
Write-Host "scorecard written: $OutFile" -ForegroundColor Green
