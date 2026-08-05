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
#include "common\BetaConfig.h"
#include <cmath>
#include <cstdio>
#include <thread>

bool g_ChihiroCrazyTaxiGame = false;

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
	ExecFn exec5 = (ExecFn)0xCBCA0;  // CRI ExecServer priority 5 (file completion)

	for (;;) {
		int stat = getStat(0);
		if (stat == 3) break;
		exec2();
		exec5();
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

// CRI async I/O completion spin loop fix.
// The function at ~0xC52D0 spins on a CRI completion flag ([003409C4])
// without ever calling ExecServer, so async I/O never completes.
// Root cause: the tight spin never enters alertable wait, so async I/O
// completion APCs are never delivered to this thread.
static volatile uint32_t* g_CriSpinFlagAddr = nullptr;
static volatile DWORD g_GameThreadId = 0;  // captured from CRI spin helper
static uint8_t* g_CriSpinPatchAddr = nullptr; // Address of CRI spin loop for delayed patching

static volatile DWORD g_SpinEnterTick = 0;   // for diag thread to read
static volatile DWORD g_SpinExitTick = 0;

static void __cdecl TaxiCriAsyncSpinHelper() {
	if (!g_CriSpinFlagAddr) return;

	// NtReadFile queues its completion APC to this same native thread, but the
	// original title code only busy-spins. Enter an alertable wait so Windows
	// can dispatch the real completion callback without forcing CRI requests.
	g_GameThreadId = GetCurrentThreadId();
	g_SpinEnterTick = GetTickCount();

	const DWORD start = GetTickCount();
	while (*g_CriSpinFlagAddr == 0) {
		SleepEx(1, TRUE);
		if (g_BetaConfig.ct_cri_wait_timeout_ms != 0 &&
			GetTickCount() - start >= g_BetaConfig.ct_cri_wait_timeout_ms) {
			InterlockedExchange(
				reinterpret_cast<volatile LONG*>(g_CriSpinFlagAddr),
				1);
			break;
		}
	}

	g_SpinExitTick = GetTickCount();
}

// Apply CRI async spin patch at runtime (called when GM=7 detected)
static void ApplyCriAsyncSpinPatch() {
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
		for (int i = 7; i < kPatchLen; ++i) funcStart[i] = 0x90;
		VirtualProtect(funcStart, kPatchLen, oldProtect, &oldProtect);
		FlushInstructionCache(GetCurrentProcess(), funcStart, kPatchLen);
	}
	g_CriSpinPatchAddr = nullptr; // only apply once
}

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

