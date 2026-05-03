#include "BetaConfig.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>

BetaConfig g_BetaConfig;

static int ReadIniInt(const char* path, const char* key, int defaultVal)
{
	char buf[64];
	DWORD n = GetPrivateProfileStringA("beta", key, "", buf, sizeof(buf), path);
	if (n == 0) return defaultVal;
	// Trim whitespace
	char* p = buf;
	while (*p == ' ' || *p == '\t') p++;
	if (*p == '\0') return defaultVal;
	return atoi(p);
}

void BetaConfig_Load()
{
	// Build path: <exe_dir>\beta.ini
	char exePath[MAX_PATH] = {};
	GetModuleFileNameA(GetModuleHandle(nullptr), exePath, MAX_PATH);

	// Strip filename to get directory
	char* lastSlash = strrchr(exePath, '\\');
	if (lastSlash) *(lastSlash + 1) = '\0';

	std::string iniPath = std::string(exePath) + "beta.ini";

	// Check if file exists; if not, keep defaults (all enabled)
	DWORD attr = GetFileAttributesA(iniPath.c_str());
	if (attr == INVALID_FILE_ATTRIBUTES) {
		return;
	}

	const char* path = iniPath.c_str();

	// Kernel
	g_BetaConfig.apc_try_lock             = ReadIniInt(path, "apc_try_lock", 1);
	g_BetaConfig.satisfy_wait_apc_flags   = ReadIniInt(path, "satisfy_wait_apc_flags", 1);
	g_BetaConfig.wait_list_toctou_recheck = ReadIniInt(path, "wait_list_toctou_recheck", 1);
	g_BetaConfig.timer_try_lock           = ReadIniInt(path, "timer_try_lock", 1);

	// D3D
	g_BetaConfig.rt_texture_alias         = ReadIniInt(path, "rt_texture_alias", 1);
	g_BetaConfig.default_transparent_tex  = ReadIniInt(path, "default_transparent_tex", 1);
	g_BetaConfig.rt_resolve_self_sampling = ReadIniInt(path, "rt_resolve_self_sampling", 1);
	g_BetaConfig.vsh_constant_delta_upload = ReadIniInt(path, "vsh_constant_delta_upload", 1);
	g_BetaConfig.staging_surface_pool     = ReadIniInt(path, "staging_surface_pool", 1);
	g_BetaConfig.texture_stage_cache      = ReadIniInt(path, "texture_stage_cache", 1);

	// Timer
	g_BetaConfig.precise_sleep_timer      = ReadIniInt(path, "precise_sleep_timer", 1);
	g_BetaConfig.periodic_irq10           = ReadIniInt(path, "periodic_irq10", 1);
	g_BetaConfig.llong_min_timeout_fix    = ReadIniInt(path, "llong_min_timeout_fix", 1);
	g_BetaConfig.get_now_lock             = ReadIniInt(path, "get_now_lock", 1);
	g_BetaConfig.atomic_interrupts        = ReadIniInt(path, "atomic_interrupts", 1);
	g_BetaConfig.system_events_other_affinity = ReadIniInt(path, "system_events_other_affinity", 0);
	g_BetaConfig.system_events_normal_priority = ReadIniInt(path, "system_events_normal_priority", 0);

	// KiTimerExpiration safety
	g_BetaConfig.timer_exp_pointer_guard  = ReadIniInt(path, "timer_exp_pointer_guard", 1);
	g_BetaConfig.timer_exp_max_expired    = ReadIniInt(path, "timer_exp_max_expired", 0);
	g_BetaConfig.timer_exp_max_ticks      = ReadIniInt(path, "timer_exp_max_ticks", 0);

	// Crazy Taxi game-specific
	g_BetaConfig.ct_skip_movies           = ReadIniInt(path, "ct_skip_movies", 1);
	g_BetaConfig.ct_cri_force_complete    = ReadIniInt(path, "ct_cri_force_complete", 1);

	// Chihiro MediaBoard
	g_BetaConfig.mb_board_type            = ReadIniInt(path, "mb_board_type", 0);
	g_BetaConfig.mb_dimm_size             = ReadIniInt(path, "mb_dimm_size", 0);
}
