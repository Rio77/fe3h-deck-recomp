# Third-party notices and corresponding sources

The runtime, generator and compiler are separate components. Each keeps its own license. Full notices are collected from the exact build inputs into `usr/share/licenses/fe3hrecomp` inside the AppImage. The installer kit exposes these notices in `licenses/` without extracting the AppImage.

| Component | Origin / license information |
|---|---|
| FE3H Recomp / LASRecomp code | MIT; root LICENSE retains upstream copyrights |
| Ryujinx audio-derived reverb and auxiliary processing | MIT; third_party/ryujinx/LICENSE.txt |
| AMD FidelityFX Super Resolution 1 retained shader helpers | MIT; third_party/fsr/LICENSE.txt; no new opt5 FSR features enabled |
| Dear ImGui | MIT; third_party/imgui/LICENSE.txt |
| SMAA reference implementation | MIT; third_party/smaa/LICENSE.txt |
| Baloo 2 and EB Garamond | SIL Open Font License; individual OFL.txt files |
| SDL 3.4.16 | zlib license; collected from the pinned SDL source |
| FFmpeg 9.0.2 | Shared LGPL build; no GPL/nonfree codecs enabled; complete source archive accompanies the source distribution |
| shaderc, glslang, SPIRV-Tools / headers | Their Apache/BSD notices are collected from the source trees used to build shaderc |
| Vulkan Memory Allocator | MIT notice included; system Vulkan loader and GPU driver are not bundled |
| Zig 0.17.0 and its compiler/runtime files | Zig and bundled LLVM/Clang/runtime notices retained |
| Python runtime and helper libraries | PSF and individual system-library notices collected from the build environment |
| Capstone, lz4 and Python packaging dependencies | Individual notices and exact package versions in components.json |
| Unicorn 2.1.4 | GPLv2; complete source, including its core engine, accompanies the source distribution |
| PyInstaller | Full license and bootloader exception included with the notices |

The generator is frozen from the project's Python sources using `tools/linux/rando_helper.sh` (the inherited filename is historical; it builds no randomizer). Its package report records wheel URLs and hashes. `tools/linux/Containerfile`, `build_deps.sh`, `fetch_zig.sh`, the helper script and PyInstaller spec describe the build and installation steps. No game files are needed to rebuild the application or generator.

The complete corresponding-source TAR.GZ is distributed separately beside the installer and AppImage in the same public release. It includes the project sources, FFmpeg and Unicorn archives and their checksum manifest. Keep those sources available alongside the binaries, and retain the installer’s `licenses/` notices. These files contain no game data. The project's own generator sources and build scripts are included too. No private source-download credentials are required.

Users may rebuild or replace components. For a modified AppImage, regenerate the local release checksum manifest before using the installer; there is no signature lock or remote authorization service. An AppImage can also be extracted to inspect its files and notices. The bundled shared FFmpeg libraries can be replaced in a rebuilt AppDir.

The application icon was supplied by the maintainer for use in this distribution and repository. The code license does not grant rights to the characters or emblems depicted in that image. Nintendo fonts, extracted game artwork, game libraries, game shaders, movies and game data are not included. Optional console fonts and the launcher background are obtained locally from the user's own files.


Selective 0.2.5 changes originate from onerag90/FE3H Recomp: SHA-256 validation (36e5ccd931), Unicode name entry (a0e066253d, e653bd1aae), keyboard modifiers (066406d366), and the upstream MIT SHA-256 implementation. Linux clipboard threading and UTF-8-safe deletion are adaptations in this fork. No upstream menu resizing, dimming or upscaler changes are included.
