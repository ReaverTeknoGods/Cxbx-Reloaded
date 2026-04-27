// ******************************************************************
// *  Cxbx Gundam Battle Operating Simulator patches
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

#include "devices\chihiro\JvsIo.h"

// ── Hook functions ────────────────────────────────────────────────

static int __stdcall GundamMbRecvHook(uint32_t a1, uint32_t a2, uint32_t a3) {
	return 0; // no data
}

static int __stdcall GundamMbSendHook(uint32_t a1, uint32_t a2, uint32_t a3) {
	return 0;
}

static int __stdcall GundamLinkOkHook(uint32_t a1, uint32_t a2, uint32_t a3) {
	return 1; // link is OK
}

// Initialize an Xbox RTL_CRITICAL_SECTION at the given address.
static void InitXboxCriticalSection(uintptr_t addr) {
	std::memset((void*)addr, 0, 0x1C);
	*(uint8_t*)(addr + 0x00) = 1;     // Type = SynchronizationEvent
	*(uint8_t*)(addr + 0x02) = 4;     // Size = sizeof(KEVENT)/sizeof(LONG)
	*(uint32_t*)(addr + 0x08) = (uint32_t)(addr + 8); // WaitListHead.Flink = self
	*(uint32_t*)(addr + 0x0C) = (uint32_t)(addr + 8); // WaitListHead.Blink = self
	*(int32_t*)(addr + 0x10) = -1;    // LockCount = -1 (unlocked)
}

// Hook replacing sub_99600 (DIMM/JVS full init).
// Initializes critical sections and state without hardware I/O.
static int __stdcall GundamDimmInitHook(int a1, int a2, const void* a3) {
	std::memset((void*)0x688540, 0, 0x12C);
	*(int*)0x688540 = 1;
	*(int*)0x688544 = a1;
	*(int*)0x688550 = 1;
	*(uint8_t*)0x688556 = 8;
	*(uint8_t*)0x688670 = 1;
	*(int*)0x688548 = 1;
	*(uint8_t*)0x688864 = 1;
	*(int*)0x68854C = 1;

	std::memset((void*)0x688A78, 0, 0x288);
	InitXboxCriticalSection(0x688CE4);
	InitXboxCriticalSection(0x67ABF8);
	InitXboxCriticalSection(0x67AA74);
	*(int*)0x69554C |= 8;
	*(int*)0x11384C = 1;
	*(int*)0x67AA70 = 3;

	std::memset((void*)0x695250, 0, 0xF0);
	*(int*)0x695334 = 0x1000000;
	*(int*)0x6952FC = -1;
	InitXboxCriticalSection(0x695318);
	std::memset((void*)0x6D8260, 0, 0x1800);

	std::memset((void*)0x695100, 0, 0x88);
	*(int*)0x695184 = 1;
	InitXboxCriticalSection(0x695168);

	std::memset((void*)0x6D9AC0, 0, 0x84C);
	InitXboxCriticalSection(0x6D9AF0);

	std::memset((void*)0x6D9A60, 0, 0x4C);
	InitXboxCriticalSection(0x6D9A80);

	return 0;
}

// ── CRI ADXF_GetStat hook ────────────────────────────────────────
static uintptr_t g_CriExecServerVA = 0;
static uintptr_t g_CriExecSrv2VA = 0;
static uintptr_t g_CriExecSrv4VA = 0;

