param([string]$ToolchainRoot)

$ErrorActionPreference = 'Stop'
$teyvatRoot = Split-Path -Parent $PSScriptRoot
$teyvatLock = (Get-Content -LiteralPath (Join-Path $teyvatRoot 'dependencies.lock.json') -Raw | ConvertFrom-Json).opencv_source
New-Item -ItemType Directory -Force -Path (Join-Path $teyvatRoot 'work/downloads') | Out-Null
$teyvatArchive = Join-Path $teyvatRoot ('work/downloads/' + $teyvatLock.filename)
if (-not (Test-Path -LiteralPath $teyvatArchive)) {
    & curl.exe --fail --location --proto '=https' --proto-redir '=https' --connect-timeout 20 --max-time 240 --retry 1 --silent --show-error --output $teyvatArchive $teyvatLock.url
    if ($LASTEXITCODE -ne 0) { throw 'Official OpenCV source download failed.' }
}
$teyvatHash = (Get-FileHash -LiteralPath $teyvatArchive -Algorithm SHA256).Hash.ToLowerInvariant()
if ($teyvatHash -ne $teyvatLock.sha256) { throw 'OpenCV source SHA-256 mismatch. Source was not extracted or built.' }
Write-Output ('OpenCV source SHA-256: ' + $teyvatHash)
$teyvatSourceRoot = Join-Path $teyvatRoot 'work/third-party'
New-Item -ItemType Directory -Force -Path $teyvatSourceRoot | Out-Null
if (-not (Test-Path -LiteralPath (Join-Path $teyvatSourceRoot 'opencv-4.12.0/CMakeLists.txt'))) {
    $teyvatEntries = @(& tar.exe -tf $teyvatArchive)
    if ($LASTEXITCODE -ne 0 -or @($teyvatEntries | Where-Object { $_ -notlike 'opencv-4.12.0/*' -or $_ -match '(^|/)\.\.(/|$)|^[A-Za-z]:' }).Count -ne 0) { throw 'Unexpected OpenCV archive path.' }
    & tar.exe -xf $teyvatArchive -C $teyvatSourceRoot
    if ($LASTEXITCODE -ne 0) { throw 'OpenCV source extraction failed.' }
}
$teyvatKit = if ($ToolchainRoot) { (Resolve-Path -LiteralPath $ToolchainRoot).Path } else { Join-Path $teyvatRoot 'work/toolchain' }
$env:PATH = (Join-Path $teyvatKit 'Tools/mingw1310_64/bin') + ';' + (Join-Path $teyvatKit 'bin') + ';' + $env:PATH
$teyvatCompiler = Join-Path $teyvatKit 'Tools/mingw1310_64/bin/g++.exe'
$teyvatCCompiler = Join-Path $teyvatKit 'Tools/mingw1310_64/bin/gcc.exe'
$teyvatInstall = Join-Path $teyvatRoot 'work/opencv'
$teyvatBuild = Join-Path $teyvatRoot 'work/build-opencv'
$teyvatProfile = [Environment]::GetFolderPath('UserProfile')
$teyvatSourcePath = $teyvatRoot.Replace('\', '/')
$teyvatProfilePath = $teyvatProfile.Replace('\', '/')
$teyvatToolchainPath = $teyvatKit.Replace('\', '/')
$teyvatPathFlags = '-ffile-prefix-map="{0}"=. -ffile-prefix-map="{1}"=.' -f $teyvatProfilePath,$teyvatSourcePath
& cmake -S (Join-Path $teyvatSourceRoot 'opencv-4.12.0') -B $teyvatBuild -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_CXX_COMPILER=$teyvatCompiler" "-DCMAKE_C_COMPILER=$teyvatCCompiler" "-DCMAKE_CXX_FLAGS=$teyvatPathFlags" "-DCMAKE_C_FLAGS=$teyvatPathFlags" "-DCMAKE_INSTALL_PREFIX=$teyvatInstall" '-DBUILD_LIST=core,imgproc,features2d,calib3d' -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTS=OFF -DBUILD_PERF_TESTS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_opencv_apps=OFF -DBUILD_JAVA=OFF -DBUILD_opencv_python2=OFF -DBUILD_opencv_python3=OFF -DBUILD_PROTOBUF=OFF -DWITH_FFMPEG=OFF -DWITH_GSTREAMER=OFF -DWITH_IPP=OFF -DWITH_TBB=OFF -DWITH_OPENCL=OFF -DWITH_ITT=OFF -DWITH_LAPACK=OFF -DWITH_EIGEN=OFF -DWITH_CUDA=OFF -DWITH_OPENEXR=OFF -DWITH_OPENJPEG=OFF -DWITH_JPEG=OFF -DWITH_PNG=OFF -DWITH_TIFF=OFF -DWITH_WEBP=OFF -DWITH_JASPER=OFF -DWITH_AVIF=OFF -DWITH_GDCM=OFF -DBUILD_ZLIB=ON -DOPENCV_ENABLE_NONFREE=OFF -DCPU_BASELINE=SSE2 '-DCPU_DISPATCH=' *> (Join-Path $teyvatRoot 'work/opencv-configure.log')
if ($LASTEXITCODE -ne 0) { Get-Content (Join-Path $teyvatRoot 'work/opencv-configure.log') -Tail 40; throw 'Minimal OpenCV configure failed.' }
foreach ($generated in @('version_string.tmp','modules/core/version_string.inc','opencv_data_config.hpp')) {
    $path = Join-Path $teyvatBuild $generated
    if (!(Test-Path -LiteralPath $path)) { continue }
    $contents = Get-Content -LiteralPath $path -Raw
    foreach ($prefix in @($teyvatSourcePath,$teyvatToolchainPath,$teyvatProfilePath,$teyvatRoot.Replace('\','\\'),
            $teyvatKit.Replace('\','\\'),$teyvatProfile.Replace('\','\\'))) {
        $contents = $contents.Replace($prefix, '.', [StringComparison]::OrdinalIgnoreCase)
    }
    Set-Content -LiteralPath $path -Value $contents -NoNewline -Encoding utf8
}
Write-Output 'Minimal static OpenCV configured with matching Qt GCC 13.1.'
& cmake --build (Join-Path $teyvatRoot 'work/build-opencv') --parallel 4 *> (Join-Path $teyvatRoot 'work/opencv-build.log')
if ($LASTEXITCODE -ne 0) { Get-Content (Join-Path $teyvatRoot 'work/opencv-build.log') -Tail 35; throw 'Minimal OpenCV build failed.' }
& cmake --install (Join-Path $teyvatRoot 'work/build-opencv') *> (Join-Path $teyvatRoot 'work/opencv-install.log')
if ($LASTEXITCODE -ne 0) { throw 'OpenCV installation failed.' }
Write-Output 'Minimal static OpenCV installed locally under work/opencv.'

