# Changelog

## v1.0.4 — ReproVR only runs in the games you choose

v1.0.3 registered ReproVR for **every** OpenXR game on the PC. Users saw it start in
games they never installed it in (for example Cyberpunk 2077 reporting "OFXR is running"
after installing ReproVR only for another game). v1.0.4 fixes this and repairs PCs that
were set up by v1.0.3.

### Fixed
- **ReproVR is active only in the games you install it in.** The setup tool and the game
  loader no longer register ReproVR for all OpenXR apps.
- **Automatic repair of v1.0.3 installs.** Opening the new `reprovr_setup.exe` removes the
  old all-games setting. Before removing it, ReproVR is set up again inside each game that
  used it, so those games keep working.
  - Windows asks for permission once when the old setting was saved for all users.
  - A game whose files are in use (game still running) keeps the old setting until the
    tool is opened again with the game closed.
  - If nothing shows that a game can start ReproVR on its own, its old setting is kept for
    now so the game is not broken.
  - A backup is saved first. `reprovr_setup.exe --restore-legacy` puts the old setting back.
  - Only ReproVR's own settings are touched; other OpenXR layers are left alone.
- **No more settings files in games ReproVR is not installed in.** The game list also shows
  VR games found automatically in Steam; changing settings there used to create
  `repro_vr.ini` / `ofxr_bridge.ini` in that game's folder. Settings now require ReproVR
  to be installed for that game first.
- **Better choice of loader DLL.** ReproVR now uses a DLL name the game really loads
  (`version.dll`, `winmm.dll` or `dxgi.dll`) instead of always trying `version.dll` first.
  Example: Skyrim VR loads `winmm.dll`, not `version.dll`. Helper programs such as crash
  reporters are ignored. Another mod's DLL is still never overwritten.

### Changed
- When the tool cannot confirm that a game will start ReproVR on its own, it shows a short
  information message after installing.
- Setup messages are shorter and written for players, not developers.

### For advanced users
- `reprovr_setup.exe --repair-legacy` runs the repair without the window
  (`--dry-run` lists what would change, nothing is modified).
- A log of the repair is written to `%APPDATA%\ReproVR\migration.log`.

### Known issue
- The `reprovr_setup --self-check` layout test fails; it already failed in v1.0.3 and does
  not affect the tool.
