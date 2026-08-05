#include "BetaConfig.h"
#include "Logging.h"
#include "PerfTrace.h"
#include "RenderTrace.h"
#include <windows.h>
#include <cstdarg>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

BetaConfig g_BetaConfig;

namespace
{
	std::mutex g_BetaTraceMutex;
	std::mutex g_BetaCriWatchMutex;
	FILE* g_BetaTraceFile = nullptr;
	uint64_t g_BetaTraceSequence = 0;
	LARGE_INTEGER g_BetaTraceFrequency = {};
	LARGE_INTEGER g_BetaTraceStart = {};
	std::uint64_t g_BetaCriWatchHashes[64] = {};
	bool g_BetaCriWatchSeen[64] = {};

	bool BetaTrace_CopyGuest(
		void* destination,
		std::uintptr_t source,
		std::size_t size)
	{
		__try {
			memcpy(
				destination,
				reinterpret_cast<const void*>(source),
				size);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	void BetaTrace_OpenLocked()
	{
		if (g_BetaTraceFile != nullptr) {
			return;
		}

		char exePath[MAX_PATH] = {};
		GetModuleFileNameA(GetModuleHandle(nullptr), exePath, MAX_PATH);
		char* lastSlash = strrchr(exePath, '\\');
		if (lastSlash != nullptr) {
			*(lastSlash + 1) = '\0';
		}

		const std::string tracePath =
			std::string(exePath) + "cxbxr-scheduler-io-trace.log";
		if (fopen_s(&g_BetaTraceFile, tracePath.c_str(), "wb") != 0) {
			g_BetaTraceFile = nullptr;
			return;
		}

		QueryPerformanceFrequency(&g_BetaTraceFrequency);
		QueryPerformanceCounter(&g_BetaTraceStart);
		fprintf(
			g_BetaTraceFile,
			"# CXBXR atomic scheduler/I/O trace v1 pid=%lu\n",
			GetCurrentProcessId());
		fflush(g_BetaTraceFile);
	}
}

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

static std::uint32_t ReadIniUInt32(
	const char* path,
	const char* key,
	std::uint32_t defaultVal)
{
	char buf[64];
	DWORD n = GetPrivateProfileStringA("beta", key, "", buf, sizeof(buf), path);
	if (n == 0) return defaultVal;
	char* p = buf;
	while (*p == ' ' || *p == '\t') p++;
	if (*p == '\0') return defaultVal;
	char* end = nullptr;
	const unsigned long value = strtoul(p, &end, 0);
	return end == p ? defaultVal : static_cast<std::uint32_t>(value);
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

	// Diagnostics
	g_BetaConfig.full_trace = ReadIniInt(path, "full_trace", 0);
	g_BetaConfig.scheduler_io_trace =
		ReadIniInt(path, "scheduler_io_trace", 0);
	g_BetaConfig.scheduler_io_probe_address =
		ReadIniUInt32(path, "scheduler_io_probe_address", 0);
	g_BetaConfig.scheduler_io_wait_trace =
		ReadIniInt(path, "scheduler_io_wait_trace", 1);
	g_BetaConfig.scheduler_io_thread_trace =
		ReadIniInt(path, "scheduler_io_thread_trace", 1);
	g_BetaConfig.scheduler_io_event_trace =
		ReadIniInt(path, "scheduler_io_event_trace", 1);
	g_BetaConfig.scheduler_io_file_trace =
		ReadIniInt(path, "scheduler_io_file_trace", 1);
	g_BetaConfig.scheduler_io_cri_watch_base =
		ReadIniUInt32(path, "scheduler_io_cri_watch_base", 0);
	g_BetaConfig.scheduler_io_cri_watch_stride =
		ReadIniUInt32(path, "scheduler_io_cri_watch_stride", 340);
	g_BetaConfig.scheduler_io_cri_watch_count =
		ReadIniUInt32(path, "scheduler_io_cri_watch_count", 0);
#if defined(_DEBUG)
	if (g_BetaConfig.full_trace) {
		g_PerfTraceEnabled = true;
		g_RenderTraceEnabled = true;
		log_enable_full_trace();
	}
#else
	// Investigation traces must never create files or add runtime overhead in
	// public Release builds, even when a developer beta.ini is copied beside it.
	g_BetaConfig.full_trace = 0;
	g_BetaConfig.scheduler_io_trace = 0;
#endif

	// Kernel
	g_BetaConfig.apc_try_lock             = ReadIniInt(path, "apc_try_lock", 1);
	g_BetaConfig.satisfy_wait_apc_flags   = ReadIniInt(path, "satisfy_wait_apc_flags", 1);
	g_BetaConfig.wait_list_toctou_recheck = ReadIniInt(path, "wait_list_toctou_recheck", 1);
	g_BetaConfig.notification_event_wait_recheck =
		ReadIniInt(path, "notification_event_wait_recheck", 0);
	g_BetaConfig.cooperative_self_suspend =
		ReadIniInt(path, "cooperative_self_suspend", 0);
	g_BetaConfig.timer_try_lock           = ReadIniInt(path, "timer_try_lock", 1);

	// D3D
	g_BetaConfig.rt_texture_alias         = ReadIniInt(path, "rt_texture_alias", 1);
	g_BetaConfig.default_transparent_tex  = ReadIniInt(path, "default_transparent_tex", 1);
	g_BetaConfig.rt_resolve_self_sampling = ReadIniInt(path, "rt_resolve_self_sampling", 1);
	g_BetaConfig.vsh_constant_delta_upload = ReadIniInt(path, "vsh_constant_delta_upload", 1);
	g_BetaConfig.staging_surface_pool     = ReadIniInt(path, "staging_surface_pool", 1);
	g_BetaConfig.texture_stage_cache      = ReadIniInt(path, "texture_stage_cache", 1);
	g_BetaConfig.ff_nv2a_blend_matrices   = ReadIniInt(path, "ff_nv2a_blend_matrices", 1);
	g_BetaConfig.ff_hlsl_vertex_shader    = ReadIniInt(path, "ff_hlsl_vertex_shader", 1);
	g_BetaConfig.ff_force_full_rebuild    = ReadIniInt(path, "ff_force_full_rebuild", 0);
	g_BetaConfig.or2_draw_probe           = ReadIniInt(path, "or2_draw_probe", 0);

	// Timer
	g_BetaConfig.precise_sleep_timer      = ReadIniInt(path, "precise_sleep_timer", 1);
	g_BetaConfig.periodic_irq10           = ReadIniInt(path, "periodic_irq10", 1);
	g_BetaConfig.llong_min_timeout_fix    = ReadIniInt(path, "llong_min_timeout_fix", 1);
	g_BetaConfig.llong_min_timeout_sleep_ms = ReadIniInt(path, "llong_min_timeout_sleep_ms", 0);
	g_BetaConfig.get_now_lock             = ReadIniInt(path, "get_now_lock", 1);
	g_BetaConfig.atomic_interrupts        = ReadIniInt(path, "atomic_interrupts", 1);
	g_BetaConfig.system_events_other_affinity = ReadIniInt(path, "system_events_other_affinity", 0);
	g_BetaConfig.system_events_normal_priority = ReadIniInt(path, "system_events_normal_priority", 0);
	g_BetaConfig.wmmt_device_poll_yield_ms =
		ReadIniInt(path, "wmmt_device_poll_yield_ms", 0);
	g_BetaConfig.wmmt_gamepad_init_bypass =
		ReadIniInt(path, "wmmt_gamepad_init_bypass", 0);

	// KiTimerExpiration safety
	g_BetaConfig.timer_exp_pointer_guard  = ReadIniInt(path, "timer_exp_pointer_guard", 1);
	g_BetaConfig.timer_exp_max_expired    = ReadIniInt(path, "timer_exp_max_expired", 0);
	g_BetaConfig.timer_exp_max_ticks      = ReadIniInt(path, "timer_exp_max_ticks", 0);

	// Crazy Taxi game-specific
	g_BetaConfig.ct_gamepad_init_bypass =
		ReadIniInt(path, "ct_gamepad_init_bypass", 0);
	g_BetaConfig.ct_skip_movies           = ReadIniInt(path, "ct_skip_movies", 0);
	g_BetaConfig.ct_cri_drive_movie_server =
		ReadIniInt(path, "ct_cri_drive_movie_server", 0);
	g_BetaConfig.ct_cri_force_complete    = ReadIniInt(path, "ct_cri_force_complete", 0);
	g_BetaConfig.ct_cri_wait_timeout_ms   =
		ReadIniUInt32(path, "ct_cri_wait_timeout_ms", 3000);
	g_BetaConfig.ct_infinite_timer        = ReadIniInt(path, "ct_infinite_timer", 0);
	g_BetaConfig.ct_timer_lock_value      =
		ReadIniUInt32(path, "ct_timer_lock_value", 3000);
	g_BetaConfig.ct_debug_course         = ReadIniInt(path, "ct_debug_course", -1);
#if defined(_DEBUG)
	g_BetaConfig.ct_debug_movie_dump     =
		ReadIniInt(path, "ct_debug_movie_dump", 0);
#else
	g_BetaConfig.ct_debug_movie_dump     = 0;
#endif
	g_BetaConfig.ct_debug_mip_lod_bias   =
		ReadIniInt(path, "ct_debug_mip_lod_bias", 99);
	g_BetaConfig.ct_debug_disable_lighting =
		ReadIniInt(path, "ct_debug_disable_lighting", 0);
	g_BetaConfig.ct_debug_point_light_range_percent =
		ReadIniInt(path, "ct_debug_point_light_range_percent", 100);
	g_BetaConfig.ct_debug_point_light_attenuation_percent =
		ReadIniInt(path, "ct_debug_point_light_attenuation_percent", 100);
	g_BetaConfig.ct_debug_fog_mode =
		ReadIniInt(path, "ct_debug_fog_mode", 0);
	g_BetaConfig.ct_debug_skip_noise_textures =
		ReadIniInt(path, "ct_debug_skip_noise_textures", 0);
	g_BetaConfig.ct_debug_stencil_volume_mode =
		ReadIniInt(path, "ct_debug_stencil_volume_mode", 0);
	g_BetaConfig.ct_headlights =
		ReadIniInt(path, "ct_headlights", 1);

	// Chihiro MediaBoard
	g_BetaConfig.mb_board_type            = ReadIniInt(path, "mb_board_type", 0);
	g_BetaConfig.mb_dimm_size             = ReadIniInt(path, "mb_dimm_size", 0);
	g_BetaConfig.mb_trace                 = ReadIniInt(path, "mb_trace", 0);
}

void BetaTrace_Record(const char* category, const char* format, ...)
{
#if !defined(_DEBUG)
	(void)category;
	(void)format;
	return;
#else
	if (!g_BetaConfig.scheduler_io_trace || category == nullptr ||
		format == nullptr) {
		return;
	}
	if ((!g_BetaConfig.scheduler_io_wait_trace &&
			(strncmp(category, "WAIT_", 5) == 0 ||
				strncmp(category, "NT_WAIT_", 8) == 0)) ||
		(!g_BetaConfig.scheduler_io_thread_trace &&
			strncmp(category, "THREAD_", 7) == 0) ||
		(!g_BetaConfig.scheduler_io_event_trace &&
			strncmp(category, "EVENT_", 6) == 0) ||
		(!g_BetaConfig.scheduler_io_file_trace &&
			(strncmp(category, "READ_", 5) == 0 ||
				strncmp(category, "FILE_", 5) == 0))) {
		return;
	}

	char message[1024] = {};
	va_list args;
	va_start(args, format);
	vsnprintf(message, sizeof(message), format, args);
	va_end(args);

	std::lock_guard<std::mutex> lock(g_BetaTraceMutex);
	BetaTrace_OpenLocked();
	if (g_BetaTraceFile == nullptr) {
		return;
	}

	LARGE_INTEGER now = {};
	QueryPerformanceCounter(&now);
	const long long elapsedTicks = now.QuadPart - g_BetaTraceStart.QuadPart;
	const long long elapsedMicros = g_BetaTraceFrequency.QuadPart > 0
		? (elapsedTicks * 1000000LL) / g_BetaTraceFrequency.QuadPart
		: 0;

	fprintf(
		g_BetaTraceFile,
		"%010llu +%lld.%06lld tid=%08lX %-12s %s\n",
		static_cast<unsigned long long>(++g_BetaTraceSequence),
		elapsedMicros / 1000000LL,
		elapsedMicros % 1000000LL,
		GetCurrentThreadId(),
		category,
		message);
	fflush(g_BetaTraceFile);
#endif
}

bool BetaTrace_ReadProbe(std::uint32_t* value)
{
#if !defined(_DEBUG)
	(void)value;
	return false;
#else
	if (!g_BetaConfig.scheduler_io_trace ||
		g_BetaConfig.scheduler_io_probe_address == 0 ||
		value == nullptr) {
		return false;
	}

	__try {
		*value = *reinterpret_cast<volatile std::uint32_t*>(
			static_cast<std::uintptr_t>(
				g_BetaConfig.scheduler_io_probe_address));
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
#endif
}

void BetaTrace_WatchCriFileStates(const char* reason)
{
#if !defined(_DEBUG)
	(void)reason;
	return;
#else
	const std::uint32_t base = g_BetaConfig.scheduler_io_cri_watch_base;
	const std::uint32_t stride = g_BetaConfig.scheduler_io_cri_watch_stride;
	const std::uint32_t count =
		(g_BetaConfig.scheduler_io_cri_watch_count < 64)
			? g_BetaConfig.scheduler_io_cri_watch_count
			: 64;
	if (!g_BetaConfig.scheduler_io_trace || base == 0 || stride < 340 ||
		count == 0) {
		return;
	}

	std::lock_guard<std::mutex> lock(g_BetaCriWatchMutex);
	for (std::uint32_t slot = 0; slot < count; ++slot) {
		unsigned char snapshot[340] = {};
		if (!BetaTrace_CopyGuest(
				snapshot,
				static_cast<std::uintptr_t>(base + slot * stride),
				sizeof(snapshot))) {
			continue;
		}

		// Byte zero is the wxCi allocation marker. Ignore unused slots.
		if (snapshot[0] == 0) {
			g_BetaCriWatchSeen[slot] = false;
			g_BetaCriWatchHashes[slot] = 0;
			continue;
		}

		std::uint64_t hash = 1469598103934665603ULL;
		for (unsigned char byte : snapshot) {
			hash ^= byte;
			hash *= 1099511628211ULL;
		}
		if (g_BetaCriWatchSeen[slot] &&
			g_BetaCriWatchHashes[slot] == hash) {
			continue;
		}
		g_BetaCriWatchSeen[slot] = true;
		g_BetaCriWatchHashes[slot] = hash;

		auto readU32 = [&snapshot](std::size_t offset) {
			std::uint32_t value = 0;
			memcpy(&value, snapshot + offset, sizeof(value));
			return value;
		};
		char path[65] = {};
		memcpy(path, snapshot + 76, sizeof(path) - 1);
		for (char& character : path) {
			const unsigned char byte = static_cast<unsigned char>(character);
			if (character == '\0') {
				break;
			}
			if (byte < 0x20 || byte >= 0x7F) {
				character = '?';
			}
		}

		BetaTrace_Record(
			"CRI_STATE",
			"reason=%s slot=%u address=%08X alloc=%u state=%u buffer=%08X sector_size=%u file_size=%u sectors=%u position=%u transferred=%u request_sectors=%u cached=%u xbox_file=%08X iosb_status=%08X io_buffer=%08X offset=%u length=%u request=%u pending=%u mode=%u path=\"%s\"",
			reason != nullptr ? reason : "unknown",
			static_cast<unsigned int>(slot),
			static_cast<unsigned int>(base + slot * stride),
			static_cast<unsigned int>(snapshot[0]),
			static_cast<unsigned int>(snapshot[1]),
			static_cast<unsigned int>(readU32(4)),
			static_cast<unsigned int>(readU32(8)),
			static_cast<unsigned int>(readU32(12)),
			static_cast<unsigned int>(readU32(16)),
			static_cast<unsigned int>(readU32(20)),
			static_cast<unsigned int>(readU32(24)),
			static_cast<unsigned int>(readU32(28)),
			static_cast<unsigned int>(readU32(32)),
			static_cast<unsigned int>(readU32(36)),
			static_cast<unsigned int>(readU32(40)),
			static_cast<unsigned int>(readU32(48)),
			static_cast<unsigned int>(readU32(60)),
			static_cast<unsigned int>(readU32(64)),
			static_cast<unsigned int>(readU32(68)),
			static_cast<unsigned int>(readU32(72)),
			static_cast<unsigned int>(readU32(336)),
			path);
	}
#endif
}
