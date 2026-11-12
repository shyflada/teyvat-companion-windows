# Build Teyvat Companion for Windows

Use Windows x64, PowerShell 7, CMake 3.24 or newer, and Ninja. Install the
[Microsoft Edge WebView2 Runtime](https://developer.microsoft.com/en-us/microsoft-edge/webview2/) to use HoYoLAB sign-in.
Node.js runs the optional HoYoLAB UI adapter test.

From the repository root:

~~~powershell
./scripts/setup-windows.ps1
./scripts/setup-opencv.ps1
./scripts/build.ps1
~~~

The setup scripts verify the Qt, MinGW, WebView2 SDK and OpenCV archives against
`dependencies.lock.json`. The build runs the CTest suite and produces
`work/build-app/TeyvatCompanion.exe`. The standalone core can be tested with
`./scripts/build.ps1 -CoreOnly` using a C++20 compiler on PATH.

To package the Windows preview with DLLs, translations, examples and license files:

~~~powershell
./scripts/package-preview.ps1
~~~

The archive is created under `outputs/release-candidate3/`. Choose a new
`-OutputName` for later packaging runs. Run the executable from the extracted
folder. A separate data directory can be selected with `--data-dir`.

The build tree and release outputs are ignored by Git. Use a fresh local
data directory for release smoke tests and screenshots.
