# FE3H Deck Recomp - Installer

Keep all files in this folder together. Open **Install.desktop** in SteamOS Desktop Mode and select your own FE3H 1.2.0 **exefs** and merged **romfs** folders. Extracted DLC is optional.

The installer uses the bundled AppImage to prepare and compile the game. Compilation may take a long time. After successful installation, start **FE3H Deck Recomp** from the desktop shortcut. The shortcut uses the project icon, copied into the installed data folder.

## If the installer fails: use the AppImage directly

Close the installer first, then launch **FE3H-Deck-Recomp-0.2.6-linux-x86_64.AppImage** directly in Desktop Mode.

If the game is not already detected, open **Settings > Game Files**, select the folder containing your own FE3H 1.2.0 `exefs` and merged `romfs`, and run **Set up game files**. Wait for copying and compilation to finish, then choose **Play**. Your input files still need to be complete and valid.

If compilation had already finished before the installer reported an error, preserve the retained work folder and logs: the completed build may be recoverable without compiling it again.

**Important:** with this direct method, the AppImage itself remains your executable. Keep it in a permanent location, such as `~/Games/FE3H-Deck-Recomp/`, and add it to Steam manually if desired. Do not delete it with the downloaded package. The managed desktop shortcut is created only when the installer completes successfully.

## Required Python modules

- `manage.py`: installation, validation and managed launching.
- `install_core.py`: locking, recoverable replacement and integrity checks.
- `game_files.py`: game version, file-copy and DLC validation.
- `processes.py`: stopping only the installer/game process session, including compiler children.

All four modules are required by this installer. Do not remove or rename them. `release.json` identifies the bundled AppImage and its expected hash; `icon.png` supplies the installed shortcut icon.

## Installed files

Graphical default: `~/Games/FE3H-Deck-Recomp`, or your selected folder. Command-line default without `--root`: `~/.local/share/fe3hrecomp`. It contains the prepared game, a copy of the AppImage, saves, configuration and installed launcher. After installation through Install.desktop completes successfully, this downloaded package may be removed. This does not apply to the direct-AppImage fallback: keep the AppImage you use to play. Do not remove the installed data folder.

This installer includes the fix for the false failure reported when closing the graphical importer after successful compilation. Existing saves and settings are preserved. There is no need to reinstall a successfully completed installation.

## Licenses and sources

Keep `LICENSE`, `THIRD_PARTY.md`, `UPSTREAM.md` and `licenses/` with this package. The complete corresponding sources are available separately from the project's release page:
https://github.com/Rio77/fe3h-deck-recomp/releases

No game files, DLC or cloud-save credentials are included.

Graphical installation now offers `~/Games/FE3H-Deck-Recomp` or another exact destination folder. Existing installations are not moved. Command-line `--root` selects an exact destination; command-line defaults remain unchanged. Reuse the same destination to update an installation.


### 0.2.4: Settings launcher

Open **Settings.desktop** in the installed folder (or **Settings** on the desktop) to configure mods before starting the game. It uses the same data folder and locks as the normal launcher. Close the running game first. Mod changes take effect on the next start. Opening the internal AppImage directly may select a different data folder. This release updates Linux packaging and launchers; the game engine remains the validated opt5 build, not upstream engine 0.2.4.


### 0.2.5: file checks and text entry

This release selectively adds advisory SHA-256 checks for four small base-game files, Unicode name conversion and character limits, UTF-8-safe deletion, and Ctrl+A/C/X/V/Z editing shortcuts. Hash mismatches warn and allow continuing; existing structural errors remain errors. Checks apply before data mods are layered. This is not a full content verification of the game or DLC.

The name entry supports Unicode code points without splitting multibyte text. Glyph availability depends on installed fonts; Korean fonts are loaded only when needed. The controller keyboard keeps its Latin layout. Linux clipboard work is performed on the UI thread without making the render thread wait. The current engine remains the optimized opt5 fork with these selected fixes; no upstream updater, menu resizing, dimming or temporal upscalers were added.
