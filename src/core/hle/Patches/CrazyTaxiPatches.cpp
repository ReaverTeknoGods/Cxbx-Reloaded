// ******************************************************************
// *  Cxbx Crazy Taxi High Roller patches
// *
// *  This file is part of the Cxbx project.
// *
// *  Cxbx and Cxbe are free software; you can redistribute them
// *  and/or modify them under the terms of the GNU General Public
// *  License as published by the Free Software Foundation; either
// *  version 2 of the license, or (at your option) any later version.
// *
// *  This program is distributed in the hope that it will be useful,
// *  but WITHOUT ANY WARRANTY; without even the implied warranty of
// *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// *  GNU General Public License for more details.
// *
// *  You should have recieved a copy of the GNU General Public License
// *  along with this program; see the file COPYING.
// *  If not, write to the Free Software Foundation, Inc.,
// *  59 Temple Place - Suite 330, Bostom, MA 02111-1307, USA.
// *
// *  All rights reserved
// *
// ******************************************************************

#define LOG_PREFIX CXBXR_MODULE::JVS

#include "PatchUtil.h"
#include "ChihiroPatches.h"
#include "core\kernel\support\Emu.h"
#include "core\hle\D3D8\Direct3D9\Direct3D9.h"
#include "common\BetaConfig.h"
#include "devices\chihiro\JvsIo.h"
#include <cmath>
#include <cstdio>
#include <thread>

#if defined(_DEBUG)
#define TAXI_DEBUG_PRINTF(...) std::printf(__VA_ARGS__)
#else
#define TAXI_DEBUG_PRINTF(...) do {} while (0)
#endif

bool IsCrazyTaxiXbe(uint64_t xbeHash)
{
	return xbeHash == 0xF8CB941EC5A7B4B4ULL
		|| xbeHash == 0xf319c176ab55e589ULL
		|| xbeHash == 0xE9EE166CCCBD7847ULL;
}

// ── CRI ADXF_GetStat hook (CT-local copies of Gundam's CRI hook) ──
static uintptr_t g_CriExecServerVA = 0;
static uintptr_t g_CriExecSrv2VA = 0;
static uintptr_t g_CriExecSrv4VA = 0;

// Crazy Taxi services the same priority-5 CRI movie list from both its ADX
// worker and the title thread. Wine/Box32 can schedule those calls
// concurrently, but suppressing the worker call also suppresses the video and
// audio pipeline that makes the next movie ready. Preserve both callers and
// serialize only this non-reentrant CRI list. CRITICAL_SECTION is recursive, so
// a priority-5 callback can safely re-enter on its owning thread.
static INIT_ONCE g_TaxiCriPriority5InitOnce = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION g_TaxiCriPriority5Lock;

static BOOL CALLBACK TaxiInitializeCriPriority5Lock(
	PINIT_ONCE,
	PVOID,
	PVOID*)
{
	InitializeCriticalSection(&g_TaxiCriPriority5Lock);
	return TRUE;
}

class TaxiCriPriority5Guard
{
public:
	TaxiCriPriority5Guard()
	{
		InitOnceExecuteOnce(
			&g_TaxiCriPriority5InitOnce,
			TaxiInitializeCriPriority5Lock,
			nullptr,
			nullptr);
		EnterCriticalSection(&g_TaxiCriPriority5Lock);
	}

	~TaxiCriPriority5Guard()
	{
		LeaveCriticalSection(&g_TaxiCriPriority5Lock);
	}

	TaxiCriPriority5Guard(const TaxiCriPriority5Guard&) = delete;
	TaxiCriPriority5Guard& operator=(const TaxiCriPriority5Guard&) = delete;
};

static int TaxiDrivePriority5Serialized()
{
	TaxiCriPriority5Guard guard;
	using CriServerFn = int(__cdecl*)(int);
	return reinterpret_cast<CriServerFn>(0x000CBC30)(5);
}

static int __cdecl TaxiAdxWorkerMovieServerSerialized()
{
	TaxiCriPriority5Guard guard;
	using WorkerMovieServerFn = int(__cdecl*)();
	return reinterpret_cast<WorkerMovieServerFn>(0x000CBCA0)();
}

static int __cdecl TaxiGetStatHook(int handle) {
	int stat = -3;
	if (handle) {
		stat = *(char*)(handle + 1);
	}
	if (stat != 3) {
		typedef int (__cdecl *ExecFn)(void);
		if (g_CriExecSrv2VA) ((ExecFn)g_CriExecSrv2VA)();
		if (g_CriExecSrv4VA) ((ExecFn)g_CriExecSrv4VA)();
		if (g_CriExecServerVA) ((ExecFn)g_CriExecServerVA)();
		if (handle) stat = *(char*)(handle + 1);
		Sleep(0);
	}
	return stat;
}

// CRI Sofdec spin loop helper for Crazy Taxi.
// The game's init code spins on SFD_GetStat(0) waiting for status==3 (complete),
// but never calls CRI ExecServer, so async I/O completions are never processed.
// This helper drives ExecServer between iterations so the I/O actually completes.
static void __cdecl TaxiSfdSpinHelper() {
	typedef int (__cdecl *GetStatFn)(int);
	typedef void (__cdecl *ExecFn)(void);

	GetStatFn getStat = (GetStatFn)0xC7160;
	ExecFn exec2 = (ExecFn)0xCBC90;  // CRI ExecServer priority 2 (filesystem I/O)

	for (;;) {
		int stat = getStat(0);
		if (stat == 3) break;
		exec2();
		TaxiDrivePriority5Serialized();
		Sleep(0);
	}
}

// Delayed movie status hook — return "playing" for 5s, then "done"
static DWORD g_movieStartTick = 0;
static int __cdecl TaxiMovieGetStatus() {
	if (g_movieStartTick == 0) g_movieStartTick = GetTickCount();
	if (GetTickCount() - g_movieStartTick < 5000)
		return 1; // playing
	return 3; // done
}

// Crazy Taxi chooses CRI priority 4 or 5 for its Sofdec decoder worker from a
// runtime option. On the Android Wine/Box32 path, the AFS request completes but
// the selected decoder server can stop being serviced. Drive the normal
// filesystem, optional priority-4, and completion/decoder lists in the same
// order used by the title's other CRI helpers. Keep this opt-in until both
// Android and native Windows have passed. The throttle also prevents the two
// status queries in one game frame from advancing the decoder twice.
static volatile LONG g_TaxiMovieServerLastTick = 0;
static volatile LONG g_TaxiMovieTraceLastTick = 0;
static volatile LONG g_TaxiVideoModuleTraceLastTick = 0;
static volatile LONG g_TaxiAudioModuleTraceLastTick = 0;
static volatile LONG g_TaxiStateTraceStarted = 0;
static volatile LONG g_TaxiCurrentMovieFileId = -1;
static uint32_t g_TaxiMovieBusyStream = 0;
static DWORD g_TaxiMovieBusySinceTick = 0;
static bool g_TaxiMovieBusyRecovered = false;

struct TaxiMovieTraceState {
	uint32_t movie = 0;
	uint32_t movieActive = 0xFFFFFFFFU;
	uint32_t movieState = 0xFFFFFFFFU;
	uint32_t movieBusy = 0xFFFFFFFFU;
	uint32_t movieServerBusy = 0xFFFFFFFFU;
	uint32_t moviePoolPaused = 0xFFFFFFFFU;
	uint32_t movieSystemReady = 0xFFFFFFFFU;
	uint32_t movieSystemBusy = 0xFFFFFFFFU;
	uint32_t movieSystemDispatching = 0xFFFFFFFFU;
	uint32_t movieSystemCurrentPlayer = 0;
	uint32_t stream = 0;
	uint32_t lsc = 0;
	uint32_t frame30 = 0;
	uint32_t frame31 = 0;
	uint32_t frame32 = 0;
	uint32_t frame33 = 0;
	uint32_t sfd = 0;
	uint32_t sfdStart = 0xFFFFFFFFU;
	uint32_t sfdState = 0xFFFFFFFFU;
	uint32_t sfdPhase = 0xFFFFFFFFU;
	uint32_t sfdStatus = 0xFFFFFFFFU;
	uint32_t sfdTrack5Enabled = 0xFFFFFFFFU;
	uint32_t sfdTrack6Enabled = 0xFFFFFFFFU;
	uint32_t sfdTrack6ReadyA = 0xFFFFFFFFU;
	uint32_t sfdTrack6ReadyB = 0xFFFFFFFFU;
	uint32_t sfdTrack7ReadyA = 0xFFFFFFFFU;
	uint32_t sfdTrack7ReadyB = 0xFFFFFFFFU;
	uint32_t sfdTrack6Module = 0xFFFFFFFFU;
	uint32_t sfdTrack6ModuleReadyA = 0xFFFFFFFFU;
	uint32_t sfdTrack6ModuleReadyB = 0xFFFFFFFFU;
	uint32_t sfdTrack7Module = 0xFFFFFFFFU;
	uint32_t sfdTrack7ModuleReadyA = 0xFFFFFFFFU;
	uint32_t sfdTrack7ModuleReadyB = 0xFFFFFFFFU;
	uint32_t sfdModuleReadyA[9] = {};
	uint32_t sfdModuleReadyB[9] = {};
	uint32_t sfdModulePresentMask = 0;
	uint32_t criState = 0xFFFFFFFFU;
	uint32_t criRequest = 0xFFFFFFFFU;
	uint32_t criPending = 0xFFFFFFFFU;
	uint32_t priority4Callbacks[4] = {};
	uint32_t priority4Contexts[4] = {};
	uint32_t priority4Dispatches = 0xFFFFFFFFU;
	uint32_t priority5Callbacks[4] = {};
	uint32_t priority5Contexts[4] = {};
	uint32_t priority5Dispatches = 0xFFFFFFFFU;
	uint32_t firstCallbackCode[2] = {};
	uint32_t firstPriority5Code[2] = {};
	bool readable = false;
};

static TaxiMovieTraceState TaxiReadMovieTraceState(uint32_t* movieWrapper)
{
	TaxiMovieTraceState state = {};
	__try {
		if (movieWrapper == nullptr || movieWrapper[0] == 0) {
			return state;
		}

		state.movie = movieWrapper[0];
		const auto movie =
			reinterpret_cast<const uint32_t*>(state.movie);
		state.movieActive = movie[1];
		state.movieState = movie[2];
		state.movieBusy = movie[24];
		state.movieServerBusy =
			*reinterpret_cast<const uint32_t*>(state.movie - 16);
		state.moviePoolPaused =
			*reinterpret_cast<const uint32_t*>(state.movie - 68);
		state.movieSystemReady =
			*reinterpret_cast<const uint32_t*>(0x00381AF4);
		const auto movieSystem =
			reinterpret_cast<const uint32_t*>(0x003DD4C0);
		state.movieSystemBusy = movieSystem[9];
		state.movieSystemDispatching = movieSystem[22];
		state.movieSystemCurrentPlayer =
			*reinterpret_cast<const uint32_t*>(0x003DD4A0);
		state.stream = movie[17];
		state.lsc = movie[19];
		state.frame30 = movie[30];
		state.frame31 = movie[31];
		state.frame32 = movie[32];
		state.frame33 = movie[33];
		state.sfd = movie[16];
		if (state.sfd != 0) {
			const auto sfd =
				reinterpret_cast<const uint32_t*>(state.sfd);
			state.sfdStart = sfd[15];
			state.sfdState = sfd[16];
			state.sfdPhase = sfd[17];
			state.sfdStatus = sfd[18];
			state.sfdTrack5Enabled = sfd[619];
			state.sfdTrack6Enabled = sfd[620];
			state.sfdTrack6ReadyA = sfd[6348];
			state.sfdTrack6ReadyB = sfd[6349];
			state.sfdTrack7ReadyA = sfd[6857];
			state.sfdTrack7ReadyB = sfd[6858];
			state.sfdTrack6Module = sfd[6844];
			state.sfdTrack6ModuleReadyA =
				sfd[274 * state.sfdTrack6Module + 1104];
			state.sfdTrack6ModuleReadyB =
				sfd[274 * state.sfdTrack6Module + 1105];
			state.sfdTrack7Module = sfd[7353];
			state.sfdTrack7ModuleReadyA =
				sfd[274 * state.sfdTrack7Module + 1104];
			state.sfdTrack7ModuleReadyB =
				sfd[274 * state.sfdTrack7Module + 1105];
			for (size_t index = 0; index < 9; ++index) {
				state.sfdModuleReadyA[index] =
					sfd[274 * index + 1104];
				state.sfdModuleReadyB[index] =
					sfd[274 * index + 1105];
				if (sfd[509 * index + 3789] != 0) {
					state.sfdModulePresentMask |= 1U << index;
				}
			}
		}
		const auto criSlot =
			reinterpret_cast<const uint8_t*>(0x003D6D80);
		state.criState = criSlot[1];
		state.criRequest =
			*reinterpret_cast<const uint32_t*>(criSlot + 68);
		state.criPending =
			*reinterpret_cast<const uint32_t*>(criSlot + 72);
		const auto priority4 =
			reinterpret_cast<const uint32_t*>(0x003431A0);
		for (size_t index = 0; index < 4; ++index) {
			state.priority4Callbacks[index] =
				priority4[index * 2];
			state.priority4Contexts[index] =
				priority4[index * 2 + 1];
		}
		state.priority4Dispatches =
			*reinterpret_cast<const uint32_t*>(0x003D4330);
		const auto priority5 =
			reinterpret_cast<const uint32_t*>(0x003431C0);
		for (size_t index = 0; index < 4; ++index) {
			state.priority5Callbacks[index] =
				priority5[index * 2];
			state.priority5Contexts[index] =
				priority5[index * 2 + 1];
		}
		state.priority5Dispatches =
			*reinterpret_cast<const uint32_t*>(0x003D4334);
		if (state.priority4Callbacks[0] != 0) {
			const auto callbackCode =
				reinterpret_cast<const uint32_t*>(
					state.priority4Callbacks[0]);
			state.firstCallbackCode[0] = callbackCode[0];
			state.firstCallbackCode[1] = callbackCode[1];
		}
		if (state.priority5Callbacks[0] != 0) {
			const auto callbackCode =
				reinterpret_cast<const uint32_t*>(
					state.priority5Callbacks[0]);
			state.firstPriority5Code[0] = callbackCode[0];
			state.firstPriority5Code[1] = callbackCode[1];
		}
		state.readable = true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		state.readable = false;
	}
	return state;
}

