# Development preview: use and local data

**Current default: 0.1.0-beta.3, official map navigation beta.** Start with
[the current short instructions](NAVIGATION_UK.md). The tools below are optional,
opened through **Advanced tools**. A live synthetic example connects position,
target lock, compass, detection, progress and advancement. Real-game navigation
uses public official map resources, global acquisition through M, a moving local
reference and a camera-relative compass/course badge. Standard HUD calibration
is learned in memory; the optional personal 2560x1440 pack is excluded from ZIPs.
Opening generalization, floor overlays and official-account synchronization
still need work. Prepare game data is a
separate explicit local recording action, never enabled by opening this app.

This preview implements profiles/settings, local maps, manual markers, progress,
history/undo, interchange, backups and experimental offline recording analysis.
Obstacle-aware routes remain future work. Live matching and calibrated recordings
can estimate positions from minimap images; see [LOCALIZATION.md](LOCALIZATION.md).
The planned first feature release remains **v0.1.0 Alpha**.

## Run

Extract the complete Windows preview ZIP into a writable folder and run
`TeyvatCompanion.exe`. Keep the DLLs, `platforms`, `sqldrivers`, `imageformats`, `tls`, `translations`
and `qt.conf` alongside it. No Qt installation or administrator account is needed.
The build is unsigned and has not been tested on a clean Windows installation.

The Start, Local profiles and Settings pages in Advanced tools are functional. Create a profile,
rename it, switch profiles using the selector or profile list, and choose English,
Ukrainian or Russian, dark/light theme and 100/125/150% interface size. New profiles
start in English. The compact window also offers the language choice under
**Settings → Language**. These preferences save immediately to that local profile.
Window geometry and the last active
profile are saved too. OS DPI scaling remains Qt-managed and is separate from
the optional interface-size setting.

Local profiles are names chosen on this computer, not HoYoLAB accounts.
No password, UID, cookies or tokens are requested. Profile deletion is not provided.

## Local map and progress

Open **Local map**, expand **More tools**, choose **New layer**, and enter a name, region/level and local
bounds. No official geography or chest dataset is bundled. Alternatively, import
your permitted JSON/CSV data; [the format](INTERCHANGE.md) and a synthetic example
are provided with the source.

Drag to pan, scroll or use +/− to zoom, and press F to fit. Normal-mode double-click
zooms. Expand More tools, then double-click a point or choose Add marker to enter its name, category, coordinates and notes. Marker
definitions are shared by profiles; collected statuses are isolated by profile.

Select pins or table rows, using Ctrl/Shift for batches. Choose a status and Apply
to selected. Collected/Manually confirmed count as confirmed; Possibly collected
and Unknown do not. Undo reverses the latest progress command and appends reversal
events. Repeated undo works; metadata/background changes are outside its scope.

Search names/notes/IDs and filter category/status. Archive/restore changes marker
visibility for all profiles without deleting progress. Enable Show archived to
reactivate markers. Progress/history shows regional summaries and pages of 250
events, with UTC timestamps displayed in the local timezone.

Optional PNG backgrounds require source and licence/permission descriptions.
Use only own/permitted images. Limits: 16 MiB, 4096×4096 pixels and 128 MiB of
stored PNG bytes across the database. Images are stretched to declared bounds
without automatic geolocation; coordinates are local units, not metres. PNGs are
included in database backups, not JSON/CSV. Filters and viewport state are session-only.

Import validates off the UI thread, previews counts and preserves local progress
by default. Replacement is explicit; changed data invalidates the preview. Export
includes all markers in the active layer, including archived/filtered rows. Use
a database backup for layers exceeding the 8 MiB interchange limit.

## Official map and sign-in

Choose **Open official map** to open the fixed
[official HTTPS address](https://act.hoyolab.com/ys/app/interactive-map/index.html)
in the regular browser. Sign in on that website to save opened-chest marks there.
The application neither sees nor checks the browser's signed-in state. This
preview does not update official marks automatically. That integration remains a
development goal; browser sign-in alone does not enable it or authorize private
API calls. Local tracking remains independent. Experimental local recordings can
update local statuses automatically; live game recognition is not implemented.
Follow [the automation guide](AUTOMATION.md) to try the bundled synthetic example.

## Storage

Qt resolves the per-user local application-data directory; on Windows the default
is `%LOCALAPPDATA%\TeyvatCompanion`. The database is `companion.db`. It may have
`-wal` and `-shm` companions while open. An `application.lock` prevents two
application instances from opening the same data directory simultaneously.

Keep the data directory when replacing or removing preview program files.
The development ZIP itself is not a portable-data mode. An explicit directory
override is available:

```powershell
.\TeyvatCompanion.exe --data-dir "D:\My local data\Teyvat"
```

To copy data manually at this phase, close every application instance first and
copy the entire data directory, retaining any WAL files. Do not copy just the main
database while it is open. Compass options live separately in `compass.ini`.
Prefer **Backup database** for a consistent live
snapshot. Choose a new filename; existing files are never overwritten.

Schema upgrades produce a consistent `.before-vX-to-vY-<id>.bak` snapshot beside
an existing database before a transactional migration. A new empty database needs
no pre-migration snapshot. A failed migration rolls back. Corrupt, unrecognized
or newer databases cause a startup error; they are not silently reset or downgraded.

**Restore backup** replaces every profile, setting, map, marker, progress record,
history event and image after confirmation. It validates a read-only source,
saves current data as `.before-restore-<id>.bak`, and copies rows transactionally.
Corrupt or incompatible backups are rejected. Restore accepts schema v6 and
upgrades compatible v5 backups in a temporary copy.
Older migration snapshots remain intact and can be opened with their older build.
The source backup is never modified.

If corruption prevents startup, use a fresh `--data-dir` and restore a compatible
known-good backup there. Preserve the damaged directory for diagnosis.

## Diagnostics

```powershell
$process = Start-Process .\TeyvatCompanion.exe -ArgumentList '--check-storage' -Wait -PassThru
$process.ExitCode
```

`--check-storage` initializes/checks the local database and exits with 0 on success,
1 on failure, without opening the main window. It checks the runtime platform and
SQLite plugin as well. Use `--data-dir` to select a separate diagnostic workspace.
The mode never launches a browser. Startup errors explain the affected directory;
keep the database and migration snapshots when diagnosing an error.

## Libraries and source

This preview dynamically links Qt 6.11.3 Core, Gui, Widgets and Sql, with the QSQLITE
driver. Background work uses Qt Concurrent templates and Qt Core's thread pool.
The preview folder includes upstream notices, runtime notices, Qt package
SBOMs, the original project source ZIP and the complete Qt Base 6.11.3 source archive
under `source/`. The project source contains exact dependency URLs/hashes and
build scripts. Qt Tools are used only for building translations and are not shipped.

Qt is used under LGPLv3; original project files use MIT. The DLLs remain replaceable
with ABI-compatible builds. You may modify the application and replace/relink its
Qt libraries, including debugging modifications, subject to the applicable licences.
No restriction on reverse engineering for debugging library modifications is imposed.
See the shipped licence texts and `THIRD_PARTY_NOTICES.md`. The upstream SBOM
describes the full Qt Base package; the runtime file list describes this smaller
preview distribution. No HoYoverse assets or Windows fonts are redistributed.

