#pragma once

#include <cstdint>

// BetaConfig: runtime feature toggles loaded from beta.ini next to the executable.
// All features default to ENABLED (1) to preserve current behavior.
// Set a key to 0 in beta.ini to disable the corresponding feature.

struct BetaConfig {
	// Diagnostics
	int full_trace              = 0; // Timestamped all-module kernel/HLE + perf/render trace files
	int scheduler_io_trace      = 0; // Atomic low-volume wait, suspend/resume, thread, and file-I/O trace
	std::uint32_t scheduler_io_probe_address = 0; // Optional guest u32 address appended to wait records
	int scheduler_io_wait_trace = 1;
	int scheduler_io_thread_trace = 1;
	int scheduler_io_event_trace = 1;
	int scheduler_io_file_trace = 1;
	std::uint32_t scheduler_io_cri_watch_base = 0; // Optional CRI wxCi handle-pool base
	std::uint32_t scheduler_io_cri_watch_stride = 340;
	std::uint32_t scheduler_io_cri_watch_count = 0;

	// Kernel features
	int apc_try_lock            = 1; // KiExecuteApc/KiInsertQueueApc: try_lock instead of blocking lock
	int satisfy_wait_apc_flags  = 1; // SatisfyWait: use APC pending flags instead of mutex
	int wait_list_toctou_recheck = 1; // KeWaitFor*: re-check signal state after InsertTailList
	int notification_event_wait_recheck = 0; // Narrow single-wait recheck for notification events only
	int cooperative_self_suspend = 0; // Avoid Wine SuspendThread(self)/ResumeThread races
	int timer_try_lock          = 1; // KiClockIsr: try_lock on timer mutex (skip tick if contended)

	// D3D / rendering features
	int rt_texture_alias        = 1; // RT-to-texture aliasing for unified memory emulation
	int default_transparent_tex = 1; // 1x1 transparent default for unbound texture stages
	int rt_resolve_self_sampling = 1; // StretchRect resolve when RT is sampled as texture
	int vsh_constant_delta_upload = 1; // Upload only changed vertex shader constants per draw
	int staging_surface_pool    = 1; // Reuse SYSTEMMEM staging surfaces across uploads
	int texture_stage_cache     = 1; // Per-stage host texture binding cache
	int ff_nv2a_blend_matrices  = 1; // Source multi-matrix fixed-function skinning from NV2A constants
	int ff_hlsl_vertex_shader   = 1; // Emulate Xbox fixed function through the Cxbx HLSL vertex shader
	int ff_force_full_rebuild   = 0; // Rebuild fixed-function state for every draw (diagnostic)
	int or2_draw_probe          = 0; // OutRun-only live indexed-draw filter diagnostics

	// Timer / frame pacing
	int precise_sleep_timer     = 1; // Use high-resolution waitable timer in SleepPrecise
	int periodic_irq10          = 1; // Periodic IRQ10 assertion for Chihiro media board
	int llong_min_timeout_fix   = 1; // Treat INT64_MIN as an infinite alertable wait (CRI library fix)
	int llong_min_timeout_sleep_ms = 0; // Legacy key retained for beta.ini compatibility
	int get_now_lock            = 1; // Spinlock in get_now() to prevent concurrent time double-count
	int atomic_interrupts       = 1; // Use std::atomic for HalSystemInterrupt flags (thread safety)
	int system_events_other_affinity = 0; // Move system_events off Xbox core to Other cores
	int system_events_normal_priority = 0; // Lower system_events from ABOVE_NORMAL to NORMAL
	int wmmt_device_poll_yield_ms = 0; // WMMT gamepad-change busy-loop yield; desktop default stays unchanged
	int wmmt_gamepad_init_bypass = 0; // Complete WMMT's empty gamepad enumeration loop after normal USB init

	// KiTimerExpiration safety guards
	int timer_exp_pointer_guard  = 1; // Validate timer list pointers before dereferencing
	int timer_exp_max_expired    = 0; // Max expired timers per DPC call (0=unlimited)
	int timer_exp_max_ticks      = 0; // Max tick range per DPC call (0=use caller range)

	// Crazy Taxi game-specific patches
	int ct_gamepad_init_bypass = 0; // Complete the empty gamepad-change loop used before JVS initialization
	int ct_skip_movies          = 0; // 1 = skip Sofdec FMVs (return "done"), 0 = allow movie playback
	int ct_cri_drive_movie_server = 0; // 1 = service the title's CRI 2/4/5 callbacks while a movie starts or plays
	int ct_cri_force_complete   = 0; // 1 = force CRI async I/O instant-complete, 0 = let CRI spin naturally
	std::uint32_t ct_cri_wait_timeout_ms = 3000; // Recover only a GM=7 CRI completion wait that remains stuck this long
	int ct_infinite_timer       = 0; // 1 = keep the in-game timer at ct_timer_lock_value
	std::uint32_t ct_timer_lock_value = 3000; // Crazy Taxi timer value written while the lock is enabled
	int ct_debug_course         = -1; // -1 = normal selection, 0-2 = hold a course for renderer diagnostics
	int ct_debug_movie_dump     = 0; // 1 = dump selected post-conversion BGRA movie frames
	int ct_debug_mip_lod_bias   = 99; // 99 = native sampler state, otherwise force an integer LOD bias
	int ct_debug_disable_lighting = 0; // 1 = bypass fixed-function lighting for renderer diagnostics
	int ct_debug_point_light_range_percent = 100; // 100 = native Crazy Taxi point-light range
	int ct_debug_point_light_attenuation_percent = 100; // 100 = native Crazy Taxi point-light attenuation
	int ct_debug_fog_mode       = 0; // 1 = force positive linear fog for Crazy Taxi renderer diagnostics
	int ct_debug_skip_noise_textures = 0; // 1 = skip the 64x128 point-filtered DXT1 facade-noise materials
	int ct_debug_stencil_volume_mode = 0; // 0 = native, 1 = swap cull winding, 2 = swap INCR/DECR, 3 = skip volumes, 4 = skip final darkening quad
	int ct_headlights            = 1; // 1 = gameplay headlight cone, 2 = diagnostic point light, 0 = disabled

	// Chihiro MediaBoard hardware identity
	int mb_board_type           = 0; // 0 = Type-1 (0x0000), 1 = Type-3 (0x0001)
	int mb_dimm_size            = 0; // 0 = 1GB (0x03), 1 = 512MB (0x02)
	int mb_trace                = 0; // 1 = write the narrowly scoped MediaBoard register/command trace
};

// Global instance — loaded once at startup, read-only thereafter.
extern BetaConfig g_BetaConfig;

// Load beta.ini from the directory containing the running executable.
// Call this early in initialization (after szFilePath_CxbxReloaded_Exe is set).
void BetaConfig_Load();

// Writes one complete scheduler/I/O diagnostic record to
// <exe_dir>\cxbxr-scheduler-io-trace.log. The writer is serialized so records
// from different guest threads cannot be interleaved under Wine/Box86.
void BetaTrace_Record(const char* category, const char* format, ...);

// Safely samples scheduler_io_probe_address from the directly mapped guest
// address space. Returns false when no probe is configured or the page cannot
// be read.
bool BetaTrace_ReadProbe(std::uint32_t* value);

// Records only changes to active entries in a CRI wxCi file-handle pool. This
// makes it possible to follow queued -> reading -> complete transitions without
// enabling the extremely verbose scheduler wait trace.
void BetaTrace_WatchCriFileStates(const char* reason);
