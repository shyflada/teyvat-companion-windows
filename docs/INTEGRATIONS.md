# Official map integration, beta.6 — 2026-11-10

The source is the [official HoYoLAB map](https://act.hoyolab.com/ys/app/interactive-map/index.html).
Its anonymous first-party resources were inspected in an isolated browser context
without an account, credentials, session cookies or private progress requests.
This is an observed website format, not a documented third-party API contract
or a claim of HoYoverse approval. The implementation stops on schema changes.

The application makes HTTPS GET requests to these observed public resources:

- `sg-public-api-static.hoyolab.com/common/map_user/ys_obc/v3/map/info`
- The same host/prefix, `v2/map/label/tree`, `v3/map/point/list?label_ids=13`
  and `v2/map/point_group`. `v3/map/point/list?label_ids=1` supplies optional
  public waypoints/statues/domains; it supplies no account unlock state.
- `act-webstatic.hoyoverse.com/map_manage/map/{map_id}/{map_version}/{x}_{y}_{N|P}{zoom}.webp`.
- Observed official floor PNG URLs on `act-upload.hoyoverse.com` or
  `act-webstatic.hoyoverse.com`, restricted to HTTPS with bounded images.

Requests include only public map IDs, language, label ID and tile coordinates.
Cookies are neither loaded nor saved; redirects and foreign hosts are rejected.
Downloads are bounded and cancellable, TLS certificate checks remain enabled,
and Windows uses Qt's Schannel backend. No screenshot, pose, progress, profile,
author field, password, UID or token is uploaded. There are no account writes.

Each Start refreshes the selected map's metadata and complete ordinary chest list.
Counts must agree with the five ordinary chest labels. Duplicate IDs, partial
responses, unknown floor references and changed categories reject the update.
A complete snapshot replaces the cache atomically; failure retains the prior
valid snapshot with a visible notice. Tile versions and a checksum-protected
feature index are cached separately. Chest updates are checked even if the art
version has not changed. Runtime snapshots omit public author/account fields.
Optional sanitized area membership, waypoints and floor geometry have a separate
checksum inside the backward-compatible snapshot. Legacy snapshots remain usable;
they cannot supply region/floor/waypoint metadata until a successful refresh.

Map IDs currently verified: 2 (Teyvat), 7 (Enkanomiya), 9 (The Chasm: Underground
Mines), 36 (Ancient Sacred Mountain). New ordinary chest IDs in an existing map
need data refresh, not a new coordinate database from GitHub. New map IDs or a
changed public format require an adapter update.

The page's first-party MhyGameGis projection is `(lng + origin_x, lat + origin_y)`
at zoom 0, y downward. Raster coordinates at zoom z are multiplied by `2^z`.
Stored world coordinates are raw point XY plus a fixed 500000 offset per axis.
That offset is independent of the changing raster origin. Source and coordinate
frame are scoped by map ID; floor identity includes both group ID and floor ID
(floor 0 occurs in multiple groups). Local statuses survive metadata updates;
removed imported definitions are retired, preserving history and progress.
Legacy community IDs are never transferred by approximate proximity.

The v2 global SIFT index is built from zoom -2 tiles once per art revision. Bright
snow is retained rather than masked as UI. Minimap proposals use three input
sizes; sparse coarse areas require at least 24 detailed inliers and 75% support.
The coarse candidate is never used as the live player position. World-map
acquisition requires a unique cyan player marker and distributed, consistent
terrain matches. Panning does not move the inferred player to the map center.
Gameplay uses a moving 1024px local reference at zoom 0, with ORB/SIFT and HUD
and geometric gates. Minimap-only startup/recovery uses global low-resolution
proposals followed by fresh detailed reference verification; a proposal is never
a position. A selected region prioritizes search, while exact official `area_id`
membership limits targets. Regional viewport rectangles are not region borders.
Dragonspine is an explicitly approximate vicinity around an official statue.
The standard HUD presence crop is learned only after the
centered cyan player icon and high-contrast Eye icon are both recognized. It
stays in memory. An optional personal HUD preset overrides the standard layout.
Camera heading comes from the bright viewing sector relative to this terrain,
independently of the avatar's blue arrow; it is sampled every 60ms while position
matching is throttled to 250/500ms. Neither interval is a real-time guarantee.

After misses, local matching continues and global recovery retries are throttled.
Last verified XY is persisted locally as a search hint, never an accepted pose.
Spatially distributed fallback features improve sparse terrain coverage. Narrow
coastal matches need independent dense image agreement; their increased uncertainty
blocks automatic collection. Missing or old samples do not retain live guidance.

Around a known XY hint, the matcher can compare nearby official floor overlays.
A uniquely matched overlay switches the navigated layer; indistinguishable floors
abstain. This does not provide global underground acquisition or measured height.
Inspected point data exposes XY and symbolic level references, not numeric player/
chest Z. No verified traversability graph or fastest-path/ETA source was found.
Suggested same-level teleports (up to three alternatives) are conditional horizontal shortcuts: account unlock
and access are unknown. They open the official map, never control game input.
The official entrance to a recognized underground group can also be opened on
the map. Its coordinates do not prove a walkable route to each member floor.
Puzzle-trigger completion and map-text OCR are not implemented. The real CDN
floor-PNG path is covered by the live native download check. Stable surface
tracking compares cached nearby floors periodically without starting floor downloads.
Map 2's approximate metre scale was checked
against in-game distance-counter differences; other maps do not display
unverified metres. The screen badge indicates course, not a 3D projection.

Account sync uses the actual official website in a native WebView2 window.
The user signs in directly there, once; its separate browser profile owns the
session. Existing external-browser cookies are never imported. The adapter reads
only the login boolean and ordinary chest point IDs, selects the exact official
point popup, and clicks its existing non-batch Mark/Unmark button. No application
code constructs private authenticated HTTP calls or reads credentials, UID,
cookies or tokens. Observed Vue/Leaflet UI structures are not a public API contract;
a changed page fails without acknowledging or dropping the queued local change.

The committed SQLite history is the durable outbox. A per-local-profile cursor
advances after the official page's server-backed marked state confirms the exact
point and desired status. Repeated requests check state before clicking. Undo
queues the corresponding reversal. A cursor command identity detects restored
history IDs. Remote collected point IDs are cached as navigation exclusions,
without manufacturing local collection history or changing Undo. Profile changes
pause account sync until the user connects that local profile explicitly.

The anonymous real WebView2 runtime and login-required state are smoke-tested.
Exact-point guards, no duplicate toggles, deferred acknowledgement and Undo are
checked with an isolated UI fixture. Authenticated writes on the owner's account
and additional sign-in methods still require user testing. This implementation
does not establish HoYoverse authorization or an account-safety guarantee.

Official map content remains HoYoverse content. It is cached for this local
experiment and excluded from public/runtime/source ZIPs. This project does not
relicense it or present an alternative hosted map service. Game files, game
memory, anti-cheat, hooks, injections and game input automation are not used.
