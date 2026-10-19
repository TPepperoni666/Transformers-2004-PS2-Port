# Options

See [settings profiles](SETTINGS.md). Variables are read before or during startup and may be cached; restart after changing them. This release has no global debug opt-in gate. Unknown variables are ignored by individual readers; parsing/ranges vary by reader, so use the documented values. v0.8.3 supports settings.ini and --set key=value. Precedence is defaults < settings.ini < environment < --set. Launch preset values replace inherited environment values before startup; environment overrides are locked in the menu. Scripted tests ignore settings.ini unless PS2X_SETTINGS explicitly names a file.

| Category | Contract |
|---|---|
| User | Disc/control paths, audio, aspect, scaling, sharpening, field presentation and play pacing in the table below. |
| Debug | DUMP, CAPTURE, TRACE, WATCH, WAV, STATS, LOG, PERFSCOPE and crash-test knobs from the occurrence inventory. Off unless explicitly requested; output stays private. |
| Test | PS2X_SCRIPT and AUTOPRESS families, VU1AOT_HASH, fixed worker/heap/glyph settings for controlled gates. Reproduce the complete test environment on both sides. |
| Internal/experimental | Remaining implementation, fallback, FPU, VU, GS and compatibility switches are developer interfaces, not stable player settings. |

For controlled VU hash tests use PS2X_VU1AOT_HASH=1, PS2X_VU1_THREAD=0, PS2X_KERNEL_HEAP=0, PS2X_GLYPHS=off with HASH-only observation on both sides. Shipping worker counters are a separate gate. Audio defaults/ranges below refer to the launch interface; the occurrence list is not a default-value registry.


Set environment variables before launching; launch/play_env.txt supplies a conservative play preset. Its AOT flags start disabled; the play launchers enable static VU1 automatically when disc.json records a built static package. Values from the preset override inherited environment values; edit your local preset to customize. F7 shows controller inputs, F9 cycles sharpening, F11 shows speed, F12 writes a private bug mark/screenshot, F1 opens the PC settings menu. Never submit screenshot/log/card output.

| Variable | Values / purpose |
|---|---|
| PS2X_CONTROLS_INI | Private controls.ini path; default reads next to the ELF |
| PS2X_CD_IMAGE | Absolute path to your ISO, required for disc streaming |
| PS2X_GS_BACKEND | PGS (Vulkan build) or CPU backend; PGS recommended |
| PS2X_VU1_AOT | 0 without a locally generated package; 1 only with that package compiled |
| PS2X_VU1_THREAD | 3 enables the worker; 0 interprets on the game thread |
| PS2X_AUDIO / PS2X_AUDIO_VOLUME | 0/1 enable audio; volume 0..1 |
| PS2X_AUDIO_STRETCH | 1 keeps pitch when the game runs slowly; 0 disables |
| PS2X_FIELD60 | 1 presents interlaced fields at 60 Hz with PGS; 0 whole frames |
| PS2X_REALTIME | 1 caps guest speed for play; scripted tests use legacy pacing |
| PS2X_UPSCALE | fsr1, cas, off; F9 cycles |
| PS2X_SHARPNESS | 0..1; default 0.8 |
| PS2X_WIDESCREEN | 16:10, 16:9, 21:9; unset = 4:3; some 2D screens stretch |
| PS2X_PRESENT_ASPECT | square restores square-pixel display |
| PS2X_LOAD_SKIP | 4 skips some loading-screen draws without changing game logic |
| PS2X_QUIET_IO | 1 reduces per-read logging |
| PS2X_MPEG_AHEAD | 120 movie decode lead |
| PS2X_IOP_RPC_BUDGET | 50000000 shipping RPC cycle budget |
| PS2X_PGS_PIPECACHE | 1 caches Vulkan pipelines; 0 disables |
| PS2X_SCRIPT | Local .tfs route; controller ignored during scripted runs |
| PS2X_PERFSCOPE | 1 timing collection; 0 disables |

The play preset includes tuning and revert switches with comments. Developer-only capture, dump, trace, hash and historical compatibility knobs remain in source. The inventory below records occurrences, not promises of supported user interfaces or default values.

