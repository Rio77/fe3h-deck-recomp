# FE3H Recomp â€” Linux opt5

Native Linux fork of FE3H Recomp, optimized and tested on a Steam Deck LCD. Based on [FE3H Recomp](https://git.onerag90.de/recomps/fe3h-recomp) and [LASRecomp](https://github.com/peiikai/LASRecomp).

The game, DLC, generated game library, shaders extracted from the game, saves and caches are **not included**. Supply your own extracted FE3H **1.2.0 base game with the update applied**, as `exefs` and `romfs` folders. Extracted DLC is optional. Packed NSP/NCA/XCI files are not installer inputs.

## Install on SteamOS

1. Extract the Linux distribution into a writable folder in Desktop Mode.
2. Open `Install.desktop` and select your `exefs` and merged `romfs` folders. Optionally select the folder containing the extracted DLC packs.
3. Wait for the graphical importer to copy and compile the game locally. A working installation is kept until the replacement has been verified.
4. Start **FE3H Linux** from the desktop. Add the installed `FE3H.sh` to Steam manually to use Game Mode. The installer does not edit the Steam library.

SteamOS supplies Python 3 and kdialog. Other Linux installations need these for the installer, a Vulkan 1.3 driver and an x86-64 CPU with AVX2/FMA. Other GPUs have not received the same validation as the Deck LCD. Allow space for a separate complete copy of the game and temporary compilation files. No SteamOS system packages or read-only system settings are changed.

The default data folder is `~/.local/share/fe3hrecomp`. Saves remain in its `save` folder. Configuration and saves are retained when reinstalling. The Deck launcher keeps dock mode and starts new installations at `match`; available resolutions remain adjustable in F1 â†’ Video. Closing the application releases its process memory; suspension retains it.

Command-line installation and recovery are described in [docs/linux.md](docs/linux.md). Reinstallation and updates use the installer so the previous application and game files remain recoverable. The private cloud-save synchronization used during development is not required or included.

## Changes and limits

- Lower CPU busy-waiting, a rendering-profile-specific pipeline prewarm list, and explicit ownership of resources retired during an internal restart.
- Selected upstream fixes for GPU waits, textures, pipelines, controller input, audio periods, movie buffers, saves and data mods.
- Dock behavior, native half translation, selected structured lighting, sparse local memory and direct romfs reading retained from opt4.
- The additional lighting compiler experiment, packed-half experiment and descriptor-reuse experiment were not accepted.

At `match`, three paired monastery runs improved p99 frame time by about **2.95%** and APU power by **3.51%** compared with opt4. Daytime-battle runs improved average FPS by about **2.15%**, with essentially unchanged APU power and a **1.71% worse p99**. The first battle pair had a thermal-comparability caveat. These are scene-specific measurements, not a promise of 60 FPS everywhere or at x1/1080p. See [docs/validation.md](docs/validation.md).

There is a pre-existing combat-opening blur/trail issue at `match`, also observed in opt4. It did not reproduce visibly in the manual x1 check. Higher resolution can substantially increase GPU cost.

Data mods for the base game and DLC live in `extracted/mods`, separately from the original romfs. No mods are enabled in a fresh installation. Add folders in Desktop Mode, then enable, disable or order installed mods from the game interface. The last mod has priority. A save made with gameplay-changing mods may need the same mods on another installation; mods are not automatically synchronized.

## Build, provenance and licenses

See [docs/linux.md](docs/linux.md) for the Linux build, synthetic tests and FE3H smoke test. CI does not need the game. The public reference is `v0.2.3`, commit `4e21e7bff4a2df979e57ca82639c72931c726423`; selected later fixes and the native fork's changes are documented in [UPSTREAM.md](UPSTREAM.md).

Project code retains the [MIT license](LICENSE) and upstream notices. Third-party components retain their own licenses. The standalone generator includes Unicorn; its notices and corresponding source archive accompany the distribution. See [THIRD_PARTY.md](THIRD_PARTY.md). The application icon in `docs/icon` was supplied by the maintainer for this distribution; it is not covered by the code’s MIT license. The launcher background, when available, is generated locally from the user's movie files.