static int __cdecl GundamGetStatHook(int handle) {
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

// ── XBE hash constants ───────────────────────────────────────────
static const uint64_t kGundamHashes[] = {
	0x955c9cb503f1520fULL, // gs_gtest.xbe (Gundam BOS test menu)
	0x6b43ff6b6398a88fULL, // gs.xbe       (Gundam BOS main game)
};

bool IsGundamXbe(uint64_t xbeHash)
{
	for (auto h : kGundamHashes) {
		if (xbeHash == h) return true;
	}
	return false;
}

// ── Smart DIMM update hook ────────────────────────────────────────
// sub_98B30 calls sub_98050 (JVS input polling) + slow DIMM functions.
// This hook keeps only the input polling and skips the blocking DIMM I/O.
static uintptr_t g_Sub98050VA = 0; // sub_98050 = JVS input poll + read

static int __cdecl GundamDimmUpdateSmartHook() {
	typedef int(__cdecl* Fn)(void);
	if (g_Sub98050VA) {
		((Fn)g_Sub98050VA)();
	}
	// Throttle: on real hardware the slow DIMM functions (100ms sleep,
	// retry loops) naturally paced this thread.  Without them the thread
	// spins sub_98050 at max speed and pins core 0 at 100%.
	Sleep(1);
	return 0;
}

// ── Patch toggle flags — set to 0 to disable individual patches ──
#define GP_LINKOK           1   // 1: LinkOK JMP hook // NO
#define GP_MBRECVSEND       1   // 2: MbRecv/MbSend JMP hooks // NO
#define GP_DIMM_READY       1   // 3: DIMM-board ready bypass (sub_99110) // NO
#define GP_ERROR5           0   // 4: Error-5 bypass (JZ→JMP) // OK
#define GP_ERROR_CHECKER    1   // 5: Error-checker bypass (sub_64640)
#define GP_INIT_DONE        0   // 6: Init-done bypass (sub_99100) //OK
#define GP_DIMM_INIT        0   // 7: DIMM init hook (sub_99600) // OK
#define GP_THREAD_GATE      0   // 8: Thread-gate bypass (sub_7D420) // OK
#define GP_WARN_TIMER       1   // 9: WARNING timer shortcut
#define GP_CRI_GETSTAT      1   // 10: CRI ADXF_GetStat hook
#define GP_KICKOFF_IDLE     1   // 11: D3D_KickOffAndWaitForIdle stub
#define GP_BLOCK_RESOURCE   1   // 12: D3D_BlockOnResource stub
#define GP_BLOCK_VBLANK     1   // 13: D3DDevice_BlockUntilVerticalBlank stub
#define GP_FLIP_PENDING     1   // 14: CMiniport_IsFlipPending stub
#define GP_DIMM_UPDATE      0   // 15: Smart DIMM update hook — calls sub_98050 + Sleep(1)
#define GP_DIMM_UPDATE_STUB 0   // 15b: Pure stub sub_98B30 (XOR EAX,EAX; RET) — no input, just kill the function
#define GP_DIMM_STATE       0   // 16: DIMM error state bypass (sub_978D0 → XOR EAX,EAX; RET)
#define GP_DIMM_COMM        0   // 17: Stub sub_9BEA0 — OFF (crashes KiWaitTest)
#define GP_MEDIA_RESET      0   // 18: Stub sub_98A70 — OFF
#define GP_CARD_READER      1   // 19: Stub sub_992F0 — OFF
#define GP_FORCE_JVS_INIT   0   // 20: Force JVS init — OFF
#define GP_JVS_DISCOVERY    0   // 21: Stub JVS discovery — OFF

// ── Main patch entry point ───────────────────────────────────────

void ApplyGundamPatches(uint32_t imageSize)
{
	printf("GundamPatch: applying patches (imageSize=0x%X)\n", imageSize);

#if GP_LINKOK
	// === LinkOK — JMP hook to GundamLinkOkHook ===
	{
		static const uint8_t kLinkOkPat[] = {
			0x85,0xC0, 0x75,0x08, 0xB8,0xFE,0xFF,0xFF,0xFF, 0xC2,0x0C,0x00
		};
		auto linkOkHits = ScanXbeAll(kLinkOkPat, sizeof(kLinkOkPat), imageSize);
		for (auto linkOkVA : linkOkHits) {
			if (linkOkVA >= 5 && ((const uint8_t*)(linkOkVA - 5))[0] == 0xA1) {
				PatchWithJmp(linkOkVA - 5, (const void*)&GundamLinkOkHook);
				printf("GundamPatch: LinkOK patched at VA 0x%08X\n", (unsigned)(linkOkVA - 5));
			}
		}
		if (linkOkHits.empty()) {
			printf("GundamPatch: LinkOK pattern not found!\n");
		}
	}
#endif

#if GP_MBRECVSEND
	// === MbRecvPacket / MbSendPacket — JMP hooks ===
	{
		static const uint8_t kMbFuncPat[] = {
			0x83,0xEC,0x08, 0x8D,0x44,0x24,0x04, 0x50, 0x6A,0x00,
			0xE8,0xFF,0xFF,0xFF,0xFF,
			0x8D,0x0C,0x24, 0x51, 0x6A,0x01, 0xE8
		};
		auto mbHits = ScanXbeAll(kMbFuncPat, sizeof(kMbFuncPat), imageSize);
		if (mbHits.size() >= 2) {
			PatchWithJmp(mbHits[0], (const void*)&GundamMbRecvHook);
			PatchWithJmp(mbHits[1], (const void*)&GundamMbSendHook);
			printf("GundamPatch: MbRecv/MbSend hooked at 0x%08X / 0x%08X\n", (unsigned)mbHits[0], (unsigned)mbHits[1]);
		} else {
			printf("GundamPatch: MbRecvPacket/MbSendPacket not found! (%zu hits)\n", mbHits.size());
		}
	}
#endif

#if GP_DIMM_READY
	// === DIMM-board ready bypass (sub_99110 → always return 1) ===
	{
		static const uint8_t kDimmReadyPat[] = {
			0x83,0x3D, 0x50,0x85,0x68,0x00, 0x04,
			0x1B,0xC0, 0x40, 0xC3
		};
		uintptr_t dimmReadyVA = ScanXbe(kDimmReadyPat, sizeof(kDimmReadyPat), imageSize);
		if (dimmReadyVA) {
			static const uint8_t kAlwaysTrue[] = {
				0xB8,0x01,0x00,0x00,0x00, 0xC3,
				0x90,0x90,0x90,0x90,0x90
			};
			PatchXbeBytes(dimmReadyVA, kAlwaysTrue, sizeof(kAlwaysTrue));
			printf("GundamPatch: DIMM-ready patched at 0x%08X\n", (unsigned)dimmReadyVA);
		} else {
			printf("GundamPatch: DIMM-ready pattern not found\n");
		}
	}
#endif

#if GP_ERROR5
	// === Error-5 bypass: patch JZ to JMP ===
	{
		static const uint8_t kErr5Pat[] = {
			0xE8, 0xFF,0xFF,0xFF,0xFF,
			0x80,0x78,0x10,0x01,
			0x74,0x0A,
			0xC7,0x05, 0x94,0xDC,0x22,0x00,
			0x05,0x00,0x00,0x00
		};
		uintptr_t err5VA = ScanXbe(kErr5Pat, sizeof(kErr5Pat), imageSize);
		if (err5VA) {
			static const uint8_t kJmp = 0xEB;
			PatchXbeBytes(err5VA + 9, &kJmp, 1);
			printf("GundamPatch: Error-5 bypass at 0x%08X\n", (unsigned)(err5VA + 9));
		} else {
			printf("GundamPatch: Error-5 pattern not found\n");
		}
	}
#endif

#if GP_ERROR_CHECKER
	// === Error-checker bypass (sub_64640 → XOR EAX,EAX; RET) ===
	{
		static const uint8_t kErrChkPat[] = {
			0x83,0xEC,0x14, 0x56,
			0xE8, 0xFF,0xFF,0xFF,0xFF,
			0x8B,0xF0,
			0xA1, 0x94,0xDC,0x22,0x00,
			0x85,0xC0
		};
		uintptr_t errChkVA = ScanXbe(kErrChkPat, sizeof(kErrChkPat), imageSize);
		if (errChkVA) {
			static const uint8_t kRet0[] = { 0x33,0xC0, 0xC3 };
			PatchXbeBytes(errChkVA, kRet0, sizeof(kRet0));
			printf("GundamPatch: Error-checker bypass at 0x%08X\n", (unsigned)errChkVA);
		} else {
			printf("GundamPatch: Error-checker pattern not found\n");
		}
	}
#endif

#if GP_INIT_DONE
	// === Init-done bypass (sub_99100 → always return 1) ===
	{
		static const uint8_t kInitDonePat[] = {
			0xA1, 0x40,0x85,0x68,0x00,
			0xC3,
			0xCC,0xCC,0xCC,0xCC
		};
		uintptr_t initDoneVA = ScanXbe(kInitDonePat, sizeof(kInitDonePat), imageSize);
		if (initDoneVA) {
			static const uint8_t kRet1[] = { 0xB8,0x01,0x00,0x00,0x00, 0xC3 };
			PatchXbeBytes(initDoneVA, kRet1, sizeof(kRet1));
			printf("GundamPatch: Init-done patched at 0x%08X\n", (unsigned)initDoneVA);
		} else {
			printf("GundamPatch: Init-done pattern not found\n");
		}
	}
#endif

#if GP_DIMM_INIT
	// === DIMM init hook (sub_99600 → GundamDimmInitHook) ===
	{
		static const uint8_t kInitFailPat[] = {
			0xA3, 0x1C,0x84,0x68,0x00,
			0xE8, 0xFF,0xFF,0xFF,0xFF,
			0x85,0xC0,
			0x0F,0x85
		};
		uintptr_t initFailVA = ScanXbe(kInitFailPat, sizeof(kInitFailPat), imageSize);
		if (initFailVA) {
			uintptr_t callVA = initFailVA + 5;
			int32_t callRel = *(int32_t*)(callVA + 1);
			uintptr_t sub99600VA = callVA + 5 + callRel;
			PatchWithJmp(sub99600VA, (const void*)&GundamDimmInitHook);
			printf("GundamPatch: DIMM init hooked at 0x%08X\n", (unsigned)sub99600VA);
		} else {
			printf("GundamPatch: DIMM init pattern not found\n");
		}
	}
#endif

#if GP_THREAD_GATE
	// === Thread-gate bypass (sub_7D420 → MOV EAX,1; RET) ===
	{
		static const uint8_t kThreadGatePat[] = {
			0x51, 0x8B, 0x0D, 0x04, 0x18, 0x67, 0x00,
			0x8D, 0x04, 0x24, 0x50, 0x51,
			0xC7, 0x44, 0x24, 0x08, 0x03, 0x01, 0x00, 0x00
		};
		uintptr_t threadGateVA = ScanXbe(kThreadGatePat, sizeof(kThreadGatePat), imageSize);
		if (threadGateVA) {
			static const uint8_t kRetTrue[] = { 0xB8,0x01,0x00,0x00,0x00, 0xC3 };
			PatchXbeBytes(threadGateVA, kRetTrue, sizeof(kRetTrue));
			printf("GundamPatch: Thread-gate patched at 0x%08X\n", (unsigned)threadGateVA);
		} else {
			printf("GundamPatch: Thread-gate pattern not found\n");
		}
	}
#endif

#if GP_WARN_TIMER
	// === WARNING timer shortcut (600 frames → 1) ===
	{
		static const uint8_t kWarnTimerPat[] = {
			0x8B, 0x15, 0x24, 0xE6, 0x22, 0x00,
			0xC7, 0x02, 0x58, 0x02, 0x00, 0x00,
			0xC3
		};
		uintptr_t warnTimerVA = ScanXbe(kWarnTimerPat, sizeof(kWarnTimerPat), imageSize);
		if (warnTimerVA) {
			static const uint8_t kOne[] = { 0x01, 0x00, 0x00, 0x00 };
			PatchXbeBytes(warnTimerVA + 8, kOne, sizeof(kOne));
			printf("GundamPatch: WARNING timer shortened at 0x%08X\n", (unsigned)warnTimerVA);
		} else {
			printf("GundamPatch: WARNING timer pattern not found\n");
		}
	}
#endif

#if GP_CRI_GETSTAT
	// === CRI ADXF_GetStat hook ===
	{
		static const uint8_t kGetStatPat[] = {
			0x8B, 0x44, 0x24, 0x04,
			0x85, 0xC0,
			0x75, 0x13,
			0x68
		};
		uintptr_t getStatVA = ScanXbe(kGetStatPat, sizeof(kGetStatPat), imageSize);
		if (getStatVA) {
			static const uint8_t kExecPat[] = {
				0xE9, 0x3B, 0x6E, 0x00, 0x00,
				0xCC, 0xCC, 0xCC
			};
			uintptr_t execVA = ScanXbe(kExecPat, sizeof(kExecPat), imageSize);
			if (execVA) {
				int32_t jmpRel = *(int32_t*)(execVA + 1);
				uintptr_t srv5VA = execVA + 5 + jmpRel;
				g_CriExecSrv2VA = srv5VA - 0x20;
				g_CriExecSrv4VA = srv5VA - 0x10;
				g_CriExecServerVA = srv5VA;
				printf("GundamPatch: CRI ExecServer thunks: pri2=0x%08X pri4=0x%08X pri5=0x%08X\n",
					(unsigned)g_CriExecSrv2VA, (unsigned)g_CriExecSrv4VA, (unsigned)g_CriExecServerVA);
			} else {
				printf("GundamPatch: CRI ExecServer thunk not found\n");
			}
			PatchWithJmp(getStatVA, (const void*)&GundamGetStatHook);
			printf("GundamPatch: CRI GetStat hooked at 0x%08X\n", (unsigned)getStatVA);
		} else {
			printf("GundamPatch: CRI GetStat pattern not found\n");
		}
	}
#endif

#if GP_KICKOFF_IDLE
	// === D3D_KickOffAndWaitForIdle NOP-stub ===
	// The original code polls NV2A GPU status registers which are not maintained
	// in HLE mode, causing an infinite busy-wait (~1 FPS). Stub with RET.
	{
		static const uint8_t kKickIdlePat[] = { 0xA1, 0x28, 0x9C, 0x0B, 0x00, 0x8B, 0x48, 0x2C };
		uintptr_t kickIdleVA = ScanXbe(kKickIdlePat, sizeof(kKickIdlePat), imageSize);
		if (kickIdleVA) {
			static const uint8_t kRet = 0xC3;
			PatchXbeBytes(kickIdleVA, &kRet, 1);
			printf("GundamPatch: D3D_KickOffAndWaitForIdle stubbed at 0x%08X\n", (unsigned)kickIdleVA);
		} else {
			printf("GundamPatch: D3D_KickOffAndWaitForIdle pattern NOT FOUND (will cause ~1 FPS!)\n");
		}
	}
#endif

#if GP_BLOCK_RESOURCE
	// === D3D_BlockOnResource NOP-stub ===
	// D3D_BlockOnResource polls NV2A fence registers to wait for a resource to
	// become available. In HLE mode, fences are not maintained so the poll
	// never completes.  The symbol is detected but has no HLE patch registered.
	// Stub with RET 4 (one DWORD arg: X_D3DResource* pResource, __stdcall).
	{
		const uintptr_t kBlockResVA = 0x000AF900; // from symbol cache
		const uint8_t* probe = (const uint8_t*)kBlockResVA;
		// Verify address contains valid code (not already patched or padding)
		if (probe[0] != 0xC2 && probe[0] != 0xC3 && probe[0] != 0xCC) {
			static const uint8_t kRet4[] = { 0xC2, 0x04, 0x00 }; // RET 4
			PatchXbeBytes(kBlockResVA, kRet4, sizeof(kRet4));
			printf("GundamPatch: D3D_BlockOnResource stubbed at 0x%08X\n", (unsigned)kBlockResVA);
		} else {
			printf("GundamPatch: D3D_BlockOnResource prologue mismatch at 0x%08X\n", (unsigned)kBlockResVA);
		}
	}
#endif

	// NOTE: D3D_SetFence, CDevice_KickOff, D3D_MakeRequestedSpace must NOT be
	// stubbed — the HLE rendering pipeline calls them internally.

#if GP_BLOCK_VBLANK
	// === D3DDevice_BlockUntilVerticalBlank NOP-stub ===
	// Busy-waits for VSync by polling NV2A PCRTC registers. The HLE patch for
	// this function was commented out upstream (no implementation). Stub with RET.
	// __stdcall, no params.
	{
		const uintptr_t kBlockVBlankVA = 0x000AB0D0; // from symbol cache
		const uint8_t* probe = (const uint8_t*)kBlockVBlankVA;
		if (probe[0] != 0xC2 && probe[0] != 0xC3) { // not already patched
			static const uint8_t kRet = 0xC3;
			PatchXbeBytes(kBlockVBlankVA, &kRet, 1);
			printf("GundamPatch: D3DDevice_BlockUntilVerticalBlank stubbed at 0x%08X\n", (unsigned)kBlockVBlankVA);
		}
	}
#endif

#if GP_FLIP_PENDING
	// === CMiniport_IsFlipPending NOP-stub ===
	// Polls NV2A for pending display flips. In HLE mode, return 0 (not pending).
	// __thiscall (ECX = this), no stack args. Stub with XOR EAX,EAX; RET.
	{
		const uintptr_t kFlipPendVA = 0x000B20A0; // from symbol cache
		const uint8_t* probe = (const uint8_t*)kFlipPendVA;
		if (probe[0] != 0x33 && probe[0] != 0xC2 && probe[0] != 0xC3) { // not already patched
			static const uint8_t kRet0[] = { 0x33, 0xC0, 0xC3 }; // XOR EAX,EAX; RET
			PatchXbeBytes(kFlipPendVA, kRet0, sizeof(kRet0));
			printf("GundamPatch: CMiniport_IsFlipPending stubbed at 0x%08X\n", (unsigned)kFlipPendVA);
		}
	}
#endif

#if GP_DIMM_UPDATE
	// === Smart DIMM update hook (sub_98B30 → GundamDimmUpdateSmartHook) ===
	// sub_98B30 calls sub_98050 (JVS input poll) + slow DIMM functions
	// (sub_9BEA0, sub_992F0, sub_98A70). Hook replaces sub_98B30 with a
	// lightweight version that only calls sub_98050 for input reading.
	{
		const uintptr_t sub98B30 = 0x00098B30;
		const uint8_t* p = (const uint8_t*)sub98B30;
		// Verify: first CALL inside sub_98B30 targets sub_99100
		bool verified = false;
		for (int off = 0; off < 8; off++) {
			if (p[off] == 0xE8) {
				int32_t rel = *(int32_t*)(sub98B30 + off + 1);
				uintptr_t target = sub98B30 + off + 5 + rel;
				if (target == 0x00099100) { verified = true; break; }
			}
		}
		if (verified) {
			// Resolve sub_98050 (JVS input polling) from call inside sub_98B30
			// sub_98050 is the first call after the if(sub_99100()) check
			g_Sub98050VA = 0x00098050;
			PatchWithJmp(sub98B30, (const void*)&GundamDimmUpdateSmartHook);
			printf("GundamPatch: Smart DIMM update hook at 0x%08X (sub_98050=0x%08X)\n",
				(unsigned)sub98B30, (unsigned)g_Sub98050VA);
		} else {
			printf("GundamPatch: DIMM update pattern mismatch at 0x%08X\n", (unsigned)sub98B30);
		}
	}
#endif

#if GP_DIMM_UPDATE_STUB
	// === Pure DIMM update stub (sub_98B30 → XOR EAX,EAX; RET) ===
	// Kills the entire DIMM update function. No input polling, no DIMM I/O.
	// Use with GP_DIMM_STATE to test if speed returns to full.
	{
		const uintptr_t sub98B30 = 0x00098B30;
		const uint8_t* p = (const uint8_t*)sub98B30;
		if (p[0] != 0x33 && p[0] != 0xC3) {
			static const uint8_t kRet0[] = { 0x33, 0xC0, 0xC3 };
			PatchXbeBytes(sub98B30, kRet0, sizeof(kRet0));
			printf("GundamPatch: DIMM update STUBBED at 0x%08X\n", (unsigned)sub98B30);
		}
	}
#endif

#if GP_DIMM_STATE
	// === DIMM error state bypass (sub_978D0 → XOR EAX,EAX; RET) ===
	{
		const uintptr_t sub978D0 = 0x000978D0;
		static const uint8_t kRet0[] = { 0x33, 0xC0, 0xC3 };
		PatchXbeBytes(sub978D0, kRet0, sizeof(kRet0));
		printf("GundamPatch: DIMM state check stubbed at 0x%08X\n", (unsigned)sub978D0);
	}
#endif

#if GP_DIMM_COMM
	// === DIMM board communication stub (sub_9BEA0 → XOR EAX,EAX; RET) ===
	// Uses RtlEnterCriticalSection/LeaveCriticalSection in loops; can contend.
	{
		const uintptr_t sub9BEA0 = 0x0009BEA0;
		const uint8_t* probe = (const uint8_t*)sub9BEA0;
		if (probe[0] != 0xC3 && probe[0] != 0xCC) {
			static const uint8_t kRet0[] = { 0x33, 0xC0, 0xC3 };
			PatchXbeBytes(sub9BEA0, kRet0, sizeof(kRet0));
			printf("GundamPatch: DIMM comm stubbed at 0x%08X\n", (unsigned)sub9BEA0);
		} else {
			printf("GundamPatch: DIMM comm already patched at 0x%08X\n", (unsigned)sub9BEA0);
		}
	}
#endif

#if GP_MEDIA_RESET
	// === Media reset stub (sub_98A70 → XOR EAX,EAX; RET) ===
	// Contains 100ms Sleep — major performance killer.
	{
		const uintptr_t sub98A70 = 0x00098A70;
		const uint8_t* probe = (const uint8_t*)sub98A70;
		if (probe[0] != 0xC3 && probe[0] != 0xCC) {
			static const uint8_t kRet0[] = { 0x33, 0xC0, 0xC3 };
			PatchXbeBytes(sub98A70, kRet0, sizeof(kRet0));
			printf("GundamPatch: Media reset stubbed at 0x%08X\n", (unsigned)sub98A70);
		} else {
			printf("GundamPatch: Media reset already patched at 0x%08X\n", (unsigned)sub98A70);
		}
	}
#endif

#if GP_CARD_READER
	// === Card reader state machine stub (sub_992F0 → XOR EAX,EAX; RET) ===
	// 6-state machine with retry loops (up to 60 retries). Not needed for gameplay.
	{
		const uintptr_t sub992F0 = 0x000992F0;
		const uint8_t* probe = (const uint8_t*)sub992F0;
		if (probe[0] != 0xC3 && probe[0] != 0xCC) {
			static const uint8_t kRet0[] = { 0x33, 0xC0, 0xC3 };
			PatchXbeBytes(sub992F0, kRet0, sizeof(kRet0));
			printf("GundamPatch: Card reader stubbed at 0x%08X\n", (unsigned)sub992F0);
		} else {
			printf("GundamPatch: Card reader already patched at 0x%08X\n", (unsigned)sub992F0);
		}
	}
#endif

#if GP_FORCE_JVS_INIT
	// === Force JVS init state ===
	// dword_675EE8=1: sub_97C80 won't early-exit
	// byte_695A59=1:  JVS node count (1 node)
	*(volatile int*)0x675EE8 = 1;
	*(volatile char*)0x695A59 = 1;
	printf("GundamPatch: Forced JVS init (dword_675EE8=1, node_count=1)\n");
#endif

#if GP_JVS_DISCOVERY
	// === Stub JVS discovery loop (sub_98C50 → RET 0) ===
	// sub_98C50 loops: while(sub_978D0()==5) sub_98910();
	// sub_978D0 is the JVS node discovery state machine that does hardware I/O.
	// On emulated hardware it loops forever. Since we force the init state above,
	// this function is not needed.
	{
		const uintptr_t sub98C50 = 0x00098C50;
		const uint8_t* probe = (const uint8_t*)sub98C50;
		if (probe[0] != 0xC3 && probe[0] != 0xCC) {
			static const uint8_t kRet0[] = { 0x33, 0xC0, 0xC3 }; // XOR EAX,EAX; RET
			PatchXbeBytes(sub98C50, kRet0, sizeof(kRet0));
			printf("GundamPatch: JVS discovery loop stubbed at 0x%08X\n", (unsigned)sub98C50);
		}
	}
#endif

	printf("GundamPatch: all patches applied\n");
}
