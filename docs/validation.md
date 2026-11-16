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