- `PS2X_AUDIO`: `PS2Recomp\ps2xRuntime\src\lib\ps2_host_audio.h:29`
- `PS2X_AUDIO_STATS`: `PS2Recomp\ps2xRuntime\src\lib\ps2_host_audio.h:138`
- `PS2X_AUDIO_STRETCH`: `PS2Recomp\ps2xRuntime\src\lib\ps2_audio_jitter.h:19`
- `PS2X_AUDIO_VOLUME`: `PS2Recomp\ps2xRuntime\src\lib\ps2_host_audio.h:17`
- `PS2X_AUDIO_WAV`: `PS2Recomp\ps2xRuntime\src\lib\ps2_host_audio.h:41`
- `PS2X_AUDIO_WAV_MAX_S`: `PS2Recomp\ps2xRuntime\src\lib\ps2_host_audio.h:43`
- `PS2X_AUTOPRESS`: `PS2Recomp\ps2xRuntime\src\lib\ps2_pad.cpp:35`
- `PS2X_AUTOPRESS_ANCHORS`: `PS2Recomp\ps2xIOP\src\emulator\imports\iop_cdvd.cpp:24`
- `PS2X_CALL_TRACE`: `PS2Recomp\ps2xRuntime\include\ps2_log.h:165`
- `PS2X_CD_IMAGE`: `PS2Recomp\ps2xRuntime\src\lib\ps2_runtime.cpp:2535`
- `PS2X_CINEMA_WATCH`: `PS2Recomp\ps2xRuntime\src\lib\ps2_runtime.cpp:2872`
- `PS2X_CONTROLS_INI`: `PS2Recomp\ps2xRuntime\src\lib\ps2_pad.cpp:32`
- `PS2X_CRASH_TEST`: `PS2Recomp\ps2xRuntime\src\lib\ps2_crash_reporter.cpp:512`
- `PS2X_DEBUG_UI`: `PS2Recomp\ps2xRuntime\src\lib\ps2_debug_panel.cpp:2187`
- `PS2X_DEFAULT_BOOT_ELF`: `PS2Recomp\ps2xRuntime\src\main.cpp:155`
- `PS2X_DRONE_WATCH`: `PS2Recomp\ps2xRuntime\src\lib\ps2_runtime.cpp:2871`
- `PS2X_DUMP_DIR`: `PS2Recomp\ps2xRuntime\src\lib\ps2_crash_reporter.cpp:438`
- `PS2X_DUMP_FRAMES`: `PS2Recomp\ps2xRuntime\src\lib\ps2_runtime.cpp:610`
- `PS2X_EETRIM_STATS`: `PS2Recomp\ps2xRuntime\src\lib\Kernel\EeScheduler.cpp:118`
- `PS2X_EE_ADDRWATCH`: `PS2Recomp\ps2xRuntime\src\lib\ps2_runtime.cpp:4311`
- `PS2X_EE_COLORWATCH`: `PS2Recomp\ps2xRuntime\src\lib\ps2_runtime.cpp:4361`
- `PS2X_EE_IDLESKIP`: `PS2Recomp\ps2xRuntime\src\lib\Kernel\EeScheduler.cpp:116`
- `PS2X_EE_PPE_FAST`: `PS2Recomp\ps2xRuntime\src\lib\Kernel\EeScheduler.cpp:124`
- `PS2X_EE_WORDWATCH`: `PS2Recomp\ps2xRuntime\include\ps2_runtime.h:248`
- `PS2X_EE_WORDWATCH_VS`: `PS2Recomp\ps2xRuntime\src\lib\ps2_runtime.cpp:4339`
- `PS2X_ENABLE_DEBUG_UI`: `PS2Recomp\ps2xRuntime\src\main.cpp:3`
- `PS2X_ENABLE_IOP_RPC_TRACE`: `PS2Recomp\ps2xRuntime\src\lib\Kernel\Syscalls\RPC.cpp:73`
- `PS2X_FIELD2_STOPGAP`: `PS2Recomp\ps2xRuntime\src\lib\ps2_runtime.cpp:1001`
- `PS2X_FIELD60`: `PS2Recomp\ps2xRuntime\src\lib\ps2_runtime.cpp:475`
- `PS2X_FIELD60_ADAPT_T`: `PS2Recomp\ps2xRuntime\src\lib\gs\gs_pgs_backend.cpp:45`
- `PS2X_FIELD60_DUMP`: `PS2Recomp\ps2xRuntime\src\lib\ps2_runtime.cpp:698`
- `PS2X_FIELD60_FILTER`: `PS2Recomp\ps2xRuntime\src\lib\gs\gs_pgs_backend.cpp:37`
- `PS2X_FIELD60_LOG`: `PS2Recomp\ps2xRuntime\src\lib\gs\gs_pgs_backend.cpp:47`
- `PS2X_FIELD60_NODROP`: `PS2Recomp\ps2xRuntime\src\lib\gs\gs_pgs_backend.cpp:52`
- `PS2X_FIELD60_PARITY`: `PS2Recomp\ps2xRuntime\src\lib\gs\gs_pgs_backend.cpp:44`
- `PS2X_FIELD60_PIN30`: `PS2Recomp\ps2xRuntime\src\lib\ps2_runtime.cpp:2426`
- `PS2X_FIELD60_PRECAP`: `PS2Recomp\ps2xRuntime\src\lib\gs\gs_pgs_backend.cpp:38`
- `PS2X_FIELD60_WEAVE_VS`: `PS2Recomp\ps2xRuntime\src\lib\gs\gs_pgs_backend.cpp:48`
- `PS2X_FPU_OPERAND_CLAMP`: `PS2Recomp\ps2xRuntime\include\ps2_runtime_macros.h:174`
- `PS2X_FRAMETIME_LOG`: `PS2Recomp\ps2xRuntime\src\lib\gs\gs_pgs_backend.cpp:1695`
- `PS2X_GIF_FIFO`: `PS2Recomp\ps2xRuntime\src\lib\ps2_memory.cpp:104`
- `PS2X_GIF_POOL`: `PS2Recomp\ps2xRuntime\src\lib\gs\ps2_gif_arbiter.cpp:12`
- `PS2X_GLYPHS`: `PS2Recomp\ps2xRuntime\src\lib\ps2_glyphs.h:38`
- `PS2X_GLYPHS_INI_IN_SCRIPT`: `PS2Recomp\ps2xRuntime\src\lib\ps2_pad.cpp:169`
- `PS2X_GLYPHS_LOG`: `PS2Recomp\ps2xRuntime\src\lib\ps2_remap_getter.h:21`
- `PS2X_GLYPHS_TEST_LAYOUT`: `PS2Recomp\ps2xRuntime\src\lib\ps2_remap_strings.h:101`
- `PS2X_GSCAPTURE`: `PS2Recomp\ps2xRuntime\include\runtime\gs\gs_raw_record.h:61`
- `PS2X_GSCAPTURE_SKIPFBP`: `PS2Recomp\ps2xRuntime\src\lib\gs\gs_cpu_backend.cpp:993`
- `PS2X_GSCAPTURE_SKIP_PRIMS`: `PS2Recomp\ps2xRuntime\src\lib\gs\gs_cpu_backend.cpp:1005`
- `PS2X_GSCAPTURE_VS`: `PS2Recomp\ps2xRuntime\include\runtime\gs\gs_raw_record.h:61`
- `PS2X_GS_ASYNC_FINISH`: `PS2Recomp\ps2xRuntime\include\runtime\gs\gs_threaded_backend.h:138`
- `PS2X_GS_BACKEND`: `PS2Recomp\ps2xRuntime\include\runtime\gs\gs_pgs_backend.h:2`
- `PS2X_GS_COLCLAMP_WRAP`: `PS2Recomp\ps2xRuntime\include\runtime\gs\gs_raw_record.h:44`
- `PS2X_GS_DBUFF_DISPLAY_LEGACY`: `PS2Recomp\ps2xRuntime\src\lib\Kernel\Stubs\GS.cpp:1199`
- `PS2X_GS_DEFDISPENV_LEGACY`: `PS2Recomp\ps2xRuntime\src\lib\Kernel\Stubs\GS.cpp:1273`
- `PS2X_GS_DISABLE_EARLY_DEPTH`: `PS2Recomp\ps2xRuntime\include\runtime\gs\gs_raw_record.h:44`
- `PS2X_GS_DITHER`: `PS2Recomp\ps2xRuntime\include\runtime\gs\gs_raw_record.h:44`
- `PS2X_GS_HAS_MXCSR`: `PS2Recomp\ps2xRuntime\src\lib\gs\gs_cpu_backend.cpp:26`
- `PS2X_GS_IMAGE_X8`: `PS2Recomp\ps2xRuntime\src\lib\Kernel\Stubs\GS.cpp:45`
- `PS2X_GS_PRIV_SYNC`: `PS2Recomp\ps2xRuntime\src\lib\ps2_memory.cpp:186`
- `PS2X_GS_RAWREC`: `PS2Recomp\ps2xRuntime\include\runtime\gs\gs_raw_record.h:58`
- `PS2X_GS_RAW_RECYCLE`: `PS2Recomp\ps2xRuntime\include\runtime\gs\gs_threaded_backend.h:243`
- `PS2X_GS_RECORD`: `PS2Recomp\ps2xRuntime\include\runtime\gs\gs_threaded_backend.h:36`
- `PS2X_GS_SHADOW`: `PS2Recomp\ps2xRuntime\include\runtime\gs\gs_backend.h:15`
- `PS2X_GS_SHADOW_DUMPS`: `PS2Recomp\ps2xRuntime\src\lib\gs\gs_pgs_backend.cpp:1643`
- `PS2X_GS_SHADOW_EVERY`: `PS2Recomp\ps2xRuntime\src\lib\gs\gs_pgs_backend.cpp:516`
- `PS2X_GS_THREAD`: `PS2Recomp\ps2xRuntime\include\runtime\gs\gs_threaded_backend.h:69`
- `PS2X_GS_THREAD_STATS`: `PS2Recomp\ps2xRuntime\include\runtime\gs\gs_threaded_backend.h:270`
- `PS2X_GS_ZCLAMP`: `PS2Recomp\ps2xRuntime\include\runtime\gs\gs_raw_record.h:44`
- `PS2X_GUEST_UNWIND`: `PS2Recomp\ps2xRuntime\src\lib\ps2_runtime.cpp:2887`
- `PS2X_HAS_FFMPEG`: `PS2Recomp\ps2xRuntime\src\lib\Kernel\Stubs\MPEG.cpp:8`
- `PS2X_HEAPSCOPE`: `PS2Recomp\ps2xRuntime\src\lib\ps2_runtime.cpp:131`
- `PS2X_HEAPSCOPE_VS`: `PS2Recomp\ps2xRuntime\src\lib\ps2_runtime.cpp:131`
- `PS2X_HIDDEN_WINDOW`: `PS2Recomp\ps2xRuntime\src\lib\ps2_runtime.cpp:1493`
- `PS2X_HOST_STALL_S`: `PS2Recomp\ps2xRuntime\src\lib\Kernel\TestScript.cpp:1523`
- `PS2X_HUB_MATERIAL_WATCH`: `PS2Recomp\ps2xRuntime\src\lib\ps2_runtime.cpp:3852`
- `PS2X_IOP_OUTSIDE_WAIT_CYCLES`: `PS2Recomp\ps2xIOP\src\emulator\iop_emulator.cpp:806`
- `PS2X_IOP_RPC_BUDGET`: `PS2Recomp\ps2xIOP\src\emulator\services\iop_rpc.cpp:408`
- `PS2X_IOP_RPC_SETTLE`: `PS2Recomp\ps2xIOP\src\emulator\services\iop_rpc.cpp:426`
- `PS2X_IOP_TRACE`: `PS2Recomp\ps2xIOP\src\emulator\iop_emulator.cpp:781`
- `PS2X_IOP_TRACE_LIBS`: `PS2Recomp\ps2xIOP\src\emulator\iop_emulator.cpp:374`
- `PS2X_IOP_TRACE_MAX`: `PS2Recomp\ps2xIOP\src\emulator\iop_emulator.cpp:390`
- `PS2X_ISOMAP`: `PS2Recomp\ps2xRuntime\src\lib\Kernel\TestScript.cpp:864`
- `PS2X_JALR_WATCH`: `PS2Recomp\ps2xRuntime\src\lib\ps2_runtime.cpp:2871`
- `PS2X_KERNEL_HEAP`: `PS2Recomp\ps2xRuntime\src\lib\ps2_runtime.cpp:23`
- `PS2X_LIBC_SPECIAL_FIX`: `PS2Recomp\ps2xRuntime\src\lib\Kernel\Stubs\LibC.cpp:55`
- `PS2X_LOAD_SKIP`: `PS2Recomp\ps2xRuntime\src\lib\ps2_runtime.cpp:1122`
- `PS2X_MC_ROOT`: `PS2Recomp\ps2xRuntime\src\lib\ps2_runtime.cpp:2526`
- `PS2X_MINIDUMP_FULL`: `PS2Recomp\ps2xRuntime\src\lib\ps2_crash_reporter.cpp:295`
- `PS2X_MPEG_AHEAD`: `PS2Recomp\ps2xRuntime\src\lib\Kernel\Stubs\MPEG.cpp:1460`
- `PS2X_MPEG_CB_DIAG`: `PS2Recomp\ps2xRuntime\src\lib\Kernel\Stubs\MPEG.cpp:1659`
- `PS2X_MPEG_CB_SNAPSHOT`: `PS2Recomp\ps2xRuntime\src\lib\Kernel\Stubs\MPEG.cpp:1657`
- `PS2X_MPEG_KEEP_CALLBACKS`: `PS2Recomp\ps2xRuntime\src\lib\Kernel\Stubs\MPEG.cpp:2168`
- `PS2X_MPEG_LOG_VS`: `PS2Recomp\ps2xRuntime\src\lib\Kernel\Stubs\MPEG.cpp:492`
- `PS2X_PADTEST`: `PS2Recomp\ps2xRuntime\src\lib\ps2_pad.cpp:782`
- `PS2X_PADTEST_SHOT_FRAMES`: `PS2Recomp\ps2xRuntime\src\lib\ps2_pad.cpp:7`
- `PS2X_PAD_ANALOG`: `PS2Recomp\ps2xRuntime\src\lib\Kernel\Stubs\Pad.cpp:236`
- `PS2X_PAD_HAS_GLFW`: `PS2Recomp\ps2xRuntime\src\lib\ps2_pad.cpp:39`
- `PS2X_PAD_LOG`: `PS2Recomp\ps2xRuntime\src\lib\ps2_pad.cpp:310`
- `PS2X_PAD_PRESSURE_ID`: `PS2Recomp\ps2xRuntime\src\lib\Kernel\Stubs\Pad.cpp:282`
- `PS2X_PAD_PRESSURE_LEGACY`: `PS2Recomp\ps2xRuntime\src\lib\Kernel\Stubs\Pad.cpp:297`
- `PS2X_PAL`: `PS2Recomp\ps2xRuntime\src\lib\Kernel\EeScheduler.cpp:71`
- `PS2X_PATH3_MASK_PACKET`: `PS2Recomp\ps2xRuntime\src\lib\ps2_memory.cpp:2708`
- `PS2X_PCRTC_FFMD`: `PS2Recomp\ps2xRuntime\src\lib\gs\gs_cpu_backend.cpp:2343`
- `PS2X_PERFSCOPE`: `PS2Recomp\ps2xRuntime\src\lib\ps2_perfscope.cpp:45`
- `PS2X_PERFSCOPE_CSV`: `PS2Recomp\ps2xRuntime\src\lib\ps2_perfscope.cpp:759`
- `PS2X_PERFSCOPE_SEC`: `PS2Recomp\ps2xRuntime\src\lib\ps2_perfscope.cpp:757`
- `PS2X_PGS_FRAME_CONTEXTS`: `PS2Recomp\ps2xRuntime\src\lib\gs\gs_pgs_backend.cpp:1428`
- `PS2X_PGS_PIPECACHE`: `PS2Recomp\ps2xRuntime\src\lib\gs\gs_pgs_backend.cpp:105`
- `PS2X_PGS_PIPECACHE_DIR`: `PS2Recomp\ps2xRuntime\src\lib\gs\gs_pgs_backend.cpp:106`
- `PS2X_PGS_PIPECACHE_MAX_MB`: `PS2Recomp\ps2xRuntime\src\lib\gs\gs_pgs_backend.cpp:109`
- `PS2X_PGS_PIPECACHE_SAVE_SEC`: `PS2Recomp\ps2xRuntime\src\lib\gs\gs_pgs_backend.cpp:107`
- `PS2X_PGS_SCANMSK_HACK`: `PS2Recomp\ps2xRuntime\src\lib\gs\gs_pgs_backend.cpp:1434`
- `PS2X_PGS_WARMUP_MS`: `PS2Recomp\ps2xRuntime\src\lib\gs\gs_pgs_backend.cpp:101`
- `PS2X_PKTPTR_WATCH`: `PS2Recomp\ps2xRuntime\src\lib\ps2_runtime.cpp:3970`
- `PS2X_PLAYTEST_AUTOMARK_VS`: `PS2Recomp\ps2xRuntime\src\lib\ps2_playtest.cpp:232`
- `PS2X_PLAYTEST_DIR`: `PS2Recomp\ps2xRuntime\src\lib\ps2_crash_reporter.cpp:438`
- `PS2X_PRESENT_ALLFRAMES`: `PS2Recomp\ps2xRuntime\src\lib\gs\gs_pgs_backend.cpp:757`
- `PS2X_PRESENT_ASPECT`: `PS2Recomp\ps2xRuntime\src\lib\ps2_present_upscale.cpp:488`
- `PS2X_PRESENT_LATCH`: `PS2Recomp\ps2xRuntime\src\lib\ps2_runtime.cpp:472`
- `PS2X_PROFILE`: `PS2Recomp\ps2xRuntime\src\lib\ps2_profiler.cpp:1`
- `PS2X_PROFILE_PERIOD`: `PS2Recomp\ps2xRuntime\src\lib\ps2_profiler.cpp:215`
- `PS2X_QUIET_IO`: `PS2Recomp\ps2xIOP\src\emulator\iop_emulator.cpp:367`
- `PS2X_REALTIME`: `PS2Recomp\ps2xRuntime\src\lib\ps2_perfscope.h:46`
- `PS2X_REALTIME_CATCHUP_MS`: `PS2Recomp\ps2xRuntime\src\lib\Kernel\EeScheduler.cpp:135`
- `PS2X_REALTIME_HZ`: `PS2Recomp\ps2xRuntime\src\lib\Kernel\EeScheduler.cpp:165`
- `PS2X_REALTIME_STATS`: `PS2Recomp\ps2xRuntime\src\lib\Kernel\EeScheduler.cpp:140`
- `PS2X_SCRIPT`: `PS2Recomp\ps2xIOP\src\emulator\imports\iop_cdvd.cpp:28`
- `PS2X_SCRIPT_PARSE_ONLY`: `PS2Recomp\ps2xRuntime\src\lib\Kernel\TestScript.cpp:1509`
- `PS2X_SCRIPT_RESULT`: `PS2Recomp\ps2xRuntime\src\lib\Kernel\TestScript.cpp:1527`
- `PS2X_SD_TRACE`: `PS2Recomp\ps2xIOP\src\emulator\iop_emulator.cpp:399`
- `PS2X_SHARPNESS`: `PS2Recomp\ps2xRuntime\src\lib\ps2_present_upscale.cpp:1`
- `PS2X_SIFDMA_DUMP`: `PS2Recomp\ps2xRuntime\src\lib\Kernel\Stubs\SIF.cpp:771`
- `PS2X_SIF_LANDING_FIFO`: `PS2Recomp\ps2xIOP\src\emulator\iop_emulator.cpp:65`
- `PS2X_SIRENS_RPC_TRACE`: `PS2Recomp\ps2xIOP\src\emulator\services\iop_rpc.cpp:389`
- `PS2X_SPRINT_LOG`: `PS2Recomp\ps2xRuntime\src\lib\ps2_runtime.cpp:45`
- `PS2X_SPRWATCH`: `PS2Recomp\ps2xRuntime\src\lib\ps2_runtime.cpp:4242`
- `PS2X_SPU2_TEST_KEEP_TRACE`: `PS2Recomp\ps2xIOP\tests\iop_spu2_tests.cpp:131`
- `PS2X_SPU2_TRACE`: `PS2Recomp\ps2xIOP\src\emulator\core\iop_spu2.cpp:110`
- `PS2X_SPU2_TRACE_MAX_MB`: `PS2Recomp\ps2xIOP\src\emulator\core\iop_spu2.cpp:83`
- `PS2X_SPU2_TRACE_READS`: `PS2Recomp\ps2xIOP\src\emulator\core\iop_spu2.cpp:113`
- `PS2X_SPU2_WAV`: `PS2Recomp\ps2xIOP\src\emulator\core\iop_spu2_mixer.cpp:187`
- `PS2X_STALL_VS`: `PS2Recomp\ps2xRuntime\src\lib\ps2_runtime.cpp:4674`
- `PS2X_STOP_AT_VS`: `PS2Recomp\ps2xRuntime\src\lib\ps2_runtime.cpp:4674`
- `PS2X_STOREIMAGE_LAG`: `PS2Recomp\ps2xRuntime\include\runtime\gs\gs_backend.h:21`
- `PS2X_STRICT_RETURN_DIAGNOSTICS`: `PS2Recomp\ps2xRecomp\src\lib\control_flow_emitter.cpp:259`
- `PS2X_TEX_DEBUG_DUMPS`: `PS2Recomp\ps2xRuntime\include\runtime\gs\gs_raw_record.h:61`
- `PS2X_TF_FRAMELOG`: `PS2Recomp\ps2xRuntime\src\lib\ps2_runtime.cpp:1571`
- `PS2X_TF_NOFRAMEWAIT`: `PS2Recomp\ps2xRuntime\src\lib\ps2_runtime.cpp:1553`
- `PS2X_UPSCALE`: `PS2Recomp\ps2xRuntime\src\lib\ps2_present_upscale.cpp:1`
- `PS2X_UPSCALE_BENCH`: `PS2Recomp\ps2xRuntime\src\lib\ps2_present_upscale.cpp:43`
- `PS2X_UPSCALE_CAPTURE`: `PS2Recomp\ps2xRuntime\src\lib\ps2_present_upscale.cpp:257`
- `PS2X_UPSCALE_GL_TIMER`: `PS2Recomp\ps2xRuntime\src\lib\ps2_present_upscale.cpp:43`
- `PS2X_UPSCALE_HOTKEY`: `PS2Recomp\ps2xRuntime\src\lib\ps2_present_upscale.cpp:13`
- `PS2X_UPSCALE_WINDOW`: `PS2Recomp\ps2xRuntime\src\lib\ps2_present_upscale.cpp:329`
- `PS2X_VERSION`: `PS2Recomp\ps2xRuntime\src\lib\ps2_runtime.cpp:1455`
- `PS2X_VIF1_CHAIN_SUSPEND`: `PS2Recomp\ps2xRuntime\src\lib\ps2_memory.cpp:49`
- `PS2X_VIF1_KICKLOG`: `PS2Recomp\ps2xRuntime\src\lib\ps2_memory.cpp:88`
- `PS2X_VIF1_SOURCE_DUMP`: `PS2Recomp\ps2xRuntime\src\lib\ps2_memory.cpp:1714`
- `PS2X_VIF_COLORWATCH`: `PS2Recomp\ps2xRuntime\src\lib\ps2_vif1_interpreter.cpp:1255`
- `PS2X_VIF_UNPACK_CENSUS`: `PS2Recomp\ps2xRuntime\src\lib\ps2_vif1_interpreter.cpp:177`
- `PS2X_VIF_UNPACK_HW`: `PS2Recomp\ps2xRuntime\src\lib\ps2_vif1_interpreter.cpp:27`
- `PS2X_VRAM_DUMP_VS`: `PS2Recomp\ps2xRuntime\src\lib\ps2_runtime.cpp:620`
- `PS2X_VU0_MICRO_BUDGET`: `PS2Recomp\ps2xRuntime\src\lib\ps2_runtime.cpp:3203`
- `PS2X_VU1AOT_CAPTURE`: `PS2Recomp\ps2xRuntime\src\lib\vu\ps2_vu1_aot_bridge.cpp:62`
- `PS2X_VU1AOT_CAPTURE_MAX`: `PS2Recomp\ps2xRuntime\src\lib\vu\ps2_vu1_aot_bridge.cpp:93`
- `PS2X_VU1AOT_CAPTURE_PER`: `PS2Recomp\ps2xRuntime\src\lib\vu\ps2_vu1_aot_bridge.cpp:93`
- `PS2X_VU1AOT_HASH`: `PS2Recomp\ps2xRuntime\src\lib\vu\ps2_vu1_aot_bridge.cpp:135`
- `PS2X_VU1AOT_MIDPROBE`: `PS2Recomp\ps2xRuntime\src\lib\vu\ps2_vu1_aot_bridge.cpp:169`
- `PS2X_VU1AOT_MIDPROBE_MAX`: `PS2Recomp\ps2xRuntime\src\lib\vu\ps2_vu1_aot_bridge.cpp:173`
- `PS2X_VU1AOT_MIDPROBE_PER`: `PS2Recomp\ps2xRuntime\src\lib\vu\ps2_vu1_aot_bridge.cpp:172`
- `PS2X_VU1CUT_DUMP`: `PS2Recomp\ps2xRuntime\src\lib\ps2_runtime.cpp:1070`
- `PS2X_VU1DUMP`: `PS2Recomp\ps2xRuntime\src\lib\ps2_runtime.cpp:931`
- `PS2X_VU1DUMP_SKIP`: `PS2Recomp\ps2xRuntime\src\lib\ps2_runtime.cpp:951`
- `PS2X_VU1DUMP_VS`: `PS2Recomp\ps2xRuntime\src\lib\ps2_runtime.cpp:949`
- `PS2X_VU1WATCH`: `PS2Recomp\ps2xRuntime\src\lib\ps2_runtime.cpp:1038`
- `PS2X_VU1WATCH_WORDS`: `PS2Recomp\ps2xRuntime\src\lib\ps2_vif1_interpreter.cpp:1008`
- `PS2X_VU1_AOT`: `PS2Recomp\ps2xRuntime\include\runtime\ps2_vu1.h:90`
- `PS2X_VU1_AOT_MASK`: `PS2Recomp\ps2xRuntime\src\lib\vu\ps2_vu1_aot_bridge.cpp:307`
- `PS2X_VU1_BUDGET`: `PS2Recomp\ps2xRuntime\src\lib\ps2_runtime.cpp:3976`
- `PS2X_VU1_CKPT_DRAIN_MIN`: `PS2Recomp\ps2xRuntime\src\lib\Kernel\EeScheduler.cpp:122`
- `PS2X_VU1_COALESCE`: `PS2Recomp\ps2xRuntime\src\lib\ps2_runtime.cpp:1122`
- `PS2X_VU1_COLORWATCH`: `PS2Recomp\ps2xRuntime\src\lib\vu\ps2_vu1_core.cpp:738`
- `PS2X_VU1_CSR_ORACLE`: `PS2Recomp\ps2xRuntime\include\runtime\ps2_vif1_instant.h:177`
- `PS2X_VU1_CSR_TRACE`: `PS2Recomp\ps2xRuntime\include\runtime\ps2_gs_timeline.h:11`
- `PS2X_VU1_DEAD_STAT`: `PS2Recomp\ps2xRuntime\src\lib\ps2_runtime.cpp:2129`
- `PS2X_VU1_DRAIN_FAST`: `PS2Recomp\ps2xRuntime\include\runtime\ps2_vif1_instant.h:138`
- `PS2X_VU1_FAST`: `PS2Recomp\ps2xRuntime\include\runtime\ps2_vu1.h:291`
- `PS2X_VU1_FENCE_WAKE`: `PS2Recomp\ps2xRuntime\include\runtime\ps2_vif1_instant.h:213`
- `PS2X_VU1_GUARD`: `PS2Recomp\ps2xRuntime\src\lib\vu\ps2_vu1_core.cpp:52`
- `PS2X_VU1_IDLESKIP_PENDING`: `PS2Recomp\ps2xRuntime\src\lib\Kernel\EeScheduler.cpp:121`
- `PS2X_VU1_QSCAN`: `PS2Recomp\ps2xRuntime\include\runtime\ps2_vif1_instant.h:184`
- `PS2X_VU1_RUN_TO_END`: `PS2Recomp\ps2xRuntime\include\runtime\ps2_vu1.h:73`
- `PS2X_VU1_THREAD`: `PS2Recomp\ps2xRuntime\include\runtime\ps2_vif1_instant.h:41`
- `PS2X_VU1_WCHUNK`: `PS2Recomp\ps2xRuntime\include\runtime\ps2_vif1_instant.h:70`
- `PS2X_VU1_XGKICK_IMMEDIATE`: `PS2Recomp\ps2xRuntime\src\lib\vu\ps2_vu1_core.cpp:64`
- `PS2X_VU1_XGKICK_ZERO`: `PS2Recomp\ps2xRuntime\src\lib\vu\ps2_vu1_core.cpp:70`
- `PS2X_VU_MEMCPY_WRAP`: `PS2Recomp\ps2xRuntime\src\lib\ps2_runtime.cpp:2395`
- `PS2X_WATCH`: `PS2Recomp\ps2xRuntime\src\lib\Kernel\EeScheduler.cpp:294`
- `PS2X_WCLEAN_FIX`: `PS2Recomp\ps2xRuntime\src\lib\ps2_runtime.cpp:1535`
- `PS2X_WIDESCREEN`: `PS2Recomp\ps2xRuntime\src\lib\ps2_present_upscale.cpp:489`
- `PS2X_WIDESCREEN_FORCE`: `PS2Recomp\ps2xRuntime\src\lib\ps2_runtime.cpp:1999`
- `PS2X_WIDESCREEN_HUD`: `PS2Recomp\ps2xRuntime\src\lib\ps2_runtime.cpp:1786`
- `PS2X_WIDESCREEN_MODE`: `PS2Recomp\ps2xRuntime\src\lib\ps2_runtime.cpp:1689`
- `PS2X_WIDESCREEN_PILLARBOX`: `PS2Recomp\ps2xRuntime\src\lib\ps2_runtime.cpp:2061`
- `PS2X_WITH_PGS`: `PS2Recomp\ps2xRuntime\src\lib\gs\gs_pgs_backend.cpp:59`
- `PS2X_WITH_VU1_AOT`: `PS2Recomp\ps2xRuntime\include\runtime\ps2_vu1.h:92`
- `PS2X_WS2_FMVLOG`: `PS2Recomp\ps2xRuntime\src\lib\ps2_runtime.cpp:1656`
- `PS2X_WS2_FMVRULE`: `PS2Recomp\ps2xRuntime\src\lib\ps2_runtime.cpp:1654`
- `PS2X_WS_GIFLOG`: `PS2Recomp\ps2xRuntime\src\lib\ps2_memory.cpp:2777`
- `PS2X_WS_HUDLOG`: `PS2Recomp\ps2xRuntime\src\lib\ps2_runtime.cpp:1870`
- `PS2X_WS_LOG`: `PS2Recomp\ps2xRuntime\src\lib\ps2_runtime.cpp:1703`

Controller defaults: controls.ini [sticks] sensitivity_left=1.33 and sensitivity_right=1.0; range_left/right=circle (square for DS2 shaping), host deadzone 0.05, outer range 0.95, anti_deadzone_left=0.62 and anti_deadzone_right=0.20. Set anti-deadzone to 0 to disable it. Earlier sprint engagement is a tradeoff; scripted input bypasses physical-stick shaping. Keep controls.ini beside your private ELF or select it with PS2X_CONTROLS_INI.

v0.7.3 adds `PS2X_PRESENT_RATE=30|60` (simulation remains 30 Hz; 60 selects field presentation), `PS2X_POWER_OPTOUT=1` (Windows background-throttling opt-out), experimental `PS2X_VU1_AOT_STATIC=1` (requires your generated static package) and `PS2X_VU1NEW=1` (default on; requires `vu1known.bin`). `PS2X_VU1NEW_TABLE` selects an explicit private table. Core placement remains optional; it has no established handheld gain here. Old source-line entries above are discovery hints and may shift between releases.
