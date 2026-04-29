#pragma once

// BetaConfig: runtime feature toggles loaded from beta.ini next to the executable.
// All features default to ENABLED (1) to preserve current behavior.
// Set a key to 0 in beta.ini to disable the corresponding feature.

struct BetaConfig {
	// Kernel features
	int apc_try_lock            = 1; // KiExecuteApc/KiInsertQueueApc: try_lock instead of blocking lock
	int satisfy_wait_apc_flags  = 1; // SatisfyWait: use APC pending flags instead of mutex
	int wait_list_toctou_recheck = 1; // KeWaitFor*: re-check signal state after InsertTailList
	int timer_try_lock          = 1; // KiClockIsr: try_lock on timer mutex (skip tick if contended)

	// D3D / rendering features
	int rt_texture_alias        = 1; // RT-to-texture aliasing for unified memory emulation
	int default_transparent_tex = 1; // 1x1 transparent default for unbound texture stages
	int rt_resolve_self_sampling = 1; // StretchRect resolve when RT is sampled as texture
	int vsh_constant_delta_upload = 1; // Upload only changed vertex shader constants per draw
	int staging_surface_pool    = 1; // Reuse SYSTEMMEM staging surfaces across uploads
	int texture_stage_cache     = 1; // Per-stage host texture binding cache

	// Timer / frame pacing
	int precise_sleep_timer     = 1; // Use high-resolution waitable timer in SleepPrecise
	int periodic_irq10          = 1; // Periodic IRQ10 assertion for Chihiro media board
	int llong_min_timeout_fix   = 1; // Treat INT64_MIN timeout as immediate (CRI library fix)

	// KiTimerExpiration safety guards
	int timer_exp_pointer_guard  = 1; // Validate timer list pointers before dereferencing
	int timer_exp_max_expired    = 0; // Max expired timers per DPC call (0=unlimited)
	int timer_exp_max_ticks      = 0; // Max tick range per DPC call (0=use caller range)

	// Crazy Taxi game-specific patches
	int ct_skip_movies          = 1; // 1 = skip Sofdec FMVs (return "done"), 0 = allow movie playback
	int ct_cri_force_complete   = 1; // 1 = force CRI async I/O instant-complete, 0 = let CRI spin naturally

	// Chihiro MediaBoard hardware identity
	int mb_board_type           = 0; // 0 = Type-1 (0x0000), 1 = Type-3 (0x0001)
	int mb_dimm_size            = 0; // 0 = 1GB (0x03), 1 = 512MB (0x02)
};

// Global instance — loaded once at startup, read-only thereafter.
extern BetaConfig g_BetaConfig;

// Load beta.ini from the directory containing the running executable.
// Call this early in initialization (after szFilePath_CxbxReloaded_Exe is set).
void BetaConfig_Load();
