# Validation evidence and limits

Test platform: Steam Deck LCD, SteamOS, native Vulkan, dock mode, `match`. LSFG was disabled only in test processes. No global LSFG configuration or system power settings were changed. The reported APU cap was 16 W in the paired tests.

Three alternating opt4/candidate pairs were measured for three minutes each per scene, after cache warmup. Pipeline compilation, configuration, power cap, frame progress and scene screenshots were checked separately from timed capture-heavy diagnostics.

| Scene | Mean FPS change | p95 frame-time change | p99 frame-time change | APU power change |
|---|---:|---:|---:|---:|
| Monastery | +0.39% | −2.28% | −2.95% | −3.51% |
| Daytime battle | +2.15% | −0.16% | +1.71% | +0.01% |

Daytime-battle means: opt4 about 55.38 FPS, candidate about 56.57 FPS. Process CPU use fell about 33.3%; this is not the same as an APU power saving. The candidate's GPU clock was higher under the same cap. The first battle pair differed by 3.15°C in mean temperature, above the 3°C diagnostic threshold; the other pairs differed by 0.97°C and 1.83°C and improved FPS by 1.69% and 1.94%. Do not treat all three pairs as thermally identical. Battle p99 worsened slightly in every pair, below the agreed 5% regression threshold.

The combined performance matrix used the memory5 executable. Follow-up ownership-only repairs produced memory7; its final battle comparison and functional checks passed. The source export additionally removes disabled experiments and changes installation/verification code. Release smoke-test results must identify the exact delivered artifact separately; the full performance matrix is not claimed to have been repeated for every packaging-only rebuild.

Memory6 completed 15 internal restarts and 16 movie openings. Retired NVN, IPC and kernel allocations were released, and signal-stack counts balanced; shared Vulkan reservations stayed constant. Memory7 added shared AM storage ownership and completed three targeted restarts/four openings. Synthetic ownership/concurrency and save-failure tests used sanitizers. Fifteen restarts have not been repeated on the exact memory7 executable.

A separate memory7 session ran for 1803.9 seconds without internal restarts, with two movie openings, a monastery save load, walking between the reception hall and adjoining passage/gallery, UI use and six repeated camera/movement rounds. It closed normally. RSS during the last 15 minutes ranged from 837.332 to 837.359 MiB; glibc active allocations in the last ten minutes ranged from 158.221 to 158.492 MiB. Substantial allocator capacity remained reserved but free. This is not proof that every future scene or a multi-hour session is leak-free. The automated session did not exercise repeated save loads or a full battle; these are separate evidence from prior tests.

The earlier manual check included video, combat, zone changes and three physical suspend/resume cycles with working image, audio and controller. Those physical checks were not repeated on each final packaging rebuild. Automated logs and screenshots do not constitute a new subjective audio assessment.

Opt4 showed a libMangoHud failure during an instrumented close, outside the timed window. The final candidate close and the prolonged run did not reproduce it. It has not been established as a normal-use crash without MangoHUD.

Captures verify scene, camera and basic image sanity; they are not an identical-input pixel replay. Private captures, shaders, saves and diagnostic logs are excluded from the public source and distribution.

## Distribution checks

The installer records the bundled AppImage SHA-256 in `release.json`; its `SHA256SUMS` verifies the packaged files.

A complete local import rebuilt the game from original 1.2.0 modules, merged romfs
and six DLC packs in paths containing spaces. Saves and configuration were
unchanged by installation. This import used the preceding package build
`b10ecfd700e18e8ca21c8f420161f9fb2f84a0466802ee6bab3325aca64ee459`;
the final AppImage then verified and ran that independently generated library.
The final packaging changes add importer validation and Deck defaults, without
changing the generated-library compatibility ID or translation interface.

The final managed launcher loaded the monastery, reached the scheduled 4,800-frame
capture, blocked a simultaneous launcher and exited normally with saves unchanged.
The capture was reviewed against the opt4 reference for scene/image sanity.
The first capture attempt failed because the test harness had not created its
output directory; this was corrected without repeating successful game compilation.

An actual interrupted reinstall started compilation, was cancelled, left no
children in its private application/compiler session and preserved the installed
manifest, saves and configuration. Nineteen installer/audit/process/file tests
passed, including replacement recovery, corrupt inputs and unrelated-process
preservation. A base-only layout with the newly generated library passed original
file validation and dynamic loading; a separate full base-only compilation was
not repeated because the module inputs were identical.

Actual AppDir contents and exported sources passed the independent artifact audit.
The desktop entry passed desktop-file-validate. The GitHub workflow is prepared
but has not run on GitHub: no repository or release was created or published.

The maintainer explicitly authorized promotion to opt5 after reviewing these
results. A full identical-input framebuffer comparison of opt4 and opt5 has not
been completed; promotion does not claim that this check passed. The application
icon was subsequently replaced with the image supplied by the maintainer.
This packaging change does not alter the native executable or rendering code.

The promoted private installation completed its normal launcher/cloud-sync path,
loaded the monastery, reached the scheduled capture and closed with code 0.
The cloud state was synced with zero pending uploads. These private cloud
components are not part of the public distribution.

Public branding rebuild: FE3H Deck Recomp. Only display/packaging names,
documentation and retained third-party notices changed. The private Deck
installation remains on its previously promoted artifact, independently.

The branded application loaded the isolated monastery save and closed normally;
concurrent launch was blocked and saves were unchanged. This run used packaging
hash dae3fe931d76d7c92aa360ca8ea51b3aff5285c724ce50b95ec2fd05b7e322b8.
The final archive adds the retained FSR notice and README credit without
rebuilding the native executable. The original game-library ID remains
49a7c4db42f1f30f, confirmed by loading the existing compiled game library.

Documentation refresh: repaired UTF-8 text, added two encoding regression checks
and simplified the release to installation binaries and separate source archives.
Notices are included in the installer; complete corresponding sources are available
beside it. Runtime and game formats are unchanged.
