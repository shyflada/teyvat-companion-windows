# Windows privacy

Navigation samples visible regions of the Genshin Impact window while it is
active. Live frames are processed in memory. The optional recording tool saves
captures locally when the user starts a recording.

Local profiles, progress, captures, cache and application settings are stored
under `%LOCALAPPDATA%\TeyvatCompanion` by default. Keep that directory private
and use an empty test directory for screenshots, diagnostics and packaging.

Public map resources are downloaded from HoYoLAB over HTTPS. Account sync is
optional. Sign-in takes place on the official site in a separate WebView2
session. The app reads the signed-in state and chest pin IDs and uses the
site's normal marker controls. HoYoLAB receives the corresponding pin updates.
Passwords, game UID, cookies and session tokens are handled by the site and browser.

The release contains code, branding and synthetic examples. No game data cache,
account session or user recordings ship with it.