// Crazy Taxi builds one spotlight every frame but never adds it to the
// renderer's eight-light list. Reuse that dormant object as the arcade
// cabinet's broad, camera-following headlight cone before committing lights.
static int __fastcall TaxiCommitLightsWithHeadlightHook(
	void* renderer,
	void* /*edx*/)
{
	using RegisterLightFn = int(__thiscall*)(void*, char*);
	using CommitLightsFn = int(__thiscall*)(void*);

	const auto registerLight = reinterpret_cast<RegisterLightFn>(0x000C0140);
	const auto commitLights = reinterpret_cast<CommitLightsFn>(0x000B8660);

	if (g_BetaConfig.ct_headlights) {
		__try {
			const auto gameMode =
				*reinterpret_cast<volatile const uint32_t*>(0x0031CC6C);
			if (gameMode == 3) {
				auto* light = reinterpret_cast<volatile float*>(0x00324328);
				const auto* camera =
					reinterpret_cast<volatile const float*>(0x00278DB0);
				const auto* target =
					reinterpret_cast<volatile const float*>(0x00278D60);

				float forwardX = target[0] - camera[0];
				float forwardZ = target[2] - camera[2];
				const float horizontalLength =
					std::sqrt(forwardX * forwardX + forwardZ * forwardZ);
				if (std::isfinite(horizontalLength) &&
					horizontalLength > 0.001f) {
					forwardX /= horizontalLength;
					forwardZ /= horizontalLength;

					constexpr float kForwardOffset = 8.0f;
					constexpr float kHeightOffset = 3.0f;
					constexpr float kDownwardPitch = -0.08f;
					constexpr float kRange = 900.0f;
					constexpr float kHalfRange = kRange * 0.5f;
					const float directionScale =
						1.0f / std::sqrt(
							1.0f + kDownwardPitch * kDownwardPitch);
					const float directionX = forwardX * directionScale;
					const float directionY = kDownwardPitch * directionScale;
					const float directionZ = forwardZ * directionScale;
					const float positionX =
						target[0] + forwardX * kForwardOffset;
					const float positionY = target[1] + kHeightOffset;
					const float positionZ =
						target[2] + forwardZ * kForwardOffset;

					*reinterpret_cast<volatile uint32_t*>(light) = 2u;
					light[13] = positionX;
					light[14] = positionY;
					light[15] = positionZ;
					light[16] = directionX;
					light[17] = directionY;
					light[18] = directionZ;
					light[19] = kRange;
					light[20] = 1.0f;       // Falloff
					light[21] = 1.0f;       // Attenuation0
					light[22] = 0.0015f;    // Attenuation1
					light[23] = 0.000008f;  // Attenuation2
					light[24] = 0.45f;
					light[25] = 0.90f;
					light[26] = positionX + directionX * kHalfRange;
					light[27] = positionY + directionY * kHalfRange;
					light[28] = positionZ + directionZ * kHalfRange;
					light[29] = kHalfRange;

					registerLight(
						reinterpret_cast<void*>(0x00324328),
						static_cast<char*>(renderer));
				}
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			// Alternate executable revisions retain the normal light commit.
		}
	}

	return commitLights(renderer);
}

void ApplyCrazyTaxiPatches(uint64_t xbeHash, uint32_t imageSize)
{
	printf("CrazyTaxiPatch: xbeHash=0x%016llX, match=%d\n", (unsigned long long)xbeHash, (int)IsCrazyTaxiXbe(xbeHash));

	g_ChihiroCrazyTaxiGame = IsCrazyTaxiXbe(xbeHash);
	if (!g_ChihiroCrazyTaxiGame) return;

	// The title requests an immediate interval. At 120 Hz this doubles its
	// frame-counted menus and physics, so use CXBXR's existing 60 Hz limiter.
	extern xbox::dword_xt g_Xbox_PresentationInterval_Override;
	g_Xbox_PresentationInterval_Override = 1;

	// Add the title-local headlight only when this known call still targets the
	// expected light commit function.
	{
		constexpr uintptr_t kCommitLightsCall = 0x00025CBD;
		const auto* call = reinterpret_cast<const uint8_t*>(kCommitLightsCall);
		if (call[0] == 0xE8) {
			const int32_t displacement =
				*reinterpret_cast<const int32_t*>(call + 1);
			if (kCommitLightsCall + 5 + displacement == 0x000B8660) {
				PatchWithCall(
					kCommitLightsCall,
					&TaxiCommitLightsWithHeadlightHook,
					5);
			}
		}
	}

	// NOTE: D3D BetaConfig overrides for Crazy Taxi are applied EARLY in CxbxKrnl.cpp
	// (right after BetaConfig_Load, before CxbxInitWindow) because D3D device creation
	// caches these values. Overriding here in ApplyCrazyTaxiPatches is too late.
	// See CxbxKrnlApplyGameSpecificBetaOverrides().

	FILE* plog = fopen("C:\\temp\\taxi_patches.txt", "w");

	EmuLog(LOG_LEVEL::INFO, "CrazyTaxiPatch: applying patches (hash 0x%016llX, imageSize=0x%X)", (unsigned long long)xbeHash, imageSize);
	if (plog) { fprintf(plog, "CrazyTaxiPatch: hash=0x%016llX imageSize=0x%X\n", (unsigned long long)xbeHash, imageSize); fflush(plog); }

	// ============== ACTIVE PATCHES ==============

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
			std::printf("CrazyTaxiPatch: Type-3 check B NOP'd at VA 0x%08X\n", (unsigned)type3bVA);
			if (plog) { fprintf(plog, "Type3b: NOP'd at 0x%08X\n", (unsigned)type3bVA); fflush(plog); }
		} else {
			std::printf("CrazyTaxiPatch: Type-3 check B not found\n");
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
			printf("CrazyTaxiPatch: Patched CRI Sofdec spin @0x3CB74\n");
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
					printf("CrazyTaxiPatch: CRI spin patched @0x%08X (flag=0x%08X) -> set+exit\n",
						(unsigned)(uintptr_t)funcStart, flagAddr);
					if (plog) { fprintf(plog, "CRI spin: code-patched @0x%08X flag=0x%08X\n",
						(unsigned)(uintptr_t)funcStart, flagAddr); fflush(plog); }
				}
			}
		}
	} else {
		printf("CrazyTaxiPatch: CRI spin force-complete DISABLED (ct_cri_force_complete=0)\n");
		if (plog) { fprintf(plog, "CRI spin force-complete: DISABLED by ct_cri_force_complete=0\n"); fflush(plog); }

		static const uint8_t kCriSpinPat[] = {
			0x85, 0xC0,
			0x75, 0x1B,
			0x8D, 0xA4, 0x24, 0x00, 0x00, 0x00, 0x00
		};
		const uintptr_t matchVA =
			ScanXbe(kCriSpinPat, sizeof(kCriSpinPat), imageSize);
		if (matchVA) {
			auto* funcStart = reinterpret_cast<uint8_t*>(matchVA - 5);
			if (funcStart[0] == 0xA1 &&
				funcStart[27] == 0xA1 &&
				funcStart[32] == 0x85 &&
				funcStart[33] == 0xC0 &&
				funcStart[34] == 0x74) {
				g_CriSpinPatchAddr = funcStart;
				g_CriSpinFlagAddr =
					reinterpret_cast<volatile uint32_t*>(
						*reinterpret_cast<uint32_t*>(funcStart + 1));
				ApplyCriAsyncSpinPatch();
			}
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
				printf("CrazyTaxiPatch: CRI async spin @0x%08X (flag=0x%08X)\n",
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
							printf("CrazyTaxiPatch: CRI spin patched with SleepEx trampoline @cave=0x%08X\n",
								(unsigned)(uintptr_t)cave);
							if (plog) { fprintf(plog, "CRI spin: trampoline @0x%08X, SleepEx=0x%08X, inner=0x%08X\n",
								(unsigned)(uintptr_t)cave, (unsigned)(uintptr_t)pSleepEx,
								(unsigned)(uintptr_t)innerLoop); fflush(plog); }
						}
					}
				} else {
					printf("CrazyTaxiPatch: WARNING: SleepEx not found, CRI spin NOT patched\n");
				}
			}
		} else {
			printf("CrazyTaxiPatch: CRI async spin pattern not found\n");
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
				printf("CrazyTaxiPatch: sub_11200 movie bypass APPLIED (%d bytes)\n", off);
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
				printf("CrazyTaxiPatch: MB=2 → MB=0 @0x%08X\n", (unsigned)matchVA);
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
				printf("CrazyTaxiPatch: MB=3 → MB=0 @0x%08X\n", (unsigned)matchVA);
				if (plog) { fprintf(plog, "MB=3 → MB=0 @0x%08X\n", (unsigned)matchVA); fflush(plog); }
			}
		}
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
				printf("CrazyTaxiPatch: sub_3CA20 → return 3 @0x%08X\n", (unsigned)(uintptr_t)funcAddr);
				if (plog) { fprintf(plog, "sub_3CA20: return 3 @0x%08X\n", (unsigned)(uintptr_t)funcAddr); fflush(plog); }
			}
		}
	} else {
		printf("CrazyTaxiPatch: Sofdec movie skip DISABLED (ct_skip_movies=0)\n");
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
			printf("CrazyTaxiPatch: sub_3C950 (movie stop) → RET (was 0x%02X)\n", origByte);
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
				printf("CrazyTaxiPatch: DSound spin @0x%08X: was %02X %02X\n",
					(unsigned)(uintptr_t)jnzAddr, jnzAddr[0], jnzAddr[1]);
				jnzAddr[0] = 0x90;
				jnzAddr[1] = 0x90;
				VirtualProtect(jnzAddr, 2, oldProtect, &oldProtect);
				FlushInstructionCache(GetCurrentProcess(), jnzAddr, 2);
				printf("CrazyTaxiPatch: DSound spin NOP'd @0x%08X\n", (unsigned)(uintptr_t)jnzAddr);
			}
		} else {
			printf("CrazyTaxiPatch: DSound spin pattern not found\n");
		}
	}
#endif

	std::thread(TaxiDiagThread).detach();

	printf("CrazyTaxiPatch: done\n");
	if (plog) { fprintf(plog, "CrazyTaxiPatch: done\n"); fclose(plog); }
}
