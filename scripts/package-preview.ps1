param(
    [ValidatePattern('^[A-Za-z0-9_-]+$')][string]$OutputName = 'release-candidate3',
    [ValidatePattern('^[A-Za-z0-9_-]+$')][string]$PackageName = 'Teyvat-Companion-v0_1_0-RC3',
    [ValidatePattern('^[A-Z0-9_]+\.md$')][string]$ReportFile = 'RC3_REPORT.md',
    [string]$ToolchainRoot
)
$ErrorActionPreference = 'Stop'
$teyvatRoot = Split-Path -Parent $PSScriptRoot
$teyvatKit = if ($ToolchainRoot) { (Resolve-Path -LiteralPath $ToolchainRoot).Path } else { Join-Path $teyvatRoot 'work\toolchain' }
$teyvatBuild = Join-Path $teyvatRoot 'work\build-app'
$teyvatOutput = Join-Path $teyvatRoot ('outputs\' + $OutputName)
$teyvatPreview = Join-Path $teyvatOutput $PackageName
$teyvatLock = Get-Content -LiteralPath (Join-Path $teyvatRoot 'dependencies.lock.json') -Raw | ConvertFrom-Json
if (Test-Path -LiteralPath $teyvatPreview) { throw 'Preview folder already exists. Use a fresh output directory instead of overwriting it.' }
if (-not (Test-Path -LiteralPath (Join-Path $teyvatBuild 'TeyvatCompanion.exe'))) { throw 'Run scripts/build.ps1 first.' }

$sourceArchive = Join-Path $teyvatRoot ('work\downloads\' + $teyvatLock.qtbase_source.filename)
if (-not (Test-Path -LiteralPath $sourceArchive)) {
    & curl.exe --fail --location --proto '=https' --proto-redir '=https' --retry 2 --show-error --output $sourceArchive $teyvatLock.qtbase_source.url
    if ($LASTEXITCODE -ne 0) { throw 'Qt corresponding-source download failed.' }
}
if ((Get-FileHash -LiteralPath $sourceArchive -Algorithm SHA256).Hash -ne $teyvatLock.qtbase_source.sha256) { throw 'Qt source hash mismatch.' }
$formatsArchive = Join-Path $teyvatRoot ('work\downloads\' + $teyvatLock.qtimageformats_source.filename)
if (-not (Test-Path -LiteralPath $formatsArchive)) {
    & curl.exe --fail --location --proto '=https' --proto-redir '=https' --retry 2 --show-error --output $formatsArchive $teyvatLock.qtimageformats_source.url
    if ($LASTEXITCODE -ne 0) { throw 'Qt Image Formats corresponding-source download failed.' }
}
if ((Get-FileHash -LiteralPath $formatsArchive -Algorithm SHA256).Hash -ne $teyvatLock.qtimageformats_source.sha256) { throw 'Qt Image Formats source hash mismatch.' }

New-Item -ItemType Directory -Force -Path $teyvatPreview | Out-Null
Copy-Item -LiteralPath (Join-Path $teyvatBuild 'TeyvatCompanion.exe') -Destination $teyvatPreview
Copy-Item -LiteralPath (Join-Path $teyvatBuild 'TeyvatEvaluate.exe') -Destination $teyvatPreview
Copy-Item -LiteralPath (Join-Path $teyvatKit 'webview2\build\native\x64\WebView2Loader.dll') -Destination $teyvatPreview
$teyvatOriginalPath = $env:PATH
try {
    $env:PATH = (Join-Path $teyvatKit 'Tools\mingw1310_64\bin') + ';' + (Join-Path $teyvatKit 'bin') + ';' + $env:PATH
    & (Join-Path $teyvatKit 'bin\windeployqt.exe') --release --no-translations --no-system-d3d-compiler --no-system-dxc-compiler --no-opengl-sw --skip-plugin-types 'sqldrivers,imageformats,iconengines,networkinformation,tls,generic,styles' (Join-Path $teyvatPreview 'TeyvatCompanion.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Qt deployment failed.' }
} finally { $env:PATH = $teyvatOriginalPath }
New-Item -ItemType Directory -Force -Path (Join-Path $teyvatPreview 'sqldrivers'),(Join-Path $teyvatPreview 'imageformats'),(Join-Path $teyvatPreview 'tls'),(Join-Path $teyvatPreview 'translations'),(Join-Path $teyvatPreview 'source'),(Join-Path $teyvatPreview 'licenses'),(Join-Path $teyvatPreview 'sbom') | Out-Null
Copy-Item -LiteralPath (Join-Path $teyvatKit 'plugins\sqldrivers\qsqlite.dll') -Destination (Join-Path $teyvatPreview 'sqldrivers')
Copy-Item -LiteralPath (Join-Path $teyvatKit 'plugins\imageformats\qwebp.dll') -Destination (Join-Path $teyvatPreview 'imageformats')
Copy-Item -LiteralPath (Join-Path $teyvatKit 'plugins\tls\qschannelbackend.dll') -Destination (Join-Path $teyvatPreview 'tls')
Copy-Item -LiteralPath (Join-Path $teyvatBuild 'translations\teyvat_uk.qm'),(Join-Path $teyvatBuild 'translations\teyvat_ru.qm') -Destination (Join-Path $teyvatPreview 'translations')
Set-Content -LiteralPath (Join-Path $teyvatPreview 'qt.conf') -Value "[Paths]`nPrefix=.`nPlugins=.`n" -Encoding ascii
foreach ($file in @('LICENSE','PRIVACY.md','SECURITY.md','THIRD_PARTY_NOTICES.md')) {
    Copy-Item -LiteralPath (Join-Path $teyvatRoot $file) -Destination $teyvatPreview
}
Copy-Item -LiteralPath (Join-Path $teyvatRoot 'docs\NAVIGATION_UK.md') -Destination (Join-Path $teyvatPreview 'README.md')
Copy-Item -LiteralPath (Join-Path $teyvatRoot 'docs\USAGE.md') -Destination $teyvatPreview
Copy-Item -LiteralPath (Join-Path $teyvatRoot 'docs\INTERCHANGE.md') -Destination $teyvatPreview
Copy-Item -LiteralPath (Join-Path $teyvatRoot 'docs\AUTOMATION.md'),(Join-Path $teyvatRoot 'docs\INTEGRATIONS.md') -Destination $teyvatPreview
Copy-Item -LiteralPath (Join-Path $teyvatRoot 'docs\LOCALIZATION.md') -Destination $teyvatPreview
Copy-Item -LiteralPath (Join-Path $teyvatRoot ('docs\' + $ReportFile)) -Destination $teyvatPreview
New-Item -ItemType Directory -Force -Path (Join-Path $teyvatPreview 'examples') | Out-Null
Copy-Item -LiteralPath (Join-Path $teyvatRoot 'docs\examples\local-grid.json') -Destination (Join-Path $teyvatPreview 'examples')
Copy-Item -LiteralPath (Join-Path $teyvatRoot 'docs\examples\automation') -Destination (Join-Path $teyvatPreview 'examples') -Recurse
Copy-Item -LiteralPath (Join-Path $teyvatRoot 'docs\examples\localization') -Destination (Join-Path $teyvatPreview 'examples') -Recurse
Copy-Item -LiteralPath $sourceArchive -Destination (Join-Path $teyvatPreview 'source')
Copy-Item -LiteralPath $formatsArchive -Destination (Join-Path $teyvatPreview 'source')

$sourceFiles = @('CMakeLists.txt','dependencies.lock.json','.gitignore','README.md','LICENSE','CHANGELOG.md','CONTRIBUTING.md','PRIVACY.md','SECURITY.md','THIRD_PARTY_NOTICES.md','include','src','tests','translations','scripts','docs') | ForEach-Object { Join-Path $teyvatRoot $_ }
$projectArchive = Join-Path $teyvatOutput ($PackageName + '-source.zip')
if (Test-Path -LiteralPath $projectArchive) { throw 'Source archive already exists.' }
Compress-Archive -LiteralPath $sourceFiles -DestinationPath $projectArchive -CompressionLevel Optimal
Copy-Item -LiteralPath $projectArchive -Destination (Join-Path $teyvatPreview 'source')

$noticeEntries = @(& tar -tf $sourceArchive | Where-Object { $_ -match '/LICENSES/[^/]+$|/src/3rdparty/.*/(LICEN[CS]E[^/]*|COPYING[^/]*|NOTICE[^/]*|COPYRIGHT[^/]*|qt_attribution\.json)$' })
& tar -xf $sourceArchive -C (Join-Path $teyvatPreview 'licenses') @noticeEntries
if ($LASTEXITCODE -ne 0) { throw 'Cannot extract Qt notices.' }
$formatsNotices = @(& tar -tf $formatsArchive | Where-Object { $_ -match '/LICENSES/[^/]+$|/src/3rdparty/.*/(LICEN[CS]E[^/]*|COPYING[^/]*|NOTICE[^/]*|COPYRIGHT[^/]*|qt_attribution\.json)$' })
& tar -xf $formatsArchive -C (Join-Path $teyvatPreview 'licenses') @formatsNotices
if ($LASTEXITCODE -ne 0) { throw 'Cannot extract Qt Image Formats notices.' }
New-Item -ItemType Directory -Force -Path (Join-Path $teyvatPreview 'licenses\mingw') | Out-Null
foreach ($component in @('gcc','mingw-w64','winpthreads')) {
    Copy-Item -LiteralPath (Join-Path $teyvatKit "Tools\mingw1310_64\licenses\$component") -Destination (Join-Path $teyvatPreview 'licenses\mingw') -Recurse
}
Copy-Item -LiteralPath (Join-Path $teyvatKit 'sbom\qtbase-6.11.3.cdx.json'),(Join-Path $teyvatKit 'sbom\qtbase-6.11.3.spdx.json') -Destination (Join-Path $teyvatPreview 'sbom')
Copy-Item -LiteralPath (Join-Path $teyvatKit 'sbom\qtimageformats-6.11.3.cdx.json'),(Join-Path $teyvatKit 'sbom\qtimageformats-6.11.3.spdx.json') -Destination (Join-Path $teyvatPreview 'sbom')
New-Item -ItemType Directory -Force -Path (Join-Path $teyvatPreview 'licenses\opencv') | Out-Null
$teyvatCvSource = Join-Path $teyvatRoot 'work\third-party\opencv-4.12.0'
Copy-Item -LiteralPath (Join-Path $teyvatCvSource 'LICENSE'),(Join-Path $teyvatCvSource 'COPYRIGHT') -Destination (Join-Path $teyvatPreview 'licenses\opencv')
Copy-Item -LiteralPath (Join-Path $teyvatRoot 'work\opencv\etc\licenses') -Destination (Join-Path $teyvatPreview 'licenses\opencv\upstream-third-party') -Recurse
foreach ($notice in @(@('modules/flann/include/opencv2/flann.hpp','OpenCV-BSD.txt'),@('modules/flann/include/opencv2/flann/general.h','FLANN-BSD.txt'))) {
    $header = Get-Content -LiteralPath (Join-Path $teyvatCvSource $notice[0]) -Raw
    $license = [regex]::Match($header,'(?s)/\*.*?\*/').Value
    if ([string]::IsNullOrEmpty($license)) { throw 'Cannot find OpenCV/FLANN attribution.' }
    Set-Content -LiteralPath (Join-Path $teyvatPreview ('licenses\opencv\'+$notice[1])) -Value $license -Encoding utf8
}
$teyvatLock.opencv_source | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $teyvatPreview 'sbom\opencv-build.json') -Encoding utf8
New-Item -ItemType Directory -Force -Path (Join-Path $teyvatPreview 'licenses\webview2') | Out-Null
Copy-Item -LiteralPath (Join-Path $teyvatKit 'webview2\LICENSE.txt'),(Join-Path $teyvatKit 'webview2\NOTICE.txt') -Destination (Join-Path $teyvatPreview 'licenses\webview2')
$teyvatLock.webview2_sdk | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $teyvatPreview 'sbom\webview2-sdk.json') -Encoding utf8

$runtimeHashes = Get-ChildItem -LiteralPath $teyvatPreview -Recurse -File | Where-Object { $_.Extension -in @('.exe','.dll','.qm') } | ForEach-Object {
    $hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    $relative = $_.FullName.Substring($teyvatPreview.Length + 1).Replace('\','/')
    '{0}  {1}' -f $hash,$relative
}
Set-Content -LiteralPath (Join-Path $teyvatPreview 'RUNTIME-SHA256SUMS.txt') -Value $runtimeHashes -Encoding utf8
Compress-Archive -LiteralPath $teyvatPreview -DestinationPath (Join-Path $teyvatOutput ($PackageName + '-Windows.zip')) -CompressionLevel Optimal
Write-Output ('Windows/source ZIPs created locally under outputs/' + $OutputName + '. No personal data or game captures were packaged.')
