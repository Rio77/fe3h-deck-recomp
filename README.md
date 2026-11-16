# FE3H Deck Recomp

[Español](README.es.md) · [Installation and recovery](docs/linux.md) · [Validation](docs/validation.md)

A native Linux adaptation of *Fire Emblem: Three Houses*, optimized and tested on the **Steam Deck LCD**. Based on **FE3H Recomp by onerag90**, itself built on **LASRecomp by Peiikai**. This is an independently maintained fork, not an official release of either upstream project.

> [!IMPORTANT]
> This project and the Linux adaptation were developed with AI assistance and human direction, testing and review. It is shared as-is; testing does not cover an entire playthrough or every device.

> [!NOTE]
> No game files, DLC, generated game code, extracted game shaders, console fonts or saves are included. Supply your own legally obtained **FE3H 1.2.0**, with the update already merged into extracted `exefs` and `romfs` folders. Extracted DLC is optional. Instructions for obtaining game files are not provided.

## What this is

The static recompiler translates the original AArch64 code to C and compiles it locally. The native runtime implements the system services the game uses, with Vulkan graphics and native audio/video libraries. Playing does not require Wine or Proton. The original FE3H and LASRecomp projects provide the foundation; this fork selectively adapts their code and adds the tested Linux optimizations documented in [UPSTREAM.md](UPSTREAM.md).

## Status

Current version: **0.2.3-linux-opt5**. Tested on a Steam Deck LCD in dock mode, retaining adjustable internal resolution, audio and movies. Linux x86-64 is the release target; other devices have not received the same validation. This repository does not distribute or maintain a Windows build.

New opt5 work includes lower CPU busy-waiting, profile-specific pipeline prewarming, resource ownership fixes for internal restarts, and selected upstream fixes for GPU waits, textures, input, audio, movies, saves and mods. Structured lighting, native half translation, sparse local memory and dock behavior were already present in opt4.

At `match`, paired monastery tests improved p99 frame time by **2.95%** and APU power by **3.51%** versus opt4. Daytime battle mean FPS improved **2.15%**, with essentially unchanged power and **1.71% worse p99**. One battle pair had a thermal-comparability caveat. These are scene-specific measurements, not a guarantee of 60 FPS everywhere or at x1/1080p. [Full evidence and limits](docs/validation.md).

## Requirements

- SteamOS / Linux x86-64; AppImage built for glibc 2.35 or newer.
- AVX2/FMA-capable CPU and a Vulkan 1.3 GPU/driver. The Deck LCD is the tested hardware.
- Python 3.10+ and `kdialog` for installation; SteamOS already provides them.
- Writable storage for the game, a separate staged installation and temporary compilation files. Reinstallation also retains the previous extracted copy for recovery.
- Your own extracted FE3H 1.2.0 `exefs` and merged `romfs`; optional extracted DLC.

## Installing a release on SteamOS

The release provides one Steam Deck installation package, which already contains the AppImage. Download that installer to play. The project source ZIP/TAR.GZ and complete corresponding-source TAR.GZ are available separately for building and license compliance; they are not extra installation steps.

