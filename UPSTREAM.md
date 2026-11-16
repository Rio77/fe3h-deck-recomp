# Source provenance

Public reference: FE3H Recomp `v0.2.3`, commit `4e21e7bff4a2df979e57ca82639c72931c726423`, from https://git.onerag90.de/recomps/fe3h-recomp. The selected later changes were reviewed against revision `9f850992e7`. The project originated in Peiikai's LASRecomp; upstream copyright and license notices are retained.

This source export is selective. It is not an unmodified checkout of the upstream main branch. It retains the native fork's opt4 behavior and adds the accepted opt5 changes. No history of private development files is included.

Selected groups:

- CPU limiter and cache prewarm profile; limiter call site validated against the original FE3H 1.2.0 modules.
- GPU sequence waits; texture reference cleanup and its follow-up correction; normalized pipeline keys; atomic periodic Vulkan cache saves.
- Robust configuration and logging, audio-period pacing, movie resource cleanup, brief controller presses and active-controller selection. Existing menu combination and restart controls remain.
- Confirmed-save protection and UI-thread notices; layered data mods, index merging, DLC support and regional English settings.
- Native resource ownership: compilation queues, pipeline/program lifetimes, movie buffers, reverb buffers, thread signal stacks, shader compiler contexts, kernel/IPC objects and shared AM storage.
- Linux distribution, original icon, local importer verification and recoverable installation.

Excluded: upstream memory-block replacement, experimental Linux page tracking, giant compute shaders, FSR/temporal upscalers, portrait smoothing, vibration, amiibo and an official automatic updater. The extra fragment-compiler lighting optimization, packed-half experiment and descriptor-reuse experiment were discarded after measurements. Existing structured lighting remains enabled in the Deck profile and is not counted as a new opt5 gain.

The checked-in generator and its interface headers produce game libraries locally. They do not contain the recompiled game. Build identifiers and file sizes used by the importer are format/version metadata, not game content.