// Diagnostic modes isolate Crazy Taxi's multiplexed Sofdec start handshake
// without changing the AFS or XBE. Entry 20 is video-only and completes, while
// later entries can remain in state 2 with both track-ready flags clear.
// Mode 2 disables audio. Mode 3 additionally acknowledges video-track setup,
// matching mwSfd's own E0F10(handle, 6, 1) state update, to determine whether
// the decoder is stuck before or after that callback.
static void TaxiMaybeIsolateMovieHandshakeForDiagnostic(
	uint32_t* movieWrapper)
{
	if (g_BetaConfig.ct_cri_drive_movie_server < 2 ||
		movieWrapper == nullptr ||
		movieWrapper[0] == 0) {
		return;
	}

	__try {
		auto* const movie =
			reinterpret_cast<uint32_t*>(movieWrapper[0]);
		auto* const sfd =
			reinterpret_cast<uint32_t*>(movie[16]);
		if (sfd != nullptr && sfd[620] != 0) {
			sfd[620] = 0;
			BetaTrace_Record(
				"CT_MOVIE_DIAG",
				"kind=disable-audio movie=%08X stream=%08X sfd=%08X state=%u phase=%u",
				static_cast<unsigned int>(
					reinterpret_cast<uintptr_t>(movie)),
				movie[17],
				static_cast<unsigned int>(
					reinterpret_cast<uintptr_t>(sfd)),
				sfd[16],
				sfd[17]);
		}
		if (sfd != nullptr &&
			g_BetaConfig.ct_cri_drive_movie_server == 3 &&
			sfd[16] == 2 &&
			sfd[6348] == 0 &&
			sfd[6349] == 0) {
			using SetTrackReadyFn = int(__cdecl*)(uint32_t*, int, int);
			reinterpret_cast<SetTrackReadyFn>(0x000E0F10)(sfd, 6, 1);
			BetaTrace_Record(
				"CT_MOVIE_DIAG",
				"kind=ack-video-track movie=%08X stream=%08X sfd=%08X state=%u phase=%u",
				static_cast<unsigned int>(
					reinterpret_cast<uintptr_t>(movie)),
				movie[17],
				static_cast<unsigned int>(
					reinterpret_cast<uintptr_t>(sfd)),
				sfd[16],
				sfd[17]);
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
	}
}

static bool TaxiTraceMovieState(
	const char* phase,
	uint32_t* movieWrapper,
	int status,
	bool force)
{
	if (!g_BetaConfig.scheduler_io_trace) {
		return false;
	}

	const LONG now = static_cast<LONG>(GetTickCount());
	if (!force) {
		const LONG previous =
			InterlockedCompareExchange(&g_TaxiMovieTraceLastTick, 0, 0);
		if (static_cast<DWORD>(now - previous) < 1000 ||
			InterlockedCompareExchange(
				&g_TaxiMovieTraceLastTick,
				now,
				previous) != previous) {
			return false;
		}
	}

	const TaxiMovieTraceState state =
		TaxiReadMovieTraceState(movieWrapper);
	BetaTrace_Record(
		"CT_MOVIE",
		"phase=%s wrapper=%08X status=%d readable=%u movie=%08X active=%u movie_state=%u busy=%u server_busy=%u pool_paused=%u system_ready=%u system_busy=%u system_dispatching=%u current_player=%08X stream=%08X lsc=%08X frames=%u,%u,%u,%u sfd=%08X sfd_start=%u sfd_state=%u sfd_phase=%u sfd_status=%u tracks=%u,%u ready6=%u,%u module6=%u raw6=%u,%u ready7=%u,%u module7=%u raw7=%u,%u module_mask=%03X raw_modules=0:%u/%u,1:%u/%u,2:%u/%u,3:%u/%u,4:%u/%u,5:%u/%u,6:%u/%u,7:%u/%u,8:%u/%u cri_state=%u cri_request=%u cri_pending=%u p4=%08X/%08X,%08X/%08X,%08X/%08X,%08X/%08X p4_calls=%u p4_code=%08X,%08X p5=%08X/%08X,%08X/%08X,%08X/%08X,%08X/%08X p5_calls=%u p5_code=%08X,%08X",
		phase,
		static_cast<unsigned int>(
			reinterpret_cast<uintptr_t>(movieWrapper)),
		status,
		state.readable ? 1U : 0U,
		state.movie,
		state.movieActive,
		state.movieState,
		state.movieBusy,
		state.movieServerBusy,
		state.moviePoolPaused,
		state.movieSystemReady,
		state.movieSystemBusy,
		state.movieSystemDispatching,
		state.movieSystemCurrentPlayer,
		state.stream,
		state.lsc,
		state.frame30,
		state.frame31,
		state.frame32,
		state.frame33,
		state.sfd,
		state.sfdStart,
		state.sfdState,
		state.sfdPhase,
		state.sfdStatus,
		state.sfdTrack5Enabled,
		state.sfdTrack6Enabled,
		state.sfdTrack6ReadyA,
		state.sfdTrack6ReadyB,
		state.sfdTrack6Module,
		state.sfdTrack6ModuleReadyA,
		state.sfdTrack6ModuleReadyB,
		state.sfdTrack7ReadyA,
		state.sfdTrack7ReadyB,
		state.sfdTrack7Module,
		state.sfdTrack7ModuleReadyA,
		state.sfdTrack7ModuleReadyB,
		state.sfdModulePresentMask,
		state.sfdModuleReadyA[0],
		state.sfdModuleReadyB[0],
		state.sfdModuleReadyA[1],
		state.sfdModuleReadyB[1],
		state.sfdModuleReadyA[2],
		state.sfdModuleReadyB[2],
		state.sfdModuleReadyA[3],
		state.sfdModuleReadyB[3],
		state.sfdModuleReadyA[4],
		state.sfdModuleReadyB[4],
		state.sfdModuleReadyA[5],
		state.sfdModuleReadyB[5],
		state.sfdModuleReadyA[6],
		state.sfdModuleReadyB[6],
		state.sfdModuleReadyA[7],
		state.sfdModuleReadyB[7],
		state.sfdModuleReadyA[8],
		state.sfdModuleReadyB[8],
		state.criState,
		state.criRequest,
		state.criPending,
		state.priority4Callbacks[0],
		state.priority4Contexts[0],
		state.priority4Callbacks[1],
		state.priority4Contexts[1],
		state.priority4Callbacks[2],
		state.priority4Contexts[2],
		state.priority4Callbacks[3],
		state.priority4Contexts[3],
		state.priority4Dispatches,
		state.firstCallbackCode[0],
		state.firstCallbackCode[1],
		state.priority5Callbacks[0],
		state.priority5Contexts[0],
		state.priority5Callbacks[1],
		state.priority5Contexts[1],
		state.priority5Callbacks[2],
		state.priority5Contexts[2],
		state.priority5Callbacks[3],
		state.priority5Contexts[3],
		state.priority5Dispatches,
		state.firstPriority5Code[0],
		state.firstPriority5Code[1]);
	return true;
}

static bool TaxiShouldTraceModule(volatile LONG* lastTick)
{
	const LONG now = static_cast<LONG>(GetTickCount());
	const LONG previous = InterlockedCompareExchange(lastTick, 0, 0);
	return static_cast<DWORD>(now - previous) >= 500 &&
		InterlockedCompareExchange(lastTick, now, previous) == previous;
}

// The multiplexed Sofdec path creates separate video and audio decoder
// modules. Their normal method-2 callbacks are sub_DA6F0 and sub_D78B0,
// respectively. Trace those callbacks rather than forcing their ready flags:
// the flags are consumers of a successful decode and acknowledging them early
// merely skips the movie without producing frames.
static int __cdecl TaxiVideoDecoderModuleTraceHook(uint32_t* sfd)
{
	using VideoDecoderServiceFn = int(__cdecl*)(uint32_t*);
	const auto service =
		reinterpret_cast<VideoDecoderServiceFn>(0x000DA6F0);
	const int result = service(sfd);
	if (sfd != nullptr &&
		TaxiShouldTraceModule(&g_TaxiVideoModuleTraceLastTick)) {
		__try {
			const uint32_t inputModule = sfd[4808];
			const uint32_t outputModule = sfd[4809];
			using QueryModuleValueFn = int(__cdecl*)(uint32_t*, int);
			const auto getBuffered =
				reinterpret_cast<QueryModuleValueFn>(0x000E0520);
			const auto getPosition =
				reinterpret_cast<QueryModuleValueFn>(0x000E0500);
			using VideoReadyFn = int(__cdecl*)(uint32_t*);
			const auto videoReady =
				reinterpret_cast<VideoReadyFn>(0x000D97F0);
			BetaTrace_Record(
				"CT_VIDEO_MODULE",
				"result=%d state=%u phase=%u input=%u ready=%u/%u buffered=%d position=%d output=%u ready=%u/%u decoder=%08X decoder_done=%u queue_count=%u queue_limit=%u frame_limit=%u stream_end=%u frame_seen=%u ready_gate=%u",
				result,
				sfd[16],
				sfd[17],
				inputModule,
				sfd[274 * inputModule + 1104],
				sfd[274 * inputModule + 1105],
				getBuffered(sfd, inputModule),
				getPosition(sfd, inputModule),
				outputModule,
				sfd[274 * outputModule + 1104],
				sfd[274 * outputModule + 1105],
				sfd[4314],
				sfd[4345],
				sfd[4385],
				sfd[637],
				sfd[11],
				sfd[28],
				sfd[59],
				videoReady(sfd));
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			BetaTrace_Record(
				"CT_VIDEO_MODULE",
				"result=%d unreadable=1",
				result);
		}
	}
	return result;
}

static int __cdecl TaxiAudioDecoderModuleTraceHook(uint32_t* sfd)
{
	using AudioDecoderServiceFn = int(__cdecl*)(uint32_t*);
	const auto service =
		reinterpret_cast<AudioDecoderServiceFn>(0x000D78B0);
	const int result = service(sfd);
	if (sfd != nullptr &&
		TaxiShouldTraceModule(&g_TaxiAudioModuleTraceLastTick)) {
		__try {
			const uint32_t inputModule = sfd[5317];
			const uint32_t outputModule = sfd[5318];
			using QueryModuleValueFn = int(__cdecl*)(uint32_t*, int);
			const auto getBuffered =
				reinterpret_cast<QueryModuleValueFn>(0x000E0520);
			const auto getPosition =
				reinterpret_cast<QueryModuleValueFn>(0x000E0500);
			const uint32_t adxt = sfd[4823];
			const int adxtStatus = adxt != 0
				? static_cast<int>(
					*reinterpret_cast<const int8_t*>(adxt + 1))
				: -1;
			BetaTrace_Record(
				"CT_AUDIO_MODULE",
				"result=%d state=%u phase=%u input=%u ready=%u/%u buffered=%d position=%d output=%u ready=%u/%u adxt=%08X adxt_status=%d adxt_error=%d decoded=%u consumed=%u queued=%u,%u",
				result,
				sfd[16],
				sfd[17],
				inputModule,
				sfd[274 * inputModule + 1104],
				sfd[274 * inputModule + 1105],
				getBuffered(sfd, inputModule),
				getPosition(sfd, inputModule),
				outputModule,
				sfd[274 * outputModule + 1104],
				sfd[274 * outputModule + 1105],
				adxt,
				adxtStatus,
				adxt != 0
					? static_cast<int>(
						*reinterpret_cast<const int16_t*>(
							adxt + 96))
					: -1,
				sfd[922],
				sfd[923],
				sfd[920],
				sfd[921]);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			BetaTrace_Record(
				"CT_AUDIO_MODULE",
				"result=%d unreadable=1",
				result);
		}
	}
	return result;
}

static int __cdecl TaxiMovieAfsStartStateHook(
	uint32_t* movieObject,
	unsigned int afsIndex,
	int fileId)
{
	InterlockedExchange(&g_TaxiCurrentMovieFileId, fileId);
	using MovieAfsStartFn =
		int(__cdecl*)(uint32_t*, unsigned int, int);
	return reinterpret_cast<MovieAfsStartFn>(0x000D47B0)(
		movieObject,
		afsIndex,
		fileId);
}

static bool TaxiRecoverStaleMovieWorker(
	uint32_t* movieWrapper,
	int status)
{
	if (!g_BetaConfig.ct_cri_drive_movie_server ||
		status != 1) {
		g_TaxiMovieBusyStream = 0;
		g_TaxiMovieBusySinceTick = 0;
		g_TaxiMovieBusyRecovered = false;
		return false;
	}

	const TaxiMovieTraceState state =
		TaxiReadMovieTraceState(movieWrapper);
	const bool stalledBeforeFirstFrame =
		state.readable &&
		state.movieActive == 1 &&
		state.movieBusy == 1 &&
		state.movieServerBusy == 1 &&
		state.moviePoolPaused == 0 &&
		state.movieSystemReady == 1 &&
		state.frame30 == 0 &&
		state.frame31 == 0 &&
		state.frame32 == 0 &&
		state.frame33 == 0 &&
		state.sfdStart == 1 &&
		(state.sfdState == 1 || state.sfdState == 2) &&
		state.criState == 1 &&
		state.criRequest == 0 &&
		state.criPending == 0;
	if (!stalledBeforeFirstFrame) {
		g_TaxiMovieBusyStream = 0;
		g_TaxiMovieBusySinceTick = 0;
		g_TaxiMovieBusyRecovered = false;
		return false;
	}

	const DWORD now = GetTickCount();
	if (g_TaxiMovieBusyStream != state.stream) {
		g_TaxiMovieBusyStream = state.stream;
		g_TaxiMovieBusySinceTick = now;
		g_TaxiMovieBusyRecovered = false;
		return false;
	}
	const DWORD recoveryDelayMs =
		g_BetaConfig.ct_cri_wait_timeout_ms != 0
			? g_BetaConfig.ct_cri_wait_timeout_ms
			: 500;
	if (g_TaxiMovieBusyRecovered ||
		now - g_TaxiMovieBusySinceTick < recoveryDelayMs) {
		return false;
	}

	auto* const serverBusyFlag =
		reinterpret_cast<volatile LONG*>(state.movie - 16);
	if (InterlockedCompareExchange(serverBusyFlag, 0, 1) != 1) {
		return false;
	}

	auto* const movieBusyFlag =
		reinterpret_cast<volatile LONG*>(state.movie + 96);
	if (InterlockedCompareExchange(movieBusyFlag, 0, 1) != 1) {
		InterlockedCompareExchange(serverBusyFlag, 1, 0);
		return false;
	}

	g_TaxiMovieBusyRecovered = true;
	BetaTrace_Record(
		"CT_MOVIE_RECOVER",
		"movie=%08X stream=%08X stale_ms=%lu old_busy=1 new_busy=0 old_server_busy=1 new_server_busy=0 movie_state=%u sfd_start=%u sfd_state=%u cri_state=%u cri_request=%u cri_pending=%u frames=%u,%u,%u,%u",
		state.movie,
		state.stream,
		static_cast<unsigned long>(now - g_TaxiMovieBusySinceTick),
		state.movieState,
		state.sfdStart,
		state.sfdState,
		state.criState,
		state.criRequest,
		state.criPending,
		state.frame30,
		state.frame31,
		state.frame32,
		state.frame33);
	return true;
}

static bool TaxiServiceMovieServers()
{
	const LONG now = static_cast<LONG>(GetTickCount());
	const LONG previous =
		InterlockedCompareExchange(&g_TaxiMovieServerLastTick, 0, 0);
	if (static_cast<DWORD>(now - previous) < 8 ||
		InterlockedCompareExchange(
			&g_TaxiMovieServerLastTick,
			now,
			previous) != previous) {
		return false;
	}

	using CriServerFn = int(__cdecl*)(int);
	const auto driveServer =
		reinterpret_cast<CriServerFn>(0x000CBC30);
	driveServer(2);
	driveServer(4);
	driveServer(5);
	return true;
}

static int __fastcall TaxiMovieStatusWithServiceHook(
	uint32_t* movieWrapper,
	void* /*edx*/)
{
	using MovieStatusFn = int(__thiscall*)(uint32_t*);
	const auto getStatus =
		reinterpret_cast<MovieStatusFn>(0x0003CA20);
	TaxiMaybeIsolateMovieHandshakeForDiagnostic(movieWrapper);
	int status = getStatus(movieWrapper);
	if (status == 3) {
		TaxiTraceMovieState(
			"complete",
			movieWrapper,
			status,
			true);
	}
	const bool traceThisPoll =
		TaxiTraceMovieState("before", movieWrapper, status, false);
	TaxiRecoverStaleMovieWorker(movieWrapper, status);
	if (!g_BetaConfig.ct_cri_drive_movie_server ||
		(status != 1 && status != 2)) {
		return status;
	}

	if (TaxiServiceMovieServers()) {
		status = getStatus(movieWrapper);
		if (traceThisPoll) {
			TaxiTraceMovieState(
				"after-servers-2-4-5",
				movieWrapper,
				status,
				true);
		}
	}
	return status;
}

// sub_C5260 waits up to 200 million tight iterations for the ADX worker to
// acknowledge a mode change. The Android Wine/Box32 worker can disappear after
// the paired Sofdec busy flags are recovered, leaving this teardown wait with
// no producer. Retain the same mode/service calls, add an alertable yield, and
// recover only this abandoned acknowledgement after a bounded grace period.
static int TaxiPriority5ServerCallbackWithTimeout()
{
	auto* const pending =
		reinterpret_cast<volatile LONG*>(0x003409A8);
	auto* const handle =
		reinterpret_cast<const uint32_t*>(0x003DAE48);
	auto* const activeMode =
		reinterpret_cast<const uint32_t*>(0x00340990);
	auto* const stoppedMode =
		reinterpret_cast<const uint32_t*>(0x003409A0);
	using SetModeFn = int(__stdcall*)(uint32_t, uint32_t);
	using ServiceFn = int(__stdcall*)(uint32_t);
	const auto setMode =
		reinterpret_cast<SetModeFn>(0x000AE689);
	const auto service =
		reinterpret_cast<ServiceFn>(0x000AE79B);

	InterlockedExchange(pending, 1);
	const DWORD started = GetTickCount();
	uint32_t iterations = 0;
	while (InterlockedCompareExchange(pending, 0, 0) != 0 &&
		GetTickCount() - started < 250) {
		setMode(*handle, *activeMode);
		service(*handle);
		TaxiDrivePriority5Serialized();
		SleepEx(0, TRUE);
		++iterations;
	}

	const DWORD elapsed = GetTickCount() - started;
	const bool recovered =
		InterlockedCompareExchange(pending, 0, 1) == 1;
	if (recovered) {
		BetaTrace_Record(
			"CT_MOVIE_RECOVER",
			"kind=adx-mode-ack stale_ms=%lu iterations=%u handle=%08X active_mode=%08X stopped_mode=%08X",
			static_cast<unsigned long>(elapsed),
			iterations,
			*handle,
			*activeMode,
			*stoppedMode);
	}
	return setMode(*handle, *stoppedMode);
}

// Trace the title's Sofdec core-stop routine one operation at a time while
// retaining its original ordering and ownership rules. This is installed only
// while the scheduler trace is active and lets Android identify the exact CRI
// call that fails to return at end-of-stream.
static void __cdecl TaxiMovieCoreStopTraceHook(uint32_t* movieObject)
{
	BetaTrace_Record(
		"CT_MOVIE_CORE_STOP",
		"phase=enter object=%08X sfd=%08X stream=%08X aux=%08X state=%u busy=%u",
		static_cast<unsigned int>(
			reinterpret_cast<uintptr_t>(movieObject)),
		movieObject != nullptr ? movieObject[16] : 0U,
		movieObject != nullptr ? movieObject[17] : 0U,
		movieObject != nullptr ? movieObject[53] : 0U,
		movieObject != nullptr ? movieObject[2] : 0U,
		movieObject != nullptr ? movieObject[24] : 0U);
	if (movieObject == nullptr || movieObject[16] == 0) {
		return;
	}

	BetaTrace_Record(
		"CT_MOVIE_CORE_STOP",
		"phase=before-server-quiesce object=%08X server_busy=%u movie_busy=%u",
		static_cast<unsigned int>(
			reinterpret_cast<uintptr_t>(movieObject)),
		*reinterpret_cast<const uint32_t*>(
			reinterpret_cast<uintptr_t>(movieObject) - 16),
		movieObject[24]);
	using GetMovieSystemFn = uint32_t*(__cdecl*)();
	auto* movieSystem =
		reinterpret_cast<GetMovieSystemFn>(0x000D3C40)();
	movieObject[23] = 1;
	movieSystem[9] = 1;
	auto* const priority5CallbackSlot =
		reinterpret_cast<const uint32_t*>(0x00343230);
	const uint32_t priority5Callback = priority5CallbackSlot[0];
	const uint32_t priority5Context = priority5CallbackSlot[1];
	BetaTrace_Record(
		"CT_MOVIE_CORE_STOP",
		"phase=before-server-callback callback=%08X context=%08X system=%08X system_busy=%u",
		priority5Callback,
		priority5Context,
		static_cast<unsigned int>(
			reinterpret_cast<uintptr_t>(movieSystem)),
		movieSystem[9]);
	if (priority5Callback != 0) {
		if (priority5Callback == 0x000C5260) {
			TaxiPriority5ServerCallbackWithTimeout();
		}
		else {
			using ServerCallbackFn = int(__cdecl*)(uint32_t);
			reinterpret_cast<ServerCallbackFn>(priority5Callback)(
				priority5Context);
		}
	}
	BetaTrace_Record(
		"CT_MOVIE_CORE_STOP",
		"phase=after-server-callback callback=%08X context=%08X",
		priority5Callback,
		priority5Context);
	movieSystem =
		reinterpret_cast<GetMovieSystemFn>(0x000D3C40)();
	movieObject[23] = 0;
	movieSystem[9] = 0;
	if (movieObject[24] == 1) {
		BetaTrace_Record(
			"CT_MOVIE_CORE_STOP",
			"phase=before-busy-drain object=%08X",
			static_cast<unsigned int>(
				reinterpret_cast<uintptr_t>(movieObject)));
		using ServiceFn = int(__cdecl*)();
		const auto service =
			reinterpret_cast<ServiceFn>(0x000C5720);
		for (uint32_t iteration = 0;
			iteration < 10 && movieObject[24] == 1;
			++iteration) {
			movieSystem =
				reinterpret_cast<GetMovieSystemFn>(0x000D3C40)();
			movieObject[23] = 1;
			movieSystem[9] = 1;
			service();
			movieSystem =
				reinterpret_cast<GetMovieSystemFn>(0x000D3C40)();
			movieObject[23] = 0;
			movieSystem[9] = 0;
		}
		BetaTrace_Record(
			"CT_MOVIE_CORE_STOP",
			"phase=after-busy-drain object=%08X movie_busy=%u",
			static_cast<unsigned int>(
				reinterpret_cast<uintptr_t>(movieObject)),
			movieObject[24]);
	}
	BetaTrace_Record(
		"CT_MOVIE_CORE_STOP",
		"phase=after-server-quiesce object=%08X server_busy=%u movie_busy=%u",
		static_cast<unsigned int>(
			reinterpret_cast<uintptr_t>(movieObject)),
		*reinterpret_cast<const uint32_t*>(
			reinterpret_cast<uintptr_t>(movieObject) - 16),
		movieObject[24]);

	using LockFn = int(__cdecl*)();
	using UnlockFn = int(__cdecl*)(int);
	BetaTrace_Record(
		"CT_MOVIE_CORE_STOP",
		"phase=before-lock object=%08X",
		static_cast<unsigned int>(
			reinterpret_cast<uintptr_t>(movieObject)));
	const int lockState = reinterpret_cast<LockFn>(0x000D3C20)();
	BetaTrace_Record(
		"CT_MOVIE_CORE_STOP",
		"phase=after-lock object=%08X lock=%d",
		static_cast<unsigned int>(
			reinterpret_cast<uintptr_t>(movieObject)),
		lockState);
	movieObject[2] = 0;
	reinterpret_cast<UnlockFn>(0x000D3C30)(lockState);
	BetaTrace_Record(
		"CT_MOVIE_CORE_STOP",
		"phase=after-unlock object=%08X",
		static_cast<unsigned int>(
			reinterpret_cast<uintptr_t>(movieObject)));

	const uint32_t sfd = movieObject[16];
	auto* const sfdState = reinterpret_cast<const uint32_t*>(sfd);
	BetaTrace_Record(
		"CT_MOVIE_CORE_STOP",
		"phase=before-sfd-stop sfd=%08X start=%u state=%u status=%u",
		sfd,
		sfdState[15],
		sfdState[16],
		sfdState[18]);
	using SfdStopFn = int(__cdecl*)(uint32_t);
	const int stopResult =
		reinterpret_cast<SfdStopFn>(0x000DD370)(sfd);
	BetaTrace_Record(
		"CT_MOVIE_CORE_STOP",
		"phase=after-sfd-stop sfd=%08X result=%d start=%u state=%u status=%u",
		sfd,
		stopResult,
		sfdState[15],
		sfdState[16],
		sfdState[18]);
	if (stopResult != 0) {
		using SetErrorFn = void(__cdecl*)(int);
		using ReportErrorFn = void(__cdecl*)(const char*, ...);
		reinterpret_cast<SetErrorFn>(0x000D3CF0)(-308);
		reinterpret_cast<ReportErrorFn>(0x000DB840)(
			"E2003 mwSfdStop:can't stop SFD");
	}

	const uint32_t stream = movieObject[17];
	if (stream != 0) {
		BetaTrace_Record(
			"CT_MOVIE_CORE_STOP",
			"phase=before-stream-free stream=%08X",
			stream);
		using StreamFreeFn = int(__cdecl*)(uint32_t);
		reinterpret_cast<StreamFreeFn>(0x000DBF60)(stream);
		movieObject[17] = 0;
		BetaTrace_Record(
			"CT_MOVIE_CORE_STOP",
			"phase=after-stream-free stream=%08X",
			stream);
	}

	const uint32_t auxiliary = movieObject[53];
	if (auxiliary != 0) {
		const uint32_t vtable =
			*reinterpret_cast<const uint32_t*>(auxiliary);
		const uint32_t callback =
			*reinterpret_cast<const uint32_t*>(vtable + 20);
		BetaTrace_Record(
			"CT_MOVIE_CORE_STOP",
			"phase=before-aux-free object=%08X vtable=%08X callback=%08X",
			auxiliary,
			vtable,
			callback);
		using AuxiliaryFreeFn = void(__cdecl*)(uint32_t);
		reinterpret_cast<AuxiliaryFreeFn>(callback)(auxiliary);
	}
	BetaTrace_Record(
		"CT_MOVIE_CORE_STOP",
		"phase=exit object=%08X",
		static_cast<unsigned int>(
			reinterpret_cast<uintptr_t>(movieObject)));
}

static void __fastcall TaxiMovieStopTraceHook(
	uint32_t* movieWrapper,
	void* /*edx*/)
{
	BetaTrace_Record(
		"CT_MOVIE_STOP",
		"phase=enter wrapper=%08X object=%08X",
		static_cast<unsigned int>(
			reinterpret_cast<uintptr_t>(movieWrapper)),
		movieWrapper != nullptr ? movieWrapper[0] : 0U);
	if (movieWrapper == nullptr || movieWrapper[0] == 0) {
		return;
	}

	const uint32_t movieObject = movieWrapper[0];
	const uint32_t movieVtable =
		*reinterpret_cast<const uint32_t*>(movieObject);
	const uint32_t movieStopCallback =
		*reinterpret_cast<const uint32_t*>(movieVtable + 20);
	BetaTrace_Record(
		"CT_MOVIE_STOP",
		"phase=before-callback object=%08X vtable=%08X callback=%08X",
		movieObject,
		movieVtable,
		movieStopCallback);
	using MovieStopCallback = void(__cdecl*)(uint32_t);
	reinterpret_cast<MovieStopCallback>(movieStopCallback)(movieObject);
	BetaTrace_Record(
		"CT_MOVIE_STOP",
		"phase=after-callback object=%08X",
		movieObject);

	BetaTrace_Record(
		"CT_MOVIE_STOP",
		"phase=before-audio audio=%08X",
		movieWrapper[1]);
	using StopAudioFn = int(__stdcall*)(uint32_t, int, int);
	reinterpret_cast<StopAudioFn>(0x000AE518)(
		movieWrapper[1],
		0,
		0x8000);
	BetaTrace_Record(
		"CT_MOVIE_STOP",
		"phase=after-audio audio=%08X",
		movieWrapper[1]);

	BetaTrace_Record(
		"CT_MOVIE_STOP",
		"phase=before-sync sync=%08X",
		static_cast<unsigned int>(
			reinterpret_cast<uintptr_t>(movieWrapper + 2)));
	using StopSyncFn = void(__thiscall*)(uint32_t*, int);
	reinterpret_cast<StopSyncFn>(0x000C2D10)(
		movieWrapper + 2,
		0);
	BetaTrace_Record(
		"CT_MOVIE_STOP",
		"phase=exit wrapper=%08X",
		static_cast<unsigned int>(
			reinterpret_cast<uintptr_t>(movieWrapper)));
}

static char TaxiTimelineTraceCall(
	const char* channel,
	int* timeline,
	int frame)
{
	uint32_t eventAddress = 0;
	int eventFrame = -1;
	int eventType = -1;
	int eventValue = -1;
	if (timeline != nullptr && timeline[2] != 0) {
		eventAddress = static_cast<uint32_t>(timeline[2]);
		const auto* event =
			reinterpret_cast<const int*>(eventAddress);
		eventFrame = event[0];
		eventType = event[1];
		eventValue = event[2];
	}
	const uint32_t movieController =
		*reinterpret_cast<const uint32_t*>(0x0032B648);
	const uint32_t movieControllerState =
		movieController != 0
			? *reinterpret_cast<const uint32_t*>(movieController + 8)
			: 0xFFFFFFFFU;
	BetaTrace_Record(
		"CT_TIMELINE",
		"phase=before channel=%s timeline=%08X frame=%d index=%d mode=%d event=%08X event_frame=%d event_type=%d event_value=%d movie_controller=%08X movie_state=%u",
		channel,
		static_cast<unsigned int>(
			reinterpret_cast<uintptr_t>(timeline)),
		frame,
		timeline != nullptr ? timeline[0] : -1,
		timeline != nullptr ? timeline[1] : -1,
		eventAddress,
		eventFrame,
		eventType,
		eventValue,
		movieController,
		movieControllerState);
	using TimelineFn = char(__thiscall*)(int*, int);
	const char result =
		reinterpret_cast<TimelineFn>(0x0007F310)(timeline, frame);
	BetaTrace_Record(
		"CT_TIMELINE",
		"phase=after channel=%s timeline=%08X frame=%d result=%d index=%d next_event=%08X movie_state=%u",
		channel,
		static_cast<unsigned int>(
			reinterpret_cast<uintptr_t>(timeline)),
		frame,
		static_cast<int>(result),
		timeline != nullptr ? timeline[0] : -1,
		timeline != nullptr ? static_cast<uint32_t>(timeline[2]) : 0U,
		movieController != 0
			? *reinterpret_cast<const uint32_t*>(movieController + 8)
			: 0xFFFFFFFFU);
	return result;
}

static int __cdecl TaxiMovieAfsStartTraceHook(
	uint32_t* movieObject,
	unsigned int afsIndex,
	int fileId)
{
	InterlockedExchange(&g_TaxiCurrentMovieFileId, fileId);
	BetaTrace_Record(
		"CT_MOVIE_START",
		"phase=enter object=%08X afs_index=%u file_id=%d state=%u stream=%08X",
		static_cast<unsigned int>(
			reinterpret_cast<uintptr_t>(movieObject)),
		afsIndex,
		fileId,
		movieObject != nullptr ? movieObject[2] : 0xFFFFFFFFU,
		movieObject != nullptr ? movieObject[17] : 0U);
	using IsValidFn = int(__cdecl*)(uint32_t*);
	if (movieObject == nullptr ||
		reinterpret_cast<IsValidFn>(0x000D4930)(movieObject) == 0) {
		return 0;
	}

	using GetInputFn = uint32_t(__cdecl*)(uint32_t*);
	const uint32_t input =
		reinterpret_cast<GetInputFn>(0x000D4B00)(movieObject);
	const uint32_t vtable = movieObject[0];
	const uint32_t resetCallback =
		*reinterpret_cast<const uint32_t*>(vtable + 60);
	BetaTrace_Record(
		"CT_MOVIE_START",
		"phase=before-reset object=%08X input=%08X vtable=%08X callback=%08X",
		static_cast<unsigned int>(
			reinterpret_cast<uintptr_t>(movieObject)),
		input,
		vtable,
		resetCallback);
	using ResetFn = void(__cdecl*)(uint32_t*, uint32_t);
	reinterpret_cast<ResetFn>(resetCallback)(movieObject, input);
	BetaTrace_Record(
		"CT_MOVIE_START",
		"phase=after-reset object=%08X",
		static_cast<unsigned int>(
			reinterpret_cast<uintptr_t>(movieObject)));

	using LockFn = int(__cdecl*)();
	using UnlockFn = int(__cdecl*)(int);
	const int lockState = reinterpret_cast<LockFn>(0x000D3C20)();
	int fileOffset = 0;
	int fileSize = 0;
	int fileMode = 0;
	BetaTrace_Record(
		"CT_MOVIE_START",
		"phase=before-afs-lookup afs_index=%u file_id=%d",
		afsIndex,
		fileId);
	using AfsLookupFn = int(__cdecl*)(
		unsigned int,
		int,
		uint8_t*,
		int*,
		int*,
		int*);
	const int lookupResult =
		reinterpret_cast<AfsLookupFn>(0x000C6F10)(
			afsIndex,
			fileId,
			reinterpret_cast<uint8_t*>(0x00381C18),
			&fileMode,
			&fileSize,
			&fileOffset);
	reinterpret_cast<UnlockFn>(0x000D3C30)(lockState);
	BetaTrace_Record(
		"CT_MOVIE_START",
		"phase=after-afs-lookup result=%d mode=%d size=%d offset=%d",
		lookupResult,
		fileMode,
		fileSize,
		fileOffset);
	if (lookupResult != 0) {
		return lookupResult;
	}

	const int streamLock = reinterpret_cast<LockFn>(0x000D3C20)();
	BetaTrace_Record(
		"CT_MOVIE_START",
		"phase=before-stream-open input=%08X mode=%d size=%d offset=%d",
		input,
		fileMode,
		fileSize,
		fileOffset);
	using StreamOpenFn = uint32_t(__cdecl*)(
		uint8_t*,
		int,
		int,
		int,
		uint32_t);
	const uint32_t stream =
		reinterpret_cast<StreamOpenFn>(0x000DC080)(
			reinterpret_cast<uint8_t*>(0x00381C18),
			fileMode,
			fileSize,
			fileOffset,
			input);
	movieObject[17] = stream;
	reinterpret_cast<UnlockFn>(0x000D3C30)(streamLock);
	BetaTrace_Record(
		"CT_MOVIE_START",
		"phase=after-stream-open stream=%08X",
		stream);
	if (stream == 0) {
		return 0;
	}

	BetaTrace_Record(
		"CT_MOVIE_START",
		"phase=before-player-start object=%08X stream=%08X",
		static_cast<unsigned int>(
			reinterpret_cast<uintptr_t>(movieObject)),
		stream);
	using PreparePlayerFn = int(__cdecl*)(uint32_t*);
	using StartStreamFn = int(__cdecl*)(uint32_t);
	reinterpret_cast<PreparePlayerFn>(0x000D4470)(movieObject);
	const int result =
		reinterpret_cast<StartStreamFn>(0x000DC010)(stream);
	BetaTrace_Record(
		"CT_MOVIE_START",
		"phase=exit object=%08X stream=%08X result=%d",
		static_cast<unsigned int>(
			reinterpret_cast<uintptr_t>(movieObject)),
		stream,
		result);
	return result;
}

static char __fastcall TaxiPrimaryTimelineTraceHook(
	int* timeline,
	void* /*edx*/,
	int frame)
{
	return TaxiTimelineTraceCall("primary", timeline, frame);
}

static char __fastcall TaxiSecondaryTimelineTraceHook(
	int* timeline,
	void* /*edx*/,
	int frame)
{
	return TaxiTimelineTraceCall("secondary", timeline, frame);
}

static int __cdecl TaxiSecondaryTimelineFrameTraceHook()
{
	BetaTrace_Record(
		"CT_TIMELINE",
		"phase=before-secondary-frame");
	using SecondaryFrameFn = int(__cdecl*)();
	const int frame =
		reinterpret_cast<SecondaryFrameFn>(0x00052270)();
	BetaTrace_Record(
		"CT_TIMELINE",
		"phase=after-secondary-frame frame=%d",
		frame);
	return frame;
}

static void TaxiStateTraceWorker()
{
	struct State {
		uint32_t attractState = 0xFFFFFFFFU;
		uint32_t requestedState = 0xFFFFFFFFU;
		uint32_t movieFrame = 0xFFFFFFFFU;
		uint32_t movieFrameLimit = 0xFFFFFFFFU;
		uint32_t movieWrapper = 0;
		uint32_t transitionFrame = 0xFFFFFFFFU;
		uint32_t course = 0xFFFFFFFFU;
		uint32_t attractIteration = 0xFFFFFFFFU;
		uint32_t gameMode = 0xFFFFFFFFU;
		uint32_t titleMode = 0xFFFFFFFFU;
		uint8_t fadeActive = 0xFF;
		uint8_t movieReset = 0xFF;
		bool readable = false;
	};

	State previous = {};
	DWORD lastHeartbeat = 0;
	const DWORD started = GetTickCount();
	while (GetTickCount() - started < 180000) {
		State current = {};
		__try {
			current.attractState =
				*reinterpret_cast<volatile uint32_t*>(0x0031CC6C);
			current.requestedState =
				*reinterpret_cast<volatile uint32_t*>(0x0031CC68);
			current.movieFrame =
				*reinterpret_cast<volatile uint32_t*>(0x00271720);
			current.movieFrameLimit =
				*reinterpret_cast<volatile uint32_t*>(0x00271724);
			current.movieWrapper =
				*reinterpret_cast<volatile uint32_t*>(0x0027172C);
			current.transitionFrame =
				*reinterpret_cast<volatile uint32_t*>(0x001D9190);
			current.course =
				*reinterpret_cast<volatile uint32_t*>(0x0031CC8C);
			current.attractIteration =
				*reinterpret_cast<volatile uint32_t*>(0x00334E38);
			current.gameMode =
				*reinterpret_cast<volatile uint32_t*>(0x0031C19C);
			current.titleMode =
				*reinterpret_cast<volatile uint32_t*>(0x002717B8);
			current.fadeActive =
				*reinterpret_cast<volatile uint8_t*>(0x001D6530);
			current.movieReset =
				*reinterpret_cast<volatile uint8_t*>(0x001D6680);
			current.readable = true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			current.readable = false;
		}

		const DWORD now = GetTickCount();
		const bool changed =
			current.readable != previous.readable ||
			current.attractState != previous.attractState ||
			current.requestedState != previous.requestedState ||
			current.movieWrapper != previous.movieWrapper ||
			current.transitionFrame != previous.transitionFrame ||
			current.gameMode != previous.gameMode ||
			current.titleMode != previous.titleMode ||
			current.fadeActive != previous.fadeActive ||
			current.movieReset != previous.movieReset;
		if (changed || now - lastHeartbeat >= 1000) {
			BetaTrace_Record(
				"CT_STATE",
				"readable=%u attract=%u requested=%u movie_frame=%u movie_limit=%u movie_wrapper=%08X transition_frame=%u course=%u attract_iteration=%u game_mode=%u title_mode=%u fade=%u movie_reset=%u changed=%u",
				current.readable ? 1U : 0U,
				current.attractState,
				current.requestedState,
				current.movieFrame,
				current.movieFrameLimit,
				current.movieWrapper,
				current.transitionFrame,
				current.course,
				current.attractIteration,
				current.gameMode,
				current.titleMode,
				static_cast<unsigned int>(current.fadeActive),
				static_cast<unsigned int>(current.movieReset),
				changed ? 1U : 0U);
			lastHeartbeat = now;
		}
		previous = current;
		Sleep(16);
	}
}

// CRI async I/O completion spin loop fix.
// The function at ~0xC52D0 spins on a CRI completion flag ([003409C4])
// without ever calling ExecServer, so async I/O never completes.
// Root cause: the tight spin never enters alertable wait, so async I/O
// completion APCs are never delivered to this thread.
static volatile uint32_t* g_CriSpinFlagAddr = nullptr;
static volatile uint32_t* g_CriSpinCounterAddr = nullptr;
static volatile DWORD g_GameThreadId = 0;  // captured from CRI spin helper
static uint8_t* g_CriSpinPatchAddr = nullptr; // Address of CRI spin loop for delayed patching

static volatile DWORD g_SpinEnterTick = 0;   // for diag thread to read
static volatile DWORD g_SpinExitTick = 0;

namespace {

#if defined(_DEBUG)
constexpr size_t kTaxiMovieDumpSlotCount = 7;
constexpr uint32_t kTaxiMovieDumpFrames[kTaxiMovieDumpSlotCount] = {
	1, 30, 90, 120, 180, 300, 450
};

struct TaxiMovieDumpSlot {
	volatile LONG state = 0; // 0=empty, 1=ready, 2=written
	uint32_t frameNumber = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t destination = 0;
	int32_t descriptor[16] = {};
	uint8_t* pixels = nullptr;
	uint32_t byteCount = 0;
};

TaxiMovieDumpSlot g_TaxiMovieDumpSlots[kTaxiMovieDumpSlotCount];
volatile LONG g_TaxiMovieConvertCount = 0;
volatile LONG g_TaxiTexturedQuadPatchedCalls = 0;
volatile LONG g_TaxiTexturedQuadHookCount = 0;
volatile LONG g_TaxiTexturedQuadLastTextureSlot = 0;
volatile LONG g_TaxiTexturedQuadLastTexture = 0;
constexpr LONG kTaxiTexturedQuadSampleCount = 4096;
struct TaxiTexturedQuadSample {
	LONG hookCount = 0;
	LONG textureSlot = 0;
	LONG texture = 0;
	LONG common = 0;
	LONG data = 0;
	LONG format = 0;
};
TaxiTexturedQuadSample
	g_TaxiTexturedQuadSamples[kTaxiTexturedQuadSampleCount];
volatile LONG g_TaxiTexturedQuadSampleWriteCount = 0;
#endif

// Crazy Taxi's LTCG renderer binds textures by emitting NV097 texture methods
// directly from its internal state cache. Those commands update the mirrored
// NV2A registers, but they bypass the normal D3DDevice_SetTexture interception
// that supplies Cxbx's HLE draw path with the active Xbox texture resource.
// The CRI decoder and the game's YUV-to-BGRA conversion both complete normally;
// without this bridge the subsequent movie quad is therefore drawn untextured.
//
// Mirror the actual resource pointer already passed to the game's textured-quad
// helper before allowing its original push-buffer setup and draw to continue.
// This is deliberately title-scoped rather than changing every HLE push-buffer
// user based only on raw OFFSET/FORMAT methods, which do not carry the resource
// Common/Lock fields needed by the D3D9 resource cache.
static thread_local bool g_TaxiMovieTexturedQuadBridgeActive = false;

static int __fastcall TaxiTexturedQuadHook(
	uintptr_t renderer,
	void* /*edx*/,
	uint32_t* vertices,
	int color,
	int* textureSlot)
{
#if defined(_DEBUG)
	const LONG hookCount =
		InterlockedIncrement(&g_TaxiTexturedQuadHookCount);
	InterlockedExchange(
		&g_TaxiTexturedQuadLastTextureSlot,
		static_cast<LONG>(reinterpret_cast<uintptr_t>(textureSlot)));
	InterlockedExchange(
		&g_TaxiTexturedQuadLastTexture,
		textureSlot != nullptr ? static_cast<LONG>(*textureSlot) : 0);
#endif

	// The initial movie-status loop stops polling as soon as the first logo
	// frame is ready. Keep the same throttled CRI service cadence attached to
	// the title's movie draw until that stream reaches its normal stop path.
	if (g_BetaConfig.ct_cri_drive_movie_server) {
		TaxiServiceMovieServers();
	}

#if defined(_DEBUG)
	if (g_BetaConfig.ct_debug_movie_dump) {
		const LONG sampleIndex =
			InterlockedIncrement(&g_TaxiTexturedQuadSampleWriteCount) - 1;
		if (sampleIndex >= 0 &&
			sampleIndex < kTaxiTexturedQuadSampleCount) {
			auto& sample = g_TaxiTexturedQuadSamples[sampleIndex];
			sample.hookCount = hookCount;
			sample.textureSlot =
				static_cast<LONG>(reinterpret_cast<uintptr_t>(textureSlot));
			sample.texture =
				textureSlot != nullptr ? static_cast<LONG>(*textureSlot) : 0;
			if (sample.texture != 0) {
				auto* texture =
					reinterpret_cast<xbox::X_D3DBaseTexture*>(
						static_cast<uintptr_t>(
							static_cast<uint32_t>(sample.texture)));
				__try {
					sample.common = static_cast<LONG>(texture->Common);
					sample.data = static_cast<LONG>(texture->Data);
					sample.format = static_cast<LONG>(texture->Format);
				}
				__except (EXCEPTION_EXECUTE_HANDLER) {
					sample.common = 0;
					sample.data = 0;
					sample.format = 0;
				}
			}
			MemoryBarrier();
		}
	}
#endif

	const bool useMovieTexture =
		textureSlot != nullptr &&
		*textureSlot != 0 &&
		CxbxHasRecentCrazyTaxiDecodedMovieFrame();
	if (useMovieTexture) {
		g_pXbox_SetTexture[0] =
			reinterpret_cast<xbox::X_D3DBaseTexture*>(*textureSlot);
		CxbxSetCrazyTaxiPendingTexturedQuadTexture(
			g_pXbox_SetTexture[0]);
	}
	else {
		CxbxSetCrazyTaxiPendingTexturedQuadTexture(nullptr);
	}

	using TexturedQuadFn =
		int (__fastcall *)(uintptr_t, void*, uint32_t*, int, int*);
	const auto original =
		reinterpret_cast<TexturedQuadFn>(0x000BD7E0);
	const bool previousBridgeState =
		g_TaxiMovieTexturedQuadBridgeActive;
	g_TaxiMovieTexturedQuadBridgeActive = useMovieTexture;
	const int result =
		original(renderer, nullptr, vertices, color, textureSlot);
	g_TaxiMovieTexturedQuadBridgeActive = previousBridgeState;
	if (!previousBridgeState) {
		CxbxSetCrazyTaxiPendingTexturedQuadTexture(nullptr);
	}
	return result;
}

// sub_BD7E0 calls the title's cached-state binder before it emits the quad's
// push buffer. The cached binder knows the actual X_D3DTexture object, but its
// LTCG path bypasses Cxbx's D3DDevice_SetTexture interception. Mirroring the
// pointer in the outer sub_BD7E0 wrapper is too early: the cached-state binder
// subsequently replaces the HLE stage with its stale value, leaving the movie
// draw untextured. Hook the single internal call instead and mirror both stages
// immediately after the guest binder has completed.
static int __fastcall TaxiBindTexturedQuadStateHook(
	int* renderer,
	void* /*edx*/,
	int viewportWidth,
	int viewportHeight,
	int viewportDepth,
	char useViewport,
	int* texture0Slot,
	int* texture1Slot)
{
	using BindStateFn = int (__thiscall *)(
		int*, int, int, int, char, int*, int*);
	const auto original =
		reinterpret_cast<BindStateFn>(0x000BCE90);
	const int result = original(
		renderer,
		viewportWidth,
		viewportHeight,
		viewportDepth,
		useViewport,
		texture0Slot,
		texture1Slot);

	if (g_TaxiMovieTexturedQuadBridgeActive &&
		texture0Slot != nullptr &&
		*texture0Slot != 0 &&
		CxbxHasRecentCrazyTaxiDecodedMovieFrame()) {
		g_pXbox_SetTexture[0] =
			reinterpret_cast<xbox::X_D3DBaseTexture*>(*texture0Slot);
		CxbxSetCrazyTaxiPendingTexturedQuadTexture(
			g_pXbox_SetTexture[0]);
	}
	else {
		CxbxSetCrazyTaxiPendingTexturedQuadTexture(nullptr);
	}
	if (texture1Slot != nullptr && *texture1Slot != 0) {
		g_pXbox_SetTexture[1] =
			reinterpret_cast<xbox::X_D3DBaseTexture*>(*texture1Slot);
	}
	return result;
}

#if defined(_DEBUG)
void TaxiMovieDumpThread()
{
	CreateDirectoryA("C:\\temp", nullptr);
	FILE* metadata = fopen("C:\\temp\\taxi_movie_frames.txt", "w");
	DWORD idleStart = GetTickCount();
	LONG lastTexturedQuadCount = -1;
	LONG texturedQuadSampleReadCount = 0;

	for (;;) {
		const LONG texturedQuadCount = InterlockedCompareExchange(
			&g_TaxiTexturedQuadHookCount, 0, 0);
		if (metadata != nullptr &&
			texturedQuadCount != lastTexturedQuadCount) {
			fprintf(
				metadata,
				"quad patched_calls=%ld hook_count=%ld slot=0x%08X texture=0x%08X bridge_sets=%ld pending_checks=%ld matches=%ld bridge_texture=0x%08X last_shader=0x%08X last_primitive=%ld last_vertices=%ld\n",
				InterlockedCompareExchange(
					&g_TaxiTexturedQuadPatchedCalls, 0, 0),
				texturedQuadCount,
				static_cast<uint32_t>(InterlockedCompareExchange(
					&g_TaxiTexturedQuadLastTextureSlot, 0, 0)),
				static_cast<uint32_t>(InterlockedCompareExchange(
					&g_TaxiTexturedQuadLastTexture, 0, 0)),
				InterlockedCompareExchange(
					&g_CrazyTaxiTextureBridgeSetCount, 0, 0),
				InterlockedCompareExchange(
					&g_CrazyTaxiTextureBridgePendingChecks, 0, 0),
				InterlockedCompareExchange(
					&g_CrazyTaxiTextureBridgeMatchCount, 0, 0),
				static_cast<uint32_t>(InterlockedCompareExchange(
					&g_CrazyTaxiTextureBridgeLastTexture, 0, 0)),
				static_cast<uint32_t>(InterlockedCompareExchange(
					&g_CrazyTaxiTextureBridgeLastShader, 0, 0)),
				InterlockedCompareExchange(
					&g_CrazyTaxiTextureBridgeLastPrimitiveType, 0, 0),
				InterlockedCompareExchange(
					&g_CrazyTaxiTextureBridgeLastVertexCount, 0, 0));
			fflush(metadata);
			lastTexturedQuadCount = texturedQuadCount;
		}

		const LONG texturedQuadSampleWriteCount = std::min(
			InterlockedCompareExchange(
				&g_TaxiTexturedQuadSampleWriteCount, 0, 0),
			kTaxiTexturedQuadSampleCount);
		while (metadata != nullptr &&
			texturedQuadSampleReadCount < texturedQuadSampleWriteCount) {
			const auto& sample =
				g_TaxiTexturedQuadSamples[texturedQuadSampleReadCount++];
			fprintf(
				metadata,
				"quad_sample hook=%ld slot=0x%08X texture=0x%08X common=0x%08X data=0x%08X format=0x%08X\n",
				sample.hookCount,
				static_cast<uint32_t>(sample.textureSlot),
				static_cast<uint32_t>(sample.texture),
				static_cast<uint32_t>(sample.common),
				static_cast<uint32_t>(sample.data),
				static_cast<uint32_t>(sample.format));
		}
		if (metadata != nullptr &&
			texturedQuadSampleReadCount < texturedQuadSampleWriteCount) {
			fflush(metadata);
		}

		bool pending = false;
		bool wroteFrame = false;
		for (size_t slotIndex = 0;
			slotIndex < kTaxiMovieDumpSlotCount;
			++slotIndex) {
			auto& slot = g_TaxiMovieDumpSlots[slotIndex];
			const LONG state =
				InterlockedCompareExchange(&slot.state, 0, 0);
			if (state == 0) {
				pending = true;
				continue;
			}
			if (state != 1) {
				continue;
			}

			char path[MAX_PATH] = {};
			snprintf(
				path,
				sizeof(path),
				"C:\\temp\\taxi_movie_frame_%04u_%ux%u.bgra",
				slot.frameNumber,
				slot.width,
				slot.height);
			if (slot.pixels != nullptr && slot.byteCount != 0) {
				FILE* frameFile = fopen(path, "wb");
				if (frameFile != nullptr) {
					fwrite(slot.pixels, 1, slot.byteCount, frameFile);
					fclose(frameFile);
				}
			}

			if (metadata != nullptr) {
				fprintf(
					metadata,
					"frame=%u dst=0x%08X size=%ux%u bytes=%u desc=",
					slot.frameNumber,
					slot.destination,
					slot.width,
					slot.height,
					slot.byteCount);
				for (size_t descriptorIndex = 0;
					descriptorIndex < 16;
					++descriptorIndex) {
					fprintf(
						metadata,
						"%s%08X",
						descriptorIndex == 0 ? "" : ",",
						static_cast<uint32_t>(
							slot.descriptor[descriptorIndex]));
				}
				fputc('\n', metadata);
				fflush(metadata);
			}

			if (slot.pixels != nullptr) {
				HeapFree(GetProcessHeap(), 0, slot.pixels);
			}
			slot.pixels = nullptr;
			InterlockedExchange(&slot.state, 2);
			wroteFrame = true;
		}

		if (wroteFrame) {
			idleStart = GetTickCount();
		}
		if (!pending || GetTickCount() - idleStart >= 30000) {
			break;
		}
		Sleep(10);
	}

	if (metadata != nullptr) {
		fclose(metadata);
	}
}
#endif

void* __cdecl TaxiMovieConvertHook(int player, int* frame, int destination)
{
	using MovieConvertFn = void* (__cdecl *)(int, int*, int);
	const auto original =
		reinterpret_cast<MovieConvertFn>(0x000D6560);
	void* result = original(player, frame, destination);

	if (frame == nullptr || destination == 0) {
		return result;
	}

	const uint32_t width = static_cast<uint32_t>(frame[2]);
	const uint32_t height = static_cast<uint32_t>(frame[3]);
	if (frame[0] != 0 &&
		width >= 16 && width <= 1920 &&
		height >= 16 && height <= 1080) {
		__try {
			CxbxSetCrazyTaxiDecodedMovieFrame(
				reinterpret_cast<const void*>(destination),
				width,
				height);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			// A malformed or transient CRI destination must not interrupt
			// the title's normal movie state machine.
		}
	}

#if defined(_DEBUG)
	if (!g_BetaConfig.ct_debug_movie_dump) {
		return result;
	}

	const uint32_t frameNumber = static_cast<uint32_t>(
		InterlockedIncrement(&g_TaxiMovieConvertCount));
	size_t slotIndex = kTaxiMovieDumpSlotCount;
	for (size_t index = 0; index < kTaxiMovieDumpSlotCount; ++index) {
		if (kTaxiMovieDumpFrames[index] == frameNumber) {
			slotIndex = index;
			break;
		}
	}
	if (slotIndex == kTaxiMovieDumpSlotCount) {
		return result;
	}

	auto& slot = g_TaxiMovieDumpSlots[slotIndex];
	slot.frameNumber = frameNumber;
	slot.width = width;
	slot.height = height;
	slot.destination = static_cast<uint32_t>(destination);
	memcpy(slot.descriptor, frame, sizeof(slot.descriptor));
	if (width < 16 || width > 1920 || height < 16 || height > 1080) {
		MemoryBarrier();
		InterlockedExchange(&slot.state, 1);
		return result;
	}

	const uint64_t byteCount64 =
		static_cast<uint64_t>(width) * height * sizeof(uint32_t);
	if (byteCount64 > 16 * 1024 * 1024) {
		return result;
	}
	const uint32_t byteCount = static_cast<uint32_t>(byteCount64);
	auto* copy = static_cast<uint8_t*>(
		HeapAlloc(GetProcessHeap(), 0, byteCount));
	if (copy == nullptr) {
		return result;
	}

	bool copied = false;
	__try {
		memcpy(copy, reinterpret_cast<const void*>(destination), byteCount);
		copied = true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		copied = false;
	}
	if (!copied) {
		HeapFree(GetProcessHeap(), 0, copy);
		return result;
	}

	slot.pixels = copy;
	slot.byteCount = byteCount;
	MemoryBarrier();
	InterlockedExchange(&slot.state, 1);
#endif
	return result;
}

} // namespace

// Crazy Taxi's four-byte gameplay timer. Cheat Engine reports this as
// cxbxr-ldr.exe+30DC6C; the loader image base is 0x00010000, resolving to the
// directly mapped guest address 0x0031DC6C. The title rewrites it when a level
// begins, so reproduce a value freeze instead of applying a one-time write.
static void TaxiTimerLockThread()
{
	constexpr uintptr_t kTimerAddress = 0x0031DC6C;

	for (;;) {
		__try {
			*reinterpret_cast<volatile uint32_t*>(kTimerAddress) =
				g_BetaConfig.ct_timer_lock_value;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			// The guest page can be transiently unavailable during title
			// startup. Keep waiting; the process itself owns this thread and
			// terminates it when emulation exits.
		}

		Sleep(50);
	}
}

// Renderer investigation helper. The game keeps the course carousel selection
// in two fields of its main state object and copies the accepted value into a
// global before loading the course. Holding all three values lets a diagnostic
// run enter one exact course repeatedly without changing the normal input path.
static void TaxiCourseLockThread()
{
	constexpr uintptr_t kMainStatePointerAddress = 0x003252C0;
	constexpr uintptr_t kSelectedCourseAddress = 0x0031CC8C;
	constexpr uintptr_t kCarouselCourseOffset = 824;
	constexpr uintptr_t kConfirmedCourseOffset = 872;
	constexpr uintptr_t kAcceptedCourseOffset = 880;
	const uint32_t course =
		static_cast<uint32_t>(g_BetaConfig.ct_debug_course);

	// The carousel fields are reused by later gameplay states. Leaving this
	// diagnostic writer alive for the entire process can therefore hold the
	// title in a course transition or corrupt an already loaded run. Keep
	// those fields fixed only until the title copies the carousel value into
	// its own confirmed-course slot. The global course is then held for a
	// short loader handoff, after which the game owns all state again.
	bool courseConfirmed = false;
	DWORD confirmationTick = 0;
	for (;;) {
		__try {
			*reinterpret_cast<volatile uint32_t*>(kSelectedCourseAddress) =
				course;
			const uintptr_t mainState =
				*reinterpret_cast<volatile uint32_t*>(
					kMainStatePointerAddress);
			if (mainState != 0) {
				if (!courseConfirmed) {
					*reinterpret_cast<volatile uint32_t*>(
						mainState + kCarouselCourseOffset) = course;
					*reinterpret_cast<volatile uint32_t*>(
						mainState + kAcceptedCourseOffset) = course;
					if (*reinterpret_cast<volatile uint32_t*>(
							mainState + kConfirmedCourseOffset) ==
						course) {
						courseConfirmed = true;
						confirmationTick = GetTickCount();
					}
				}
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			// Guest state is not mapped during early title startup.
		}

		if (courseConfirmed &&
			GetTickCount() - confirmationTick >= 5000) {
			break;
		}
		Sleep(16);
	}
}

// Crazy Taxi constructs one X_D3DLIGHT_SPOT every frame, but the Chihiro
// executable never adds it to the renderer's eight-light list. The original
// object is a fixed overhead course light and cannot account for the arcade
// cabinet's always-on car headlights. Reuse that otherwise dormant object as
// one broad, camera-following headlight cone immediately before the game
// commits its light list. The chase camera target tracks the player's taxi,
// while the vector from camera to target tracks its forward direction closely
// enough to keep the cone attached during steering.
static int __fastcall TaxiCommitLightsWithHeadlightHook(
	void* renderer,
	void* /*edx*/)
{
	using RegisterLightFn = int(__thiscall*)(void*, char*);
	using CommitLightsFn = int(__thiscall*)(void*);

	const auto registerLight =
		reinterpret_cast<RegisterLightFn>(0x000C0140);
	const auto commitLights =
		reinterpret_cast<CommitLightsFn>(0x000B8660);

	if (g_BetaConfig.ct_headlights) {
		__try {
			const auto gameMode =
				*reinterpret_cast<volatile const uint32_t*>(0x0031CC6C);
			if (gameMode == 3) {
				auto* light =
					reinterpret_cast<volatile float*>(0x00324328);
				const auto* camera =
					reinterpret_cast<volatile const float*>(0x00278DB0);
				const auto* target =
					reinterpret_cast<volatile const float*>(0x00278D60);

				float forwardX = target[0] - camera[0];
				float forwardZ = target[2] - camera[2];
				const float horizontalLength =
					std::sqrt(
						forwardX * forwardX +
						forwardZ * forwardZ);
				if (std::isfinite(horizontalLength) &&
					horizontalLength > 0.001f) {
					forwardX /= horizontalLength;
					forwardZ /= horizontalLength;

					// Put the source just ahead of and above the camera
					// target, then aim it slightly down toward the road.
					constexpr float kForwardOffset = 8.0f;
					constexpr float kHeightOffset = 3.0f;
					constexpr float kDownwardPitch = -0.08f;
					constexpr float kRange = 900.0f;
					constexpr float kHalfRange = kRange * 0.5f;
					const float directionScale =
						1.0f /
						std::sqrt(
							1.0f +
							kDownwardPitch * kDownwardPitch);
					const float directionX =
						forwardX * directionScale;
					const float directionY =
						kDownwardPitch * directionScale;
					const float directionZ =
						forwardZ * directionScale;
					const float positionX =
						target[0] + forwardX * kForwardOffset;
					const float positionY =
						target[1] + kHeightOffset;
					const float positionZ =
						target[2] + forwardZ * kForwardOffset;

					// X_D3DLIGHT8 layout after Type and three colours.
					*reinterpret_cast<volatile uint32_t*>(light) =
						g_BetaConfig.ct_headlights == 2 ? 1u : 2u;
					light[13] = positionX;
					light[14] = positionY;
					light[15] = positionZ;
					light[16] = directionX;
					light[17] = directionY;
					light[18] = directionZ;
					light[19] = kRange;
					// D3DLIGHT8 stores Falloff before the three
					// attenuation coefficients. Keeping Attenuation0 at
					// one is essential: placing Falloff in that slot made
					// Attenuation2 equal one, which extinguished the cone
					// almost immediately and destabilized the point-light
					// diagnostic.
					light[20] = 1.0f;       // Falloff
					light[21] = 1.0f;       // Attenuation0
					light[22] = 0.0015f;    // Attenuation1
					light[23] = 0.000008f;  // Attenuation2
					light[24] = 0.45f;
					light[25] = 0.90f;

					// The game's registration helper uses this sphere to
					// sort local lights before adding them to the list.
					light[26] =
						positionX + directionX * kHalfRange;
					light[27] =
						positionY + directionY * kHalfRange;
					light[28] =
						positionZ + directionZ * kHalfRange;
					light[29] = kHalfRange;

					registerLight(
						reinterpret_cast<void*>(0x00324328),
						static_cast<char*>(renderer));
				}
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			// If this executable revision does not expose the expected
			// camera/light globals, retain the title's normal light commit.
		}
	}

	return commitLights(renderer);
}

// The cabinet backup block persists the current coin balance. That is correct
// for real hardware, but it means an emulator session can start with credits
// left over from an earlier run. Crazy Taxi intentionally cancels its attract
// movie as soon as any credit is available, which made ct_skip_movies=0 appear
// ineffective when the saved balance was non-zero.
//
// Keep the title's original credit/config initialization and reset only the
// loaded per-session balance afterwards. New coin edges still travel through
// the normal JVS path and are counted immediately.
static int __stdcall TaxiCreditInitHook(
	const void* settings,
	const void* savedCredits,
	const void* savedTotals)
{
	using CreditInitFn =
		int (__stdcall *)(const void*, const void*, const void*);
	const auto original =
		reinterpret_cast<CreditInitFn>(0x000F7B20);
	const int result = original(settings, savedCredits, savedTotals);

	const uint8_t playerCount =
		*reinterpret_cast<volatile uint8_t*>(0x0039DAAA);
	auto* creditState =
		reinterpret_cast<volatile uint32_t*>(0x0039DAC8);
	for (uint8_t player = 0; player < playerCount && player < 4; ++player) {
		creditState[0] = 0; // Whole credits.
		creditState[1] = 0; // Partial-coin accumulator.
		creditState[3] = 0; // Per-session coin limiter/counter.
		creditState += 4;
	}

	return result;
}

static void __cdecl TaxiCriAsyncSpinHelper() {
	if (!g_CriSpinFlagAddr) return;

	// NtReadFile queues its completion APC to this same native thread, but
	// Crazy Taxi's original code only busy-spins on CompFlag. Give Windows an
	// alertable boundary so the real completion callback can supply the data.
	g_GameThreadId = GetCurrentThreadId();
	g_SpinEnterTick = GetTickCount();

	const DWORD start = GetTickCount();
	while (*g_CriSpinFlagAddr == 0) {
		SleepEx(1, TRUE);
		if (g_BetaConfig.ct_cri_wait_timeout_ms != 0 &&
			GetTickCount() - start >=
				g_BetaConfig.ct_cri_wait_timeout_ms) {
			InterlockedExchange(
				reinterpret_cast<volatile LONG*>(g_CriSpinFlagAddr),
				1);
			break;
		}
	}

	g_SpinExitTick = GetTickCount();
}

// Crazy Taxi normally completes this CRI request asynchronously, but its
// title-local worker can remain in a tight GM=7 completion spin forever when
// the emulated completion APC is not dispatched. Do not force all CRI requests
// complete at startup: that suppresses valid Sofdec and streaming work.
// Instead, watch this one verified wait and release it only after the spin
// counter has continued advancing for the configured timeout.
static void TaxiCriCompletionRecoveryThread()
{
	DWORD stuckStartTick = 0;
	uint32_t lastSpinCounter = 0;
	bool observedSpinAdvance = false;

	for (;;) {
		__try {
			const uint32_t gameMode =
				*reinterpret_cast<volatile uint32_t*>(0x0031CC6C);
			const uint32_t completionFlag = *g_CriSpinFlagAddr;
			const uint32_t spinCounter = *g_CriSpinCounterAddr;

			if (gameMode == 7 && completionFlag == 0) {
				const DWORD now = GetTickCount();
				if (stuckStartTick == 0) {
					stuckStartTick = now;
					lastSpinCounter = spinCounter;
					observedSpinAdvance = false;
				}
				else {
					if (spinCounter != lastSpinCounter) {
						lastSpinCounter = spinCounter;
						observedSpinAdvance = true;
					}
					if (observedSpinAdvance &&
						now - stuckStartTick >=
							g_BetaConfig.ct_cri_wait_timeout_ms) {
						InterlockedExchange(
							reinterpret_cast<volatile LONG*>(
								g_CriSpinFlagAddr),
							1);
						stuckStartTick = 0;
						observedSpinAdvance = false;
					}
				}
			}
			else {
				stuckStartTick = 0;
				observedSpinAdvance = false;
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			stuckStartTick = 0;
			observedSpinAdvance = false;
		}

		Sleep(10);
	}
}

// Replace the non-alertable guest spin with the helper above, then resume at
// the original function's completion block.
static void ApplyCriAsyncSpinPatch(FILE* logFile) {
	if (!g_CriSpinPatchAddr) return;
	uint8_t* funcStart = g_CriSpinPatchAddr;
	DWORD oldProtect;
	const int kPatchLen = 36;
	if (VirtualProtect(funcStart, kPatchLen, PAGE_EXECUTE_READWRITE, &oldProtect)) {
		funcStart[0] = 0xE8;
		*reinterpret_cast<int32_t*>(funcStart + 1) =
			static_cast<int32_t>(
				reinterpret_cast<uintptr_t>(&TaxiCriAsyncSpinHelper) -
				(reinterpret_cast<uintptr_t>(funcStart) + 5));
		funcStart[5] = 0xEB;
		funcStart[6] = static_cast<uint8_t>(kPatchLen - 7);
		for (int i = 7; i < kPatchLen; ++i) {
			funcStart[i] = 0x90;
		}
		VirtualProtect(funcStart, kPatchLen, oldProtect, &oldProtect);
		FlushInstructionCache(GetCurrentProcess(), funcStart, kPatchLen);
		if (logFile) {
			fprintf(
				logFile,
				"[CRI-PATCH] Applied alertable CRI wait @0x%08X "
				"(flag=0x%08X timeout=%u ms)\n",
				static_cast<unsigned>(reinterpret_cast<uintptr_t>(funcStart)),
				static_cast<unsigned>(
					reinterpret_cast<uintptr_t>(g_CriSpinFlagAddr)),
				static_cast<unsigned>(
					g_BetaConfig.ct_cri_wait_timeout_ms));
			fflush(logFile);
		}
	}
	g_CriSpinPatchAddr = nullptr; // only apply once
}

#if defined(_DEBUG)
// Minimal diagnostic: periodically write game mode to file for boot verification
static void TaxiDiagThread() {
	FILE* f = fopen("C:\\temp\\taxi_diag.txt", "w");
	if (!f) return;
	DWORD start = GetTickCount();
	uint32_t lastGm = 0xFFFFFFFF;
	// Fast sampling for first 30s to catch transient game modes, then slow
	for (int n = 0; n < 600; n++) {
		DWORD elapsed = GetTickCount() - start;
		Sleep(elapsed < 30000 ? 200 : 5000);
		__try {
			uint32_t gm = *(volatile uint32_t*)0x31CC6C;
			uint32_t cnt = *(volatile uint32_t*)0x271724;
			uint32_t mb = *(volatile uint32_t*)0x39D5A8;
			if (gm != lastGm || elapsed >= 30000) {
				fprintf(f, "[DIAG] t=%ds GM=%u cnt=%u MB=%u\n", (int)(elapsed/1000), gm, cnt, mb);
				fflush(f);
				lastGm = gm;
			}
		} __except(EXCEPTION_EXECUTE_HANDLER) {}
		if (GetTickCount() - start > 120000) break; // 2 min max
	}
	fclose(f);
}
#endif

void ApplyCrazyTaxiPatches(uint64_t xbeHash, uint32_t imageSize)
{
	TAXI_DEBUG_PRINTF("CrazyTaxiPatch: xbeHash=0x%016llX, match=%d\n", (unsigned long long)xbeHash, (int)IsCrazyTaxiXbe(xbeHash));

	if (!IsCrazyTaxiXbe(xbeHash)) return;

	// NOTE: D3D BetaConfig overrides for Crazy Taxi are applied EARLY in CxbxKrnl.cpp
	// (right after BetaConfig_Load, before CxbxInitWindow) because D3D device creation
	// caches these values. Overriding here in ApplyCrazyTaxiPatches is too late.
	// See CxbxKrnlApplyGameSpecificBetaOverrides().

	FILE* plog = nullptr;
#if defined(_DEBUG)
	plog = fopen("C:\\temp\\taxi_patches.txt", "w");
#endif

#if defined(_DEBUG)
	EmuLog(LOG_LEVEL::INFO, "CrazyTaxiPatch: applying patches (hash 0x%016llX, imageSize=0x%X)", (unsigned long long)xbeHash, imageSize);
#endif
	if (plog) { fprintf(plog, "CrazyTaxiPatch: hash=0x%016llX imageSize=0x%X\n", (unsigned long long)xbeHash, imageSize); fflush(plog); }

	// ============== ACTIVE PATCHES ==============

	if (g_BetaConfig.scheduler_io_trace &&
		InterlockedCompareExchange(
			&g_TaxiStateTraceStarted,
			1,
			0) == 0) {
		std::thread(TaxiStateTraceWorker).detach();
	}

	if (g_BetaConfig.scheduler_io_trace) {
		struct ModuleMethodPatch {
			uintptr_t address;
			uint32_t expected;
			const void* replacement;
		};
		const ModuleMethodPatch moduleMethods[] = {
			{
				0x001D3A2C,
				0x000DA6F0,
				reinterpret_cast<const void*>(
					&TaxiVideoDecoderModuleTraceHook)
			},
			{
				0x001D39A0,
				0x000D78B0,
				reinterpret_cast<const void*>(
					&TaxiAudioDecoderModuleTraceHook)
			},
		};
		for (const auto& method : moduleMethods) {
			if (*reinterpret_cast<const uint32_t*>(method.address) !=
				method.expected) {
				continue;
			}
			const uint32_t replacement =
				static_cast<uint32_t>(
					reinterpret_cast<uintptr_t>(
						method.replacement));
			PatchXbeBytes(
				method.address,
				reinterpret_cast<const uint8_t*>(&replacement),
				sizeof(replacement));
		}
	}

	// Crazy Taxi's cabinet reports wheel, gas and brake on native JVS channels
	// 0, 1 and 2. Keep TPUI's shared-page semantics (gas, wheel, unused,
	// brake) and select the title-specific mapping in JvsIo::Update.
	g_jvs_game_type = JvsGameType::CrazyTaxi;

	// The title requests an immediate presentation interval on this path. On
	// 120 Hz hosts that makes its frame-counted menus and physics run too fast,
	// and a single control edge can span several states. Reuse Cxbx's existing
	// 60 Hz limiter by overriding only this title's presentation interval.
	extern xbox::dword_xt g_Xbox_PresentationInterval_Override;
	g_Xbox_PresentationInterval_Override = 1; // D3DPRESENT_INTERVAL_ONE

	// The title's per-frame light builder leaves its only spotlight out of the
	// renderer list. Hook the final light commit and add a title-local,
	// player-following headlight cone. Validate the exact original call target
	// before patching so alternate executable revisions remain untouched.
	{
		constexpr uintptr_t kCommitLightsCall = 0x00025CBD;
		auto* call =
			reinterpret_cast<const uint8_t*>(kCommitLightsCall);
		if (call[0] == 0xE8) {
			const int32_t displacement =
				*reinterpret_cast<const int32_t*>(call + 1);
			const uintptr_t target =
				kCommitLightsCall + 5 + displacement;
			if (target == 0x000B8660) {
				PatchWithCall(
					kCommitLightsCall,
					&TaxiCommitLightsWithHeadlightHook,
					5);
				TAXI_DEBUG_PRINTF(
					"CrazyTaxiPatch: gameplay headlight hook @0x%08X\n",
					static_cast<unsigned>(kCommitLightsCall));
			}
		}
	}

	// The game's textured-quad helper is reached from four call sites in this
	// executable. Only the first three callers cover the boot/attract Sofdec
	// paths. The fourth is an unrelated in-game composite; bridging a recently
	// decoded frame there can surface as a transient white rectangle.
	// Validate every rel32 target before touching it so an alternate executable
	// revision cannot receive a call with an incompatible ABI.
	static constexpr uintptr_t kTexturedQuadCalls[] = {
		0x00011BEF,
		0x00011FFB,
		0x0007E8C2,
	};
	for (const uintptr_t callVA : kTexturedQuadCalls) {
		auto* call = reinterpret_cast<const uint8_t*>(callVA);
		if (call[0] != 0xE8) {
			continue;
		}
		const int32_t displacement =
			*reinterpret_cast<const int32_t*>(call + 1);
		const uintptr_t target = callVA + 5 + displacement;
		if (target == 0x000BD7E0) {
			PatchWithCall(callVA, &TaxiTexturedQuadHook, 5);
#if defined(_DEBUG)
			InterlockedIncrement(&g_TaxiTexturedQuadPatchedCalls);
#endif
			TAXI_DEBUG_PRINTF(
				"CrazyTaxiPatch: textured-quad texture bridge @0x%08X\n",
				static_cast<unsigned>(callVA));
		}
	}

	// The title reaches sub_3CA20 through four direct movie-status calls. Route
	// each through the same ABI-compatible hook so every Sofdec scene can drive
	// the priority-4 decoder server when the opt-in workaround is enabled.
	if (g_BetaConfig.ct_cri_drive_movie_server ||
		g_BetaConfig.scheduler_io_trace) {
		static constexpr uintptr_t kMovieStatusCalls[] = {
			0x00011AF2,
			0x00011B72,
			0x0007E7E9,
			0x0007E805,
		};
		for (const uintptr_t callVA : kMovieStatusCalls) {
			auto* call = reinterpret_cast<const uint8_t*>(callVA);
			if (call[0] != 0xE8) {
				continue;
			}
			const int32_t displacement =
				*reinterpret_cast<const int32_t*>(call + 1);
			const uintptr_t target = callVA + 5 + displacement;
			if (target == 0x0003CA20) {
				PatchWithCall(
					callVA,
					&TaxiMovieStatusWithServiceHook,
					5);
				TAXI_DEBUG_PRINTF(
					"CrazyTaxiPatch: movie priority-4 service hook @0x%08X\n",
					static_cast<unsigned>(callVA));
			}
		}
	}

	// Trace the natural boot-movie teardown separately from skip/cancel paths.
	// Keep the original title call completely untouched outside diagnostics:
	// this wrapper invokes internal x86 routines whose calling conventions are
	// not fully described by the decompiler.
	if (g_BetaConfig.scheduler_io_trace) {
		constexpr uintptr_t kBootMovieStopCall = 0x00011B08;
		auto* call =
			reinterpret_cast<const uint8_t*>(kBootMovieStopCall);
		if (call[0] == 0xE8) {
			const int32_t displacement =
				*reinterpret_cast<const int32_t*>(call + 1);
			const uintptr_t target =
				kBootMovieStopCall + 5 + displacement;
			if (target == 0x0003C950) {
				PatchWithCall(
					kBootMovieStopCall,
					&TaxiMovieStopTraceHook,
					5);
			}
		}
	}

	// Preserve sub_D5760 but replace its first direct call to sub_D42F0 with
	// the bounded equivalent above while the Android movie-server workaround
	// is active. The same route is required by sub_D4750's pre-start reset:
	// after the first Android recovery, the ADX worker can no longer
	// acknowledge either teardown on its own. Trace mode also keeps this path
	// available for scheduler diagnostics.
	if (g_BetaConfig.scheduler_io_trace ||
		g_BetaConfig.ct_cri_drive_movie_server) {
		static constexpr uintptr_t kMovieCoreStopCalls[] = {
			0x000D4391,
			0x000D4653,
			0x000D4704,
			0x000D4772,
			0x000D576E,
		};
		for (const uintptr_t callVA : kMovieCoreStopCalls) {
			auto* call =
				reinterpret_cast<const uint8_t*>(callVA);
			if (call[0] == 0xE8) {
				const int32_t displacement =
					*reinterpret_cast<const int32_t*>(call + 1);
				const uintptr_t target =
					callVA + 5 + displacement;
				if (target == 0x000D42F0) {
					PatchWithCall(
						callVA,
						&TaxiMovieCoreStopTraceHook,
						5);
				}
			}
		}
	}

	if (g_BetaConfig.scheduler_io_trace) {
		struct TimelineCallPatch {
			uintptr_t address;
			uintptr_t expectedTarget;
			const void* replacement;
		};
		const TimelineCallPatch timelineCalls[] = {
			{
				0x000126DA,
				0x0007F310,
				reinterpret_cast<const void*>(
					&TaxiPrimaryTimelineTraceHook)
			},
			{
				0x000126DF,
				0x00052270,
				reinterpret_cast<const void*>(
					&TaxiSecondaryTimelineFrameTraceHook)
			},
			{
				0x000126EB,
				0x0007F310,
				reinterpret_cast<const void*>(
					&TaxiSecondaryTimelineTraceHook)
			},
		};
		for (const auto& patch : timelineCalls) {
			auto* call =
				reinterpret_cast<const uint8_t*>(patch.address);
			if (call[0] != 0xE8) {
				continue;
			}
			const int32_t displacement =
				*reinterpret_cast<const int32_t*>(call + 1);
			const uintptr_t target =
				patch.address + 5 + displacement;
			if (target == patch.expectedTarget) {
				PatchWithCall(
					patch.address,
					patch.replacement,
					5);
			}
		}
	}

	if (g_BetaConfig.scheduler_io_trace ||
		g_BetaConfig.ct_cri_drive_movie_server) {
		constexpr uintptr_t kMovieAfsStartCall = 0x0003C9A6;
		auto* call =
			reinterpret_cast<const uint8_t*>(kMovieAfsStartCall);
		if (call[0] == 0xE8) {
			const int32_t displacement =
				*reinterpret_cast<const int32_t*>(call + 1);
			const uintptr_t target =
				kMovieAfsStartCall + 5 + displacement;
			if (target == 0x000D47B0) {
				PatchWithCall(
					kMovieAfsStartCall,
					g_BetaConfig.scheduler_io_trace
						? reinterpret_cast<const void*>(
							&TaxiMovieAfsStartTraceHook)
						: reinterpret_cast<const void*>(
							&TaxiMovieAfsStartStateHook),
					5);
			}
		}
	}

	// Place the texture bridge after sub_BD7E0's internal cached-state binder,
	// not only at the outer helper entry. This is the last texture-state update
	// before the helper emits its draw push buffer.
	{
		constexpr uintptr_t kBindStateCall = 0x000BD80A;
		auto* call = reinterpret_cast<const uint8_t*>(kBindStateCall);
		if (call[0] == 0xE8) {
			const int32_t displacement =
				*reinterpret_cast<const int32_t*>(call + 1);
			const uintptr_t target =
				kBindStateCall + 5 + displacement;
			if (target == 0x000BCE90) {
				PatchWithCall(
					kBindStateCall,
					&TaxiBindTexturedQuadStateHook,
					5);
				TAXI_DEBUG_PRINTF(
					"CrazyTaxiPatch: post-bind texture bridge @0x%08X\n",
					static_cast<unsigned>(kBindStateCall));
			}
		}
	}

	{
		auto* movieConvertCall = reinterpret_cast<uint8_t*>(0x0003CD09);
		const int32_t originalDisplacement =
			*reinterpret_cast<const int32_t*>(movieConvertCall + 1);
		const uintptr_t originalTarget =
			reinterpret_cast<uintptr_t>(movieConvertCall) +
			5 +
			originalDisplacement;
		if (movieConvertCall[0] == 0xE8 &&
			originalTarget == 0x000D6560) {
#if defined(_DEBUG)
			if (g_BetaConfig.ct_debug_movie_dump) {
				std::thread(TaxiMovieDumpThread).detach();
			}
#endif
			PatchWithCall(
				reinterpret_cast<uintptr_t>(movieConvertCall),
				&TaxiMovieConvertHook,
				5);
			TAXI_DEBUG_PRINTF(
				"CrazyTaxiPatch: movie frame dump hook @0x0003CD09\n");
		}
	}

	// Reset credits restored from the cabinet backup block after the original
	// initializer has validated and copied them. Otherwise any saved balance
	// skips the attract movie before its first decoded frame is presented.
	{
		static const uint8_t kCreditInitCallPattern[] = {
			0x68, 0xE8, 0xD6, 0x39, 0x00, // push 0x0039D6E8
			0x68, 0xA8, 0xD6, 0x39, 0x00, // push 0x0039D6A8
			0x68, 0xA4, 0xD8, 0x39, 0x00, // push 0x0039D8A4
			0xE8, 0xFF, 0xFF, 0xFF, 0xFF  // call credit initializer
		};
		const uintptr_t creditInitSequence = ScanXbe(
			kCreditInitCallPattern,
			sizeof(kCreditInitCallPattern),
			imageSize);
		if (creditInitSequence) {
			const uintptr_t callVA = creditInitSequence + 15;
			PatchWithCall(callVA, &TaxiCreditInitHook, 5);
			TAXI_DEBUG_PRINTF(
				"CrazyTaxiPatch: session credit reset hook @0x%08X\n",
				static_cast<unsigned>(callVA));
			if (plog) {
				fprintf(
					plog,
					"Session credit reset: call hook @0x%08X\n",
					static_cast<unsigned>(callVA));
				fflush(plog);
			}
		} else {
			TAXI_DEBUG_PRINTF(
				"CrazyTaxiPatch: credit initializer call not found\n");
			if (plog) {
				fprintf(
					plog,
					"Session credit reset: initializer call not found\n");
				fflush(plog);
			}
		}
	}

	// The cabinet gameplay path normalizes JVS wheel channel 0 into
	// word_32E9E4/word_31CB5A. The course carousel, however, calls the shared
	// console-menu helpers and reads word_31CB5C, whose source word_32E9E6 is
	// never populated by this JVS-only title. Mirror the already-normalized
	// wheel into that menu axis by changing:
	//
	//   mov ax, word ptr [0032E9E6]
	// to
	//   mov ax, word ptr [0032E9E4]
	//
	// This is title/hash scoped and leaves the gameplay steering path intact.
	{
		static const uint8_t kMenuAxisSource[] = {
			0x66, 0xA1, 0xE6, 0xE9, 0x32, 0x00
		};
		const uintptr_t menuAxisVA =
			ScanXbe(kMenuAxisSource, sizeof(kMenuAxisSource), imageSize);
		if (menuAxisVA) {
			static const uint8_t kWheelSourceLowByte[] = { 0xE4 };
			PatchXbeBytes(
				menuAxisVA + 2,
				kWheelSourceLowByte,
				sizeof(kWheelSourceLowByte));
			TAXI_DEBUG_PRINTF(
				"CrazyTaxiPatch: mirrored normalized wheel into menu axis @0x%08X\n",
				static_cast<unsigned>(menuAxisVA));
			if (plog) {
				fprintf(
					plog,
					"Menu wheel: source 0x0032E9E6 -> 0x0032E9E4 @0x%08X\n",
					static_cast<unsigned>(menuAxisVA));
				fflush(plog);
			}
		} else {
			TAXI_DEBUG_PRINTF(
				"CrazyTaxiPatch: menu-axis source pattern not found\n");
			if (plog) {
				fprintf(plog, "Menu wheel: source pattern not found\n");
				fflush(plog);
			}
		}
	}

	if (g_BetaConfig.ct_infinite_timer) {
		std::thread(TaxiTimerLockThread).detach();
		TAXI_DEBUG_PRINTF(
			"CrazyTaxiPatch: timer lock enabled @0x0031DC6C value=%u\n",
			static_cast<unsigned>(g_BetaConfig.ct_timer_lock_value));
		if (plog) {
			fprintf(
				plog,
				"Timer lock: address=0x0031DC6C value=%u interval=50ms\n",
				static_cast<unsigned>(g_BetaConfig.ct_timer_lock_value));
			fflush(plog);
		}
	}
	if (g_BetaConfig.ct_debug_course >= 0 &&
		g_BetaConfig.ct_debug_course <= 2) {
		std::thread(TaxiCourseLockThread).detach();
		TAXI_DEBUG_PRINTF(
			"CrazyTaxiPatch: diagnostic course lock enabled course=%d\n",
			g_BetaConfig.ct_debug_course);
	}
	// === Type-3 check B: JNZ → NOP×6 ===
	{
		static const uint8_t kType3bPat[] = {
			0x0F,0x85, 0xFF,0xFF,0xFF,0xFF,
			0xC7,0x05, 0xFF,0xFF,0xFF,0xFF, 0x06,0x00,0x00,0x00
		};
		uintptr_t type3bVA = ScanXbe(kType3bPat, sizeof(kType3bPat), imageSize);
		if (plog) { fprintf(plog, "Type3b scan: matchVA=0x%08X\n", (unsigned)type3bVA); fflush(plog); }
		if (type3bVA) {
			static const uint8_t kNop6[] = { 0x90,0x90,0x90,0x90,0x90,0x90 };
			PatchXbeBytes(type3bVA, kNop6, sizeof(kNop6));
			TAXI_DEBUG_PRINTF("CrazyTaxiPatch: Type-3 check B NOP'd at VA 0x%08X\n", (unsigned)type3bVA);
			if (plog) { fprintf(plog, "Type3b: NOP'd at 0x%08X\n", (unsigned)type3bVA); fflush(plog); }
		} else {
			TAXI_DEBUG_PRINTF("CrazyTaxiPatch: Type-3 check B not found\n");
		}
	}

	// === CRI Sofdec spin loop fix ===
	// DISABLED: Testing if CRI patches cause timing issues
#if 0
	{
		DWORD oldProtect;
		uint8_t* p = (uint8_t*)0x3CB74;
		if (VirtualProtect(p, 15, PAGE_EXECUTE_READWRITE, &oldProtect)) {
			p[0] = 0xE8; // CALL rel32
			*(int32_t*)(p + 1) = (int32_t)((uintptr_t)&TaxiSfdSpinHelper - (uintptr_t)(p + 5));
			for (int i = 5; i < 15; i++) p[i] = 0x90; // NOP padding
			VirtualProtect(p, 15, oldProtect, &oldProtect);
			FlushInstructionCache(GetCurrentProcess(), p, 15);
			TAXI_DEBUG_PRINTF("CrazyTaxiPatch: Patched CRI Sofdec spin @0x3CB74\n");
		}
	}
#endif

	// === CRI spin loop: patch function to set CompFlag=1 and exit immediately ===
	// The CRI worker thread (sub_C52D0) busy-spins on CompFlag [0x3409C4].
	// We patch the FUNCTION CODE itself so that when the thread executes, it
	// immediately sets CompFlag=1 and jumps to the exit code. This eliminates
	// the race condition where CRI init clears a pre-set flag value.
	// NOTE: This also prevents all CRI-based file loading (movies, audio streams)
	// from actually completing. Disable via ct_cri_force_complete=0 for movie support.
	{
		static const uint8_t kCriSpinProbePattern[] = {
			0x85, 0xC0,
			0x75, 0x1B,
			0x8D, 0xA4, 0x24, 0x00, 0x00, 0x00, 0x00
		};
		const uintptr_t matchVA = ScanXbe(
			kCriSpinProbePattern,
			sizeof(kCriSpinProbePattern),
			imageSize);
		if (matchVA) {
			auto* funcStart =
				reinterpret_cast<uint8_t*>(matchVA - 5);
			if (funcStart[0] == 0xA1 &&
				funcStart[16] == 0xA1 &&
				funcStart[21] == 0x40 &&
				funcStart[22] == 0xA3 &&
				funcStart[27] == 0xA1 &&
				funcStart[32] == 0x85 &&
				funcStart[33] == 0xC0 &&
				funcStart[34] == 0x74) {
				g_CriSpinPatchAddr = funcStart;
				g_CriSpinFlagAddr =
					reinterpret_cast<volatile uint32_t*>(
						*reinterpret_cast<uint32_t*>(
							funcStart + 1));
				g_CriSpinCounterAddr =
					reinterpret_cast<volatile uint32_t*>(
						*reinterpret_cast<uint32_t*>(
							funcStart + 17));
			}
		}
	}

	if (g_BetaConfig.ct_cri_force_complete) {
	// Original layout (36 bytes):
	//   +0:  A1 xx xx xx xx         MOV EAX,[CompFlag]
	//   +5:  85 C0                  TEST EAX,EAX
	//   +7:  75 1B                  JNZ +0x1B -> +36 (exit code)
	//   +9:  8D A4 24 00 00 00 00   LEA ESP,[ESP] (alignment NOP)
	//   +16: A1 yy yy yy yy        MOV EAX,[spinCounter]
	//   +21: 40                     INC EAX
	//   +22: A3 yy yy yy yy        MOV [spinCounter],EAX
	//   +27: A1 xx xx xx xx        MOV EAX,[CompFlag]
	//   +32: 85 C0                  TEST EAX,EAX
	//   +34: 74 EC                  JZ -20 (back to +16)
	//   +36: exit code: PUSH F0000001; MOV [CompFlag2],1; CALL sub_BE82A
	//
	// Patched (12 bytes used, 24 NOPs):
	//   +0:  C7 05 xx xx xx xx 01 00 00 00  MOV DWORD [CompFlag], 1
	//   +10: EB 18                            JMP +36 (exit code)
	//   +12: 90×24                            NOP padding
		static const uint8_t kCriSpinPat[] = {
			0x85, 0xC0,                                   // TEST EAX,EAX
			0x75, 0x1B,                                   // JNZ +0x1B
			0x8D, 0xA4, 0x24, 0x00, 0x00, 0x00, 0x00     // LEA ESP,[ESP] (7-byte NOP)
		};
		uintptr_t matchVA = ScanXbe(kCriSpinPat, sizeof(kCriSpinPat), imageSize);
		if (matchVA) {
			uint8_t* funcStart = (uint8_t*)(matchVA - 5);
			if (funcStart[0] == 0xA1) {
				uint32_t flagAddr = *(uint32_t*)(funcStart + 1);
				DWORD oldProtect;
				const int kPatchLen = 36;
				if (VirtualProtect(funcStart, kPatchLen, PAGE_EXECUTE_READWRITE, &oldProtect)) {
					// MOV DWORD PTR [CompFlag], 1
					funcStart[0] = 0xC7;
					funcStart[1] = 0x05;
					*(uint32_t*)(funcStart + 2) = flagAddr;
					*(uint32_t*)(funcStart + 6) = 1;
					// JMP rel8 to exit code (+36 from funcStart = +24 from here)
					funcStart[10] = 0xEB;
					funcStart[11] = 0x18;
					// NOP the rest
					for (int i = 12; i < kPatchLen; i++) funcStart[i] = 0x90;
					VirtualProtect(funcStart, kPatchLen, oldProtect, &oldProtect);
					FlushInstructionCache(GetCurrentProcess(), funcStart, kPatchLen);
					TAXI_DEBUG_PRINTF("CrazyTaxiPatch: CRI spin patched @0x%08X (flag=0x%08X) -> set+exit\n",
						(unsigned)(uintptr_t)funcStart, flagAddr);
					if (plog) { fprintf(plog, "CRI spin: code-patched @0x%08X flag=0x%08X\n",
						(unsigned)(uintptr_t)funcStart, flagAddr); fflush(plog); }
				}
			}
		}
	} else {
		TAXI_DEBUG_PRINTF("CrazyTaxiPatch: CRI spin force-complete DISABLED (ct_cri_force_complete=0)\n");
		if (plog) { fprintf(plog, "CRI spin force-complete: DISABLED by ct_cri_force_complete=0\n"); fflush(plog); }
		if (g_CriSpinFlagAddr != nullptr &&
			g_CriSpinCounterAddr != nullptr) {
			ApplyCriAsyncSpinPatch(plog);
			TAXI_DEBUG_PRINTF(
				"CrazyTaxiPatch: alertable CRI completion wait enabled "
				"(%u ms fallback)\n",
				static_cast<unsigned>(
					g_BetaConfig.ct_cri_wait_timeout_ms));
		}
	}

	// === CRI async I/O completion spin fix (SleepEx trampoline) ===
	// DISABLED: SleepEx trampoline causes GM=0 hang
#if 0
	// The CRI worker thread (sub_C52D0) is a busy-spin loop that checks a
	// completion flag ([003409C4]) but never enters an alertable wait, so APCs
	// from host async I/O are never delivered and the flag is never set.
	// Fix: replace the inner loop with CALL to a SleepEx(1,TRUE) trampoline,
	// which yields the CPU and processes pending APCs each iteration.
	//
	// Original inner loop (20 bytes, 0xC52E0-0xC52F3):
	//   C52E0: A1 AC 09 34 00    MOV EAX,[3409AC]   ; read spin counter
	//   C52E5: 40                INC EAX
	//   C52E6: A3 AC 09 34 00    MOV [3409AC],EAX    ; write spin counter
	//   C52EB: A1 C4 09 34 00    MOV EAX,[3409C4]    ; read CompFlag
	//   C52F0: 85 C0             TEST EAX,EAX
	//   C52F2: 74 EC             JZ C52E0             ; loop if zero
	//
	// Patched (14 bytes used, 6 NOPs):
	//   C52E0: E8 xx xx xx xx    CALL trampoline      ; SleepEx(1,TRUE)
	//   C52E5: A1 C4 09 34 00    MOV EAX,[3409C4]    ; check CompFlag
	//   C52EA: 85 C0             TEST EAX,EAX
	//   C52EC: 74 F2             JZ C52E0             ; loop if still zero
	//   C52EE: 90 90 90 90 90 90 NOP padding
	{
		static const uint8_t kCriAsyncSpinPat[] = {
			0x85, 0xC0,                                   // TEST EAX,EAX
			0x75, 0x1B,                                   // JNZ +0x1B
			0x8D, 0xA4, 0x24, 0x00, 0x00, 0x00, 0x00     // LEA ESP,[ESP] (7-byte NOP)
		};
		uintptr_t matchVA = ScanXbe(kCriAsyncSpinPat, sizeof(kCriAsyncSpinPat), imageSize);
		if (matchVA) {
			uint8_t* funcStart = (uint8_t*)(matchVA - 5);
			if (funcStart[0] == 0xA1) {
				g_CriSpinFlagAddr = (volatile uint32_t*)*(uint32_t*)(funcStart + 1);
				TAXI_DEBUG_PRINTF("CrazyTaxiPatch: CRI async spin @0x%08X (flag=0x%08X)\n",
					(unsigned)(uintptr_t)funcStart, (unsigned)(uintptr_t)g_CriSpinFlagAddr);

				// Resolve SleepEx from host kernel32.dll
				HMODULE hK32 = GetModuleHandleA("kernel32.dll");
				FARPROC pSleepEx = hK32 ? GetProcAddress(hK32, "SleepEx") : nullptr;
				if (pSleepEx) {
					// Allocate executable trampoline
					uint8_t* cave = (uint8_t*)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
					if (cave) {
						// Trampoline: SleepEx(1, TRUE); ret
						int ci = 0;
						cave[ci++] = 0x6A; cave[ci++] = 0x01; // push 1 (bAlertable = TRUE)
						cave[ci++] = 0x6A; cave[ci++] = 0x01; // push 1 (dwMilliseconds = 1)
						cave[ci++] = 0xB8; *(uint32_t*)(cave + ci) = (uint32_t)(uintptr_t)pSleepEx; ci += 4; // mov eax, SleepEx
						cave[ci++] = 0xFF; cave[ci++] = 0xD0; // call eax
						cave[ci++] = 0xC3; // ret

						// Patch the inner loop (C52E0-C52F3, 20 bytes)
						uint8_t* innerLoop = funcStart + 16; // C52D0 + 16 = C52E0
						DWORD oldProtect;
						if (VirtualProtect(innerLoop, 20, PAGE_EXECUTE_READWRITE, &oldProtect)) {
							int pi = 0;
							// CALL trampoline (relative)
							innerLoop[pi++] = 0xE8;
							*(int32_t*)(innerLoop + pi) = (int32_t)(cave - (innerLoop + pi + 4));
							pi += 4;
							// MOV EAX, [CompFlag]
							innerLoop[pi++] = 0xA1;
							*(uint32_t*)(innerLoop + pi) = (uint32_t)(uintptr_t)g_CriSpinFlagAddr;
							pi += 4;
							// TEST EAX, EAX
							innerLoop[pi++] = 0x85;
							innerLoop[pi++] = 0xC0;
							// JZ back to start of inner loop (C52E0)
							// At this point pi=12, instruction at C52E0+12, PC after JZ = C52E0+14
							// Displacement = C52E0 - (C52E0+14) = -14 = 0xF2
							innerLoop[pi++] = 0x74;
							innerLoop[pi++] = (uint8_t)(-14); // 0xF2
							// NOP remaining bytes
							while (pi < 20) innerLoop[pi++] = 0x90;

							VirtualProtect(innerLoop, 20, oldProtect, &oldProtect);
							FlushInstructionCache(GetCurrentProcess(), innerLoop, 20);
							TAXI_DEBUG_PRINTF("CrazyTaxiPatch: CRI spin patched with SleepEx trampoline @cave=0x%08X\n",
								(unsigned)(uintptr_t)cave);
							if (plog) { fprintf(plog, "CRI spin: trampoline @0x%08X, SleepEx=0x%08X, inner=0x%08X\n",
								(unsigned)(uintptr_t)cave, (unsigned)(uintptr_t)pSleepEx,
								(unsigned)(uintptr_t)innerLoop); fflush(plog); }
						}
					}
				} else {
					TAXI_DEBUG_PRINTF("CrazyTaxiPatch: WARNING: SleepEx not found, CRI spin NOT patched\n");
				}
			}
		} else {
			TAXI_DEBUG_PRINTF("CrazyTaxiPatch: CRI async spin pattern not found\n");
		}
	}
#endif // CRI async spin disabled

	// === Movie creation bypass (sub_11200) — DISABLED ===
	// Direct GameMode=6 skip breaks the scene manager transition (sub_25680
	// never gets called because the scene descriptor table isn't updated).
	// Instead, let the movie player be created normally but patch sub_3C950
	// (movie stop) to prevent CRI thread blocking, and sub_3CA20 to return 3.
#if 0
	{
		uint8_t* func = (uint8_t*)0x11200;
		static const uint8_t kExpected[] = { 0xC7, 0x05, 0x20, 0x17, 0x27, 0x00, 0x00, 0x00, 0x00, 0x00 };
		bool match = (memcmp(func, kExpected, sizeof(kExpected)) == 0);
		if (plog) { fprintf(plog, "sub_11200 verify: match=%d first10=%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X\n",
			(int)match, func[0],func[1],func[2],func[3],func[4],func[5],func[6],func[7],func[8],func[9]); fflush(plog); }
		if (match) {
			DWORD oldProtect;
			const int kPatchLen = 54;
			if (VirtualProtect(func, kPatchLen, PAGE_EXECUTE_READWRITE, &oldProtect)) {
				int off = 0;
				func[off++] = 0xC7; func[off++] = 0x05;
				*(uint32_t*)(func + off) = 0x271720; off += 4;
				*(uint32_t*)(func + off) = 0; off += 4;
				func[off++] = 0xC7; func[off++] = 0x05;
				*(uint32_t*)(func + off) = 0x271724; off += 4;
				*(uint32_t*)(func + off) = 0; off += 4;
				func[off++] = 0xC7; func[off++] = 0x05;
				*(uint32_t*)(func + off) = 0x1D6598; off += 4;
				*(uint32_t*)(func + off) = 1; off += 4;
				func[off++] = 0xC6; func[off++] = 0x05;
				*(uint32_t*)(func + off) = 0x1D6680; off += 4;
				func[off++] = 0x00;
				func[off++] = 0xC7; func[off++] = 0x05;
				*(uint32_t*)(func + off) = 0x31CC6C; off += 4;
				*(uint32_t*)(func + off) = 6; off += 4;
				func[off++] = 0xA1;
				*(uint32_t*)(func + off) = 0x31CC6C; off += 4;
				func[off++] = 0xC3;
				while (off < kPatchLen) func[off++] = 0x90;
				VirtualProtect(func, kPatchLen, oldProtect, &oldProtect);
				FlushInstructionCache(GetCurrentProcess(), func, kPatchLen);
				TAXI_DEBUG_PRINTF("CrazyTaxiPatch: sub_11200 movie bypass APPLIED (%d bytes)\n", off);
			}
		}
	}
#endif

	// === Prevent MB state from reaching 2 or 3 ===
	// The DIMM board communication (sub_FA350, sub_FACB0) contains busy-spins
	// that depend on IRQ10 delivery. When the emulator's faked responses
	// don't match real hardware timing, the spin loop + subsequent D3D state
	// causes rendering to stop at exactly frame 180.
	// Fix: change "MOV DWORD [39D5A8], 2" to "MOV DWORD [39D5A8], 0"
	// so MB goes from state 1 directly to 0 (inactive), preventing any DIMM
	// board communication. The game shows "MEDIA BOARD BOOT UP CHECK... SKIPPED".
	{
		static const uint8_t kMBSet2Pat[] = {
			0xC7, 0x05, 0xA8, 0xD5, 0x39, 0x00,   // MOV DWORD PTR [0039D5A8],
			0x02, 0x00, 0x00, 0x00                  // 2
		};
		uintptr_t matchVA = ScanXbe(kMBSet2Pat, sizeof(kMBSet2Pat), imageSize);
		if (plog) { fprintf(plog, "MB=2 scan: matchVA=0x%08X\n", (unsigned)matchVA); fflush(plog); }
		if (matchVA) {
			uint8_t* target = (uint8_t*)(matchVA + 6);
			DWORD oldProtect;
			if (VirtualProtect(target, 4, PAGE_EXECUTE_READWRITE, &oldProtect)) {
				*(uint32_t*)target = 0;  // change 2 to 0
				VirtualProtect(target, 4, oldProtect, &oldProtect);
				FlushInstructionCache(GetCurrentProcess(), target, 4);
				TAXI_DEBUG_PRINTF("CrazyTaxiPatch: MB=2 → MB=0 @0x%08X\n", (unsigned)matchVA);
				if (plog) { fprintf(plog, "MB=2 → MB=0 @0x%08X\n", (unsigned)matchVA); fflush(plog); }
			}
		}
	}

	// Also prevent MB=3 in case the game takes a different code path
	{
		static const uint8_t kMBSet3Pat[] = {
			0xC7, 0x05, 0xA8, 0xD5, 0x39, 0x00,   // MOV DWORD PTR [0039D5A8],
			0x03, 0x00, 0x00, 0x00                  // 3
		};
		uintptr_t matchVA = ScanXbe(kMBSet3Pat, sizeof(kMBSet3Pat), imageSize);
		if (plog) { fprintf(plog, "MB=3 scan: matchVA=0x%08X\n", (unsigned)matchVA); fflush(plog); }
		if (matchVA) {
			uint8_t* target = (uint8_t*)(matchVA + 6);
			DWORD oldProtect;
			if (VirtualProtect(target, 4, PAGE_EXECUTE_READWRITE, &oldProtect)) {
				*(uint32_t*)target = 0;
				VirtualProtect(target, 4, oldProtect, &oldProtect);
				FlushInstructionCache(GetCurrentProcess(), target, 4);
				TAXI_DEBUG_PRINTF("CrazyTaxiPatch: MB=3 → MB=0 @0x%08X\n", (unsigned)matchVA);
				if (plog) { fprintf(plog, "MB=3 → MB=0 @0x%08X\n", (unsigned)matchVA); fflush(plog); }
			}
		}
	}

	// === Skip only the two synchronous boot-readiness waits ===
	// Crazy Taxi's startup UI waits up to ~2 seconds for both the base-board
	// and media-board readiness helpers. Under Wine/Box32 the 20 ms guest wait
	// used by this code path can lose its wake while the Chihiro worker keeps
	// rendering the BDID screen. Leave the readiness helpers and the normal
	// per-frame media update untouched; replace only their first calls inside
	// sub_40160 so startup observes them as ready and continues immediately.
	{
		auto patchInitialReadyCall = [plog](
			uintptr_t target,
			const char* label) {
			uint8_t* const begin = reinterpret_cast<uint8_t*>(0x0040160);
			uint8_t* const end = reinterpret_cast<uint8_t*>(0x0040760);
			for (uint8_t* call = begin; call + 5 <= end; ++call) {
				if (call[0] != 0xE8) {
					continue;
				}
				const auto relative = *reinterpret_cast<int32_t*>(call + 1);
				const uintptr_t destination =
					reinterpret_cast<uintptr_t>(call + 5) + relative;
				if (destination != target) {
					continue;
				}

				DWORD oldProtect = 0;
				if (VirtualProtect(
						call,
						5,
						PAGE_EXECUTE_READWRITE,
						&oldProtect)) {
					static const uint8_t kReturnReady[] = {
						0xB8, 0x01, 0x00, 0x00, 0x00 // MOV EAX,1
					};
					memcpy(call, kReturnReady, sizeof(kReturnReady));
					VirtualProtect(call, 5, oldProtect, &oldProtect);
					FlushInstructionCache(
						GetCurrentProcess(),
						call,
						sizeof(kReturnReady));
					TAXI_DEBUG_PRINTF(
						"CrazyTaxiPatch: %s startup readiness call bypassed @0x%08X\n",
						label,
						static_cast<unsigned>(
							reinterpret_cast<uintptr_t>(call)));
					if (plog) {
						fprintf(
							plog,
							"%s startup readiness call bypassed @0x%08X\n",
							label,
							static_cast<unsigned>(
								reinterpret_cast<uintptr_t>(call)));
						fflush(plog);
					}
				}
				return;
			}
			TAXI_DEBUG_PRINTF(
				"CrazyTaxiPatch: %s startup readiness call not found\n",
				label);
			if (plog) {
				fprintf(plog, "%s startup readiness call not found\n", label);
				fflush(plog);
			}
		};

		patchInitialReadyCall(0x000F6AE0, "base-board");
		patchInitialReadyCall(0x000F6AF0, "media-board");
	}


	// === Sofdec movie status → always "done" (toggleable) ===
	// Sofdec movies decode MPEG into textures via the game's CRI library — the
	// emulator doesn't need special support. The skip was originally needed because
	// CRI thread hangs caused boot failures, but the KiTimerExpiration dpcIdx fix
	// resolved the root cause. Set ct_skip_movies=0 in beta.ini to allow playback.
	if (g_BetaConfig.ct_skip_movies) {
		static const uint8_t kMovieStatusPat[] = {
			0x8B, 0x01, 0x8B, 0x08, 0x50, 0xFF, 0x51, 0x20,
			0x83, 0xC4, 0x04, 0xC3
		};
		uintptr_t matchVA = ScanXbe(kMovieStatusPat, sizeof(kMovieStatusPat), imageSize);
		if (plog) { fprintf(plog, "Movie status scan: matchVA=0x%08X\n", (unsigned)matchVA); fflush(plog); }
		if (matchVA) {
			uint8_t* funcAddr = (uint8_t*)matchVA;
			DWORD oldProtect;
			if (VirtualProtect(funcAddr, 6, PAGE_EXECUTE_READWRITE, &oldProtect)) {
				static const uint8_t kRetDone[] = { 0xB8, 0x03, 0x00, 0x00, 0x00, 0xC3 };
				memcpy(funcAddr, kRetDone, 6);
				VirtualProtect(funcAddr, 6, oldProtect, &oldProtect);
				FlushInstructionCache(GetCurrentProcess(), funcAddr, 6);
				TAXI_DEBUG_PRINTF("CrazyTaxiPatch: sub_3CA20 → return 3 @0x%08X\n", (unsigned)(uintptr_t)funcAddr);
				if (plog) { fprintf(plog, "sub_3CA20: return 3 @0x%08X\n", (unsigned)(uintptr_t)funcAddr); fflush(plog); }
			}
		}
	} else {
		TAXI_DEBUG_PRINTF("CrazyTaxiPatch: Sofdec movie skip DISABLED (ct_skip_movies=0)\n");
		if (plog) { fprintf(plog, "Movie status skip: DISABLED by ct_skip_movies=0\n"); fflush(plog); }
	}

	// === Movie stop no-op (sub_3C950) ===
	// DISABLED: This breaks CRI shutdown, leaving pending I/O that causes the
	// CRI async spin (0xC52E6) to hang during GM=1 movie phase cleanup.
#if 0
	{
		uint8_t* func = (uint8_t*)0x3C950;
		DWORD oldProtect;
		if (VirtualProtect(func, 1, PAGE_EXECUTE_READWRITE, &oldProtect)) {
			uint8_t origByte = func[0];
			func[0] = 0xC3; // RET
			VirtualProtect(func, 1, oldProtect, &oldProtect);
			FlushInstructionCache(GetCurrentProcess(), func, 1);
			TAXI_DEBUG_PRINTF("CrazyTaxiPatch: sub_3C950 (movie stop) → RET (was 0x%02X)\n", origByte);
			if (plog) { fprintf(plog, "sub_3C950: RET (was 0x%02X)\n", origByte); fflush(plog); }
		}
	}
#endif

	// === DirectSound spin loop bypass ===
	// DISABLED: Match at 0x11FDA1 may be in a rendering sync function, not DSound
#if 0
	// NOP the tight busy-wait (JNZ -6 after CMP [EAX+10h],0) to prevent CPU starvation
	{
		static const uint8_t kDSoundSpinPat[] = {
			0x83, 0x78, 0x10, 0x00,   // CMP DWORD [EAX+10h], 0
			0x75, 0xFA                 // JNZ -6 (back to CMP)
		};
		uintptr_t dsSpinVA = ScanXbe(kDSoundSpinPat, sizeof(kDSoundSpinPat), imageSize);
		if (plog) { fprintf(plog, "DSound spin scan: matchVA=0x%08X\n", (unsigned)dsSpinVA); fflush(plog); }
		if (dsSpinVA) {
			// NOP the JNZ (2 bytes at offset +4)
			uint8_t* jnzAddr = (uint8_t*)(dsSpinVA + 4);
			DWORD oldProtect;
			if (VirtualProtect(jnzAddr, 2, PAGE_EXECUTE_READWRITE, &oldProtect)) {
				TAXI_DEBUG_PRINTF("CrazyTaxiPatch: DSound spin @0x%08X: was %02X %02X\n",
					(unsigned)(uintptr_t)jnzAddr, jnzAddr[0], jnzAddr[1]);
				jnzAddr[0] = 0x90;
				jnzAddr[1] = 0x90;
				VirtualProtect(jnzAddr, 2, oldProtect, &oldProtect);
				FlushInstructionCache(GetCurrentProcess(), jnzAddr, 2);
				TAXI_DEBUG_PRINTF("CrazyTaxiPatch: DSound spin NOP'd @0x%08X\n", (unsigned)(uintptr_t)jnzAddr);
			}
		} else {
			TAXI_DEBUG_PRINTF("CrazyTaxiPatch: DSound spin pattern not found\n");
		}
	}
#endif

#if defined(_DEBUG)
	std::thread(TaxiDiagThread).detach();
#endif

	TAXI_DEBUG_PRINTF("CrazyTaxiPatch: done\n");
	if (plog) { fprintf(plog, "CrazyTaxiPatch: done\n"); fclose(plog); }
}
