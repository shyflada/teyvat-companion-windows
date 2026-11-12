param([switch]$CoreOnly, [string]$ToolchainRoot)
$ErrorActionPreference = 'Stop'
$teyvatRoot = Split-Path -Parent $PSScriptRoot
$teyvatOriginalPath = $env:PATH
try {
    if ($CoreOnly) {
        $teyvatBuild = Join-Path $teyvatRoot 'work\build-core'
        & cmake -S $teyvatRoot -B $teyvatBuild -G Ninja -DCMAKE_BUILD_TYPE=Release -DTEYVAT_BUILD_APP=OFF
    } else {
        $teyvatKit = if ($ToolchainRoot) { (Resolve-Path -LiteralPath $ToolchainRoot).Path } else { Join-Path $teyvatRoot 'work\toolchain' }
        $teyvatOpenCv = Join-Path $teyvatRoot 'work\opencv'
        $compiler = Join-Path $teyvatKit 'Tools\mingw1310_64\bin\g++.exe'
        if (-not (Test-Path -LiteralPath $compiler)) { throw 'Run scripts/setup-windows.ps1 first.' }
        if (-not (Test-Path -LiteralPath (Join-Path $teyvatOpenCv 'OpenCVConfig.cmake'))) { throw 'Run scripts/setup-opencv.ps1 first.' }
        $env:PATH = (Split-Path -Parent $compiler) + ';' + (Join-Path $teyvatKit 'bin') + ';' + $env:PATH
        $teyvatBuild = Join-Path $teyvatRoot 'work\build-app'
        & cmake -S $teyvatRoot -B $teyvatBuild -G Ninja -DCMAKE_BUILD_TYPE=Release -DTEYVAT_BUILD_APP=ON "-DCMAKE_CXX_COMPILER=$compiler" "-DCMAKE_PREFIX_PATH=$teyvatKit;$teyvatOpenCv" "-DTEYVAT_WEBVIEW2_SDK=$teyvatKit/webview2"
    }
    if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }
    & cmake --build $teyvatBuild --parallel 4
    if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
    & ctest --test-dir $teyvatBuild --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
} finally {
    $env:PATH = $teyvatOriginalPath
}
