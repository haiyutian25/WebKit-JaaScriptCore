# 带重试地拉取 Tools/Scripts（WebKit 构建期需要 webkit-build-directory / hmaptool 等）
Set-Location 'd:\WebKitJavaScriptCore\WebKit-JavaScriptCore'

for ($i = 1; $i -le 25; $i++) {
    git sparse-checkout add '/Tools/Scripts/' 2>&1 | Out-Null
    $n = (Get-ChildItem 'Tools\Scripts' -Recurse -File -ErrorAction SilentlyContinue | Measure-Object).Count
    $hasHmaptool = Test-Path 'Tools\Scripts\hmaptool'
    $hasWBD = Test-Path 'Tools\Scripts\webkit-build-directory'
    "attempt $i  files=$n  hmaptool=$hasHmaptool  wbd=$hasWBD  $(Get-Date -Format 'HH:mm:ss')"
    if ($hasHmaptool -and $hasWBD) {
        "DONE"
        break
    }
    Start-Sleep -Seconds 3
}
