# Third-party inventory — beta.6, 2026-11-10

Original project code is MIT licensed. Runtime Qt 6.11.3 Core, Gui, Widgets,
Sql and Network are dynamically linked and replaceable. Qt Concurrent uses
Core's thread pool; no standalone Concurrent DLL is imported by this build.
Plugins are qwindows, qsqlite, qwebp and the native Windows qschannelbackend.
No OpenSSL DLL, game binary, game image, official marker database or personal
capture is included in the Windows/source ZIPs.

Exact archives and hashes are pinned in dependencies.lock.json. The Windows
package includes full corresponding Qt Base and Qt Image Formats source,
application source, upstream licence/attribution texts and original Qt SBOMs.
Qt's source and SBOM packages also document optional components outside this
reduced runtime. Users may replace/relink compatible Qt libraries and debug
modified libraries; this project adds no restriction on doing so.

| Component | Source and terms | Package contents |
| --- | --- | --- |
| Qt 6.11.3 Base / Network / SQL / Widgets | [Qt licensing](https://doc.qt.io/qt-6/licensing.html), LGPLv3 | Dynamic DLLs, source, licences, upstream notices and SBOMs. Schannel uses Windows system TLS; certificate checks remain enabled. |
| Qt 6.11.3 Image Formats / WebP | [Qt Image Formats](https://doc.qt.io/qt-6/qtimageformats-index.html), LGPLv3 plus retained codec notices | qwebp.dll, corresponding source, licences/third-party notices and SBOMs. Only WebP is deployed from this add-on. |
| SQLite via QSQLITE | [SQLite copyright](https://www.sqlite.org/copyright.html), public-domain deliverable code | qsqlite.dll; actual package version and notices retained in the Qt Base inventory/source. |
| GCC/MinGW 13.1.0 runtime | Matching pinned official Qt toolchain | libgcc_s_seh-1, libstdc++-6, libwinpthread-1; GCC runtime exception and MinGW/winpthreads notices. |
| Microsoft WebView2 SDK 1.0.4314.35 | Official Microsoft NuGet package pinned by SHA-256; retained LICENSE.txt and NOTICE.txt | Native WebView2Loader.dll; dynamically uses the installed Microsoft Edge WebView2 Runtime. The runtime is not bundled. |
| OpenCV 4.12.0 | [Pinned upstream licence](https://github.com/opencv/opencv/blob/4.12.0/LICENSE), Apache-2.0 with retained BSD notices | Static core/imgproc/features2d/calib3d/flann; required zlib, FLANN and SoftFloat notices. Matching compiler, no nonfree/DNN/CUDA/IPP/video/camera/codecs. Source/hash and build selection are recorded. |
| Map/game data | [Official map](https://act.hoyolab.com/ys/app/interactive-map/index.html), HoYoverse content | Obtained only into the user's local cache. No redistribution or relicensing permission is asserted. Excluded from all distributable archives. |

Qt Test/Linguist, CMake and Ninja are build/test tools. No Windows font is
redistributed. Synthetic fixtures, diagrams and example coordinates are original
project materials. A separate local personal build may contain user-provided HUD
and opening crops and a private official map cache; it is not the public ZIP.
Legacy community data remains outside the new default runtime and archives.

RUNTIME-SHA256SUMS.txt inventories shipped executables, libraries and translation.
Licences and corresponding sources reside in licenses/, source/ and sbom/.
The project's MIT licence does not relicense dependencies, maps or trademarks.
This is a per-build inventory, not a claim of game-vendor approval, production
security certification or a public installer/signing review.
