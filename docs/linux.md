# Linux build, installation and recovery

The supported target of this fork is x86-64 Linux, with validation on a Steam Deck LCD. SteamOS is not modified. The Windows installation and private cloud-save integration used during development are independent of this distribution.

## Installer

The release kit contains an AppImage, `Install.desktop`, `Install.sh`, the Python installer modules, the application icon, notices and checksums. Keep those files together. In Desktop Mode, open `Install.desktop`; allow execution if KDE asks. Select the extracted `exefs`, the merged base-plus-update `romfs` for FE3H 1.2.0, and optionally a directory containing DLC packs:

```
dlc/
  010055D009F79001/romfs/...
  010055D009F79002/romfs/...
```

Packs are optional; they must be original extracted FE3H DLC. The fifth pack legitimately has an empty DATA1.bin. Module build identifiers, application metadata, the merged romfs layout and DLC files are validated before copying. This does not use the generated library's compatibility ID as the game version.

Command line, from the release kit:

```sh
python3 manage.py install --headless --exefs "/path/to/exefs" --romfs "/path/to/romfs" --dlc "/path/to/dlc"
# Base game only:
python3 manage.py install --headless --exefs "/path/to/exefs" --romfs "/path/to/romfs" --no-dlc
```

Use `--root "/path/to/FE3H data"` to choose a dedicated installation folder. `--check-only` validates inputs without copying or compiling. No packed NSP/NCA/XCI input is supported. Keep unmodified base files and put data mods in `extracted/mods` instead.

The installer prepares a separate workspace, runs the graphical importer or its headless equivalent, and verifies the generated library with the bundled application before committing. Existing saves and configuration are outside the replacement. The new application is content-addressed, and the previous extracted folder is retained in `.recovery`. A durable journal recovers interruptions between directory replacement and application selection. Failed input/builds do not replace a working installation.

The initial profile is dock at `match`; F1 → Video can change resolution. The AppImage includes Deck profile defaults, and the installed `FE3H.sh` adds installation recovery and a launch lock. To add it to Steam, use Add a Non-Steam Game → Browse → All Files and select the installed `FE3H.sh`. No library entry is created silently. To reinstall or update existing game files, use the installer. The native import screen preserves an existing compiled installation instead of deleting it for an unverified replacement. `FE3H_DECK_PROFILE=0` disables embedded Deck defaults when running the AppImage directly for diagnostics.

The fallback game font is DejaVu Sans from the host. It is present on the tested Deck; other distributions need a TrueType DejaVu Sans font or their own supported console-font dump. Only OFL launcher fonts are distributed.

## Recovery

Close the game first. From the installed data folder:

```sh
python3 manage.py status --root .
python3 manage.py recover --root .
```

Recovery is also performed before the managed launcher starts. It either finishes a verified replacement or restores/preserves the previous files. It refuses ambiguous or unsafe paths rather than deleting copies. The `.recovery/extracted-<id>` folder retains the previous game installation; `save` and `fe3hrecomp.ini` remain in the data root. Keep the previous AppImage if manually restoring an older generated-library format. Do not combine files from different extracted backups. Preserve any journal and workspace if recovery reports that it needs attention.

An interrupted import can leave an owned workspace in `.installer-work`. These are not original user inputs. Successful installation removes its own temporary workspace. Failed workspaces retain diagnostics until reviewed; they are never used as playable game files.

## Rebuild

The binary installer kit keeps `Install.desktop`, `Install.sh`, the Python modules,
`release.json` and the AppImage together. Extract the whole archive before opening
`Install.desktop` from the SteamOS desktop. Dolphin may ask you to trust/execute
this launcher. Alternatively run `python3 manage.py install` in a terminal in that folder.
The installer creates a separate managed data directory and a desktop game
launcher; add that launcher to Steam yourself. Do not move only the desktop file
out of the kit before installation.

Requirements: podman, network access for pinned build dependencies and sufficient disk space. The Ubuntu 22.04 container builds for glibc 2.35+:

```sh
make appimage ARCH=x86_64
```

The package contains no generated game code. The generator, Zig compiler, their sources/build recipes and component notices enable local compilation from the user's files. Linux game compilation uses an explicit Zig target and does not require SteamOS development headers.

Synthetic checks, without game data, inside the build container:

```sh
DEPS=build/linux/x86_64/deps bash tools/linux/check_synthetic.sh
```

`tools/linux/audit_release.py` audits the actual source export and AppDir, separately from `.gitignore`. The release smoke test is FE3H-specific and uses an isolated data folder and copied save. Its private inputs and captures must not be committed or shipped. Full-game import tests require owned copies; CI runs only synthetic checks and a game-free application build.

The installer contains notices in `licenses/`; the complete corresponding-source TAR.GZ is a separate download in the same release. Distribute both alongside the binaries. Do not publish generated game libraries, extracted/translated game shaders, caches, saves, game images/movies, diagnostic captures or logs, private launchers, access keys, cloud tokens or cloud configuration.

To assemble the installer and copy its separate corresponding-source archive into the release output directory:

```sh
python3 tools/linux/release_kit.py /path/to/application.AppImage /path/to/output --sources /path/to/corresponding-sources.tar.gz
```