1. Download `FE3H-Deck-Recomp-0.2.3-linux-opt5-steamdeck.tar.gz` from [Releases](https://github.com/Rio77/fe3h-deck-recomp/releases/latest). Extract it into a writable folder in Desktop Mode.
2. Keep the package together and open **Install.desktop**. Allow execution if KDE asks. Select your `exefs`, merged `romfs`, and optionally your DLC directory.
3. Wait for local copying and compilation. In the full Deck import test this took about 43 minutes; other hardware and input/cache conditions vary. This is separate from normal game startup.
4. Open **FE3H Deck Recomp** from the desktop. To play in Game Mode, add the installed **FE3H.sh** to Steam as a non-Steam game manually.

Graphical installation recommends `~/Games/FE3H-Deck-Recomp` and allows another destination. The command-line default remains `~/.local/share/fe3hrecomp`; saves are in `save`. New Deck installations default to dock mode and `match`. Use **F1** or **Back + Start** for settings. The installer does not modify SteamOS or silently edit the Steam library.

**Updating and recovery:** use the complete installer, not an upstream automatic updater or a blind overwrite. It prepares and verifies a separate installation, preserves saves/configuration and retains the previous extracted data. See [installation and recovery](docs/linux.md) for command-line options and `status` / `recover`.

### If Install.desktop fails

Close the installer and open the bundled AppImage directly in Desktop Mode. If necessary, use **Settings > Game Files** to set up your own FE3H 1.2.0 files, wait for compilation, then choose **Play**. This does not bypass game-file validation. If a build already finished, preserve its work folder and logs for recovery.

When using this fallback, **keep the AppImage** in a permanent folder and add it to Steam manually. It is not automatically copied by the managed installer, and no managed shortcut is created by this direct method. Keep the prepared data folder too.

## DLC and mods

Provide DLC as folders such as `dlc/010055D009F79001/romfs/...`; all six packs are supported when supplied and validated. DLC is optional. Mods are data-only and live in `extracted/mods`, separate from the original romfs. Import folders in Desktop Mode, then enable, disable and order them in the interface. The last enabled mod takes priority; ordering applies on the next boot. No mods are enabled by default. Exefs/code mods are not supported by this installer. Saves from gameplay-changing mods may require the same mods on another installation.

## Building and testing

Use `make appimage ARCH=x86_64` with podman and network access for build dependencies. Application builds and synthetic CI tests do not need game files; game code is generated only from the user’s files. See [build instructions](docs/linux.md), the pinned dependency recipes and `.github/workflows/linux.yml`. The [first public CI run](https://github.com/Rio77/fe3h-deck-recomp/actions/runs/43784595531) passed the application build and synthetic checks.

## Known issues and limits

- A pre-existing combat-opening blur/trail issue occurs at `match`, also seen in opt4. It was not visible in the manual x1 check; higher resolution substantially increases GPU cost.
- Very large effect shaders remain excluded on the Deck; some effects may lack a layer. First-time shader compilation can cause temporary stutter or incomplete effects.
- Tests include internal restarts, a separate 30-minute continuous session and earlier physical controller/suspend checks. They do not prove every future scene or a multi-hour session is leak-free.
- Full identical-input opt4/opt5 framebuffer comparison remains uncompleted. Promotion was explicitly authorized with this limitation documented.
- The public version saves locally. It contains no OneDrive synchronization, cloud configuration, access keys or credentials.

For a bug report, include version, SteamOS/driver version, resolution, dock setting, reproducible steps and a redacted log. Do not upload game files, generated libraries, extracted shaders, saves containing personal information or credentials.

## Credits

- **[FE3H Recomp](https://git.onerag90.de/recomps/fe3h-recomp), by onerag90 and contributors** — the original Three Houses adaptation, game-specific services, audio, movies, shaders and tools.
- **[LASRecomp](https://github.com/peiikai/LASRecomp), by Peiikai and contributors** — the foundational static recompiler, runtime, renderer and tooling. See its README for the full upstream credits.
- **Ryujinx contributors** — the audio renderer’s reverb and auxiliary processing, under MIT; notice retained in `third_party/ryujinx/LICENSE.txt`.
- **Dear ImGui, SDL, FFmpeg, shaderc/glslang/SPIRV-Tools, Vulkan Memory Allocator and SMAA contributors** — interface, multimedia and graphics components.
- **[AMD FidelityFX Super Resolution 1](https://github.com/GPUOpen-Effects/FidelityFX-FSR)** — retained upstream shader helpers under MIT; no new opt5 FSR features were enabled.
- **Capstone, Unicorn, python-lz4, PyInstaller, Python and Zig contributors** — local generation, compilation and packaging.
- **Baloo 2 and EB Garamond authors**, including Georg Duffner — the distributed open fonts.
- **Switchbrew and the Switch homebrew community** — hardware and system documentation.

Exact component versions, licenses and redistribution notices are listed in [THIRD_PARTY.md](THIRD_PARTY.md) and bundled in the AppImage. Credits apply to the components actually retained; this fork does not ship LASRecomp’s randomizer or the excluded FSR integration.

## Legal and license

This is a fan project, not affiliated with or endorsed by Nintendo, Intelligent Systems or Koei Tecmo. *Fire Emblem* and its characters/emblems remain the property of their respective owners.

Project code retains the upstream **[MIT License](LICENSE)**, including the copyright notices of Peiikai and onerag90. Third-party components keep their own licenses. The installer includes notices in `licenses/`. The complete corresponding-source archive, including FFmpeg and Unicorn, is a separate TAR.GZ download in the same release. Distribute those sources and notices alongside the binaries. See [THIRD_PARTY.md](THIRD_PARTY.md).

The application icon was supplied by the maintainer for this distribution. It is not covered automatically by the code’s MIT license, which grants no rights to the depicted characters or emblems. The optional launcher background is generated locally from the user’s files.

Graphical installation now offers `~/Games/FE3H-Deck-Recomp` or another exact destination folder. Existing installations are not moved. Command-line `--root` selects an exact destination; command-line defaults remain unchanged. Reuse the same destination to update an installation.
