$ErrorActionPreference = 'Stop'
$teyvatRoot = Split-Path -Parent $PSScriptRoot
$teyvatCache = Join-Path $teyvatRoot 'work\downloads'
$teyvatKit = Join-Path $teyvatRoot 'work\toolchain'
$teyvatLock = Get-Content -LiteralPath (Join-Path $teyvatRoot 'dependencies.lock.json') -Raw | ConvertFrom-Json
New-Item -ItemType Directory -Force -Path $teyvatCache,$teyvatKit | Out-Null

foreach ($package in $teyvatLock.packages) {
    $uri = [Uri]$package.url
    if ($uri.Scheme -ne 'https' -or $uri.Host -ne 'download.qt.io') { throw 'Unexpected dependency source.' }
    $archive = Join-Path $teyvatCache ($package.name + '.7z')
    if (-not (Test-Path -LiteralPath $archive)) {
        & curl.exe --fail --location --proto '=https' --proto-redir '=https' --retry 2 --show-error --output $archive $package.url
        if ($LASTEXITCODE -ne 0) { throw "Download failed: $($package.name)" }
    }
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $package.sha256) {
        throw "Checksum mismatch: $archive. Preserve it for diagnosis or remove this cached download before retrying."
    }
    $entries = @(& tar -tf $archive)
    if ($LASTEXITCODE -ne 0) { throw "Cannot inspect $archive" }
    foreach ($entry in $entries) {
        if ($entry -match '(^[/\\]|^[A-Za-z]:|(^|[/\\])\.\.([/\\]|$))') { throw "Unsafe archive entry: $entry" }
    }
    & tar -xf $archive -C $teyvatKit
    if ($LASTEXITCODE -ne 0) { throw "Cannot extract $archive" }
    Write-Output "Verified and extracted $($package.name)."
}
$teyvatWeb = $teyvatLock.webview2_sdk
$teyvatWebUri = [Uri]$teyvatWeb.url
if ($teyvatWebUri.Scheme -ne 'https' -or $teyvatWebUri.Host -ne 'api.nuget.org') { throw 'Unexpected WebView2 SDK source.' }
$teyvatWebArchive = Join-Path $teyvatCache $teyvatWeb.filename
if (-not (Test-Path -LiteralPath $teyvatWebArchive)) {
    & curl.exe --fail --location --proto '=https' --proto-redir '=https' --retry 2 --show-error --output $teyvatWebArchive $teyvatWeb.url
    if ($LASTEXITCODE -ne 0) { throw 'WebView2 SDK download failed.' }
}
if ((Get-FileHash -LiteralPath $teyvatWebArchive -Algorithm SHA256).Hash -ne $teyvatWeb.sha256) { throw 'WebView2 SDK checksum mismatch.' }
$teyvatWebEntries = @(& tar -tf $teyvatWebArchive)
if ($LASTEXITCODE -ne 0) { throw 'Cannot inspect WebView2 SDK.' }
foreach ($entry in $teyvatWebEntries) {
    if ($entry -match '(^[/\\]|^[A-Za-z]:|(^|[/\\])\.\.([/\\]|$))') { throw "Unsafe SDK archive entry: $entry" }
}
$teyvatWebDestination = Join-Path $teyvatKit 'webview2'
New-Item -ItemType Directory -Force -Path $teyvatWebDestination | Out-Null
& tar -xf $teyvatWebArchive -C $teyvatWebDestination
if ($LASTEXITCODE -ne 0) { throw 'Cannot extract WebView2 SDK.' }
Write-Output "Qt $($teyvatLock.qt_version) and the pinned native WebView2 SDK are ready. Run scripts/build.ps1."
