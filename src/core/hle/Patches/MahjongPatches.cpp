// ******************************************************************
// *  Cxbx Sega Network Taisen Mahjong MJ2 / MJ3 / MJ3 Evolution patches
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
#include "common\xbox_types.h"
#include "core\kernel\support\Emu.h"

#include <map>
#include <string>
#include <cstdio>
#include <cstring>

extern std::map<std::string, xbox::addr_xt> g_SymbolAddresses;

int g_ChihiroMjGame = 0;  // 0=none, 1=MJ2, 2=MJ3, 3=MJ3Evo

// Keep bring-up details in CXBXR's normal debug logger. Synchronous console
// and C:\temp writes make the Release path unnecessarily expensive under Wine.
#if defined(_DEBUG)
#define MJ_LOG(...) EmuLog(LOG_LEVEL::DEBUG, __VA_ARGS__)
#else
#define MJ_LOG(...) do {} while(0)
#endif

// ── Feature toggles (1=enabled, 0=disabled) ─────────────────────
#define MJ_LINKOK           1
#define MJ_MBRECVSEND       1
#define MJ_DIMM_READY       1
#define MJ_KICKOFF_IDLE     1  // ON: stub NV2A GPU poll
#define MJ_CRI_GETSTAT      1
#define MJ_ERROR_CHECKER    1
#define MJ_CARD_READER      1
#define MJ_BLOCK_RESOURCE   1  // ON: stub D3D resource block
#define MJ_BLOCK_VBLANK     1  // ON: stub VBlank wait
#define MJ_FLIP_PENDING     1  // ON: stub flip pending
#define MJ_WARN_TIMER       1
#define MJ_JVS_DISCOVERY    0  // let CXBXR's JVS HLE populate the node table
#define MJ_NET_PATCHES      1
#define MJ_DIMM_VERSION     1  // expose the minimum supported virtual DIMM firmware
#define MJ_BASEBOARD_INIT   1  // skip fatal error on baseboard init failure
#define MJ_BOARD_READY      1  // skip bit-0x20 check in periodic update
#define MJ_DIMM_INIT_SKIP   0  // DISABLED: causes crash - game depends on sub_11CD0 state
#define MJ_DIMM_POLL_SKIP   1  // skip sub_1647A0 (DIMM poll) so sub_11CD0 can create thread
#define MJ_STARTUP_RESOURCE 1  // skip DHCP state machine (sub_106D40 → set done immediately)
#define MJ_STARTUP_OFFLINE  1  // finish MJ3/Evo startup without the retired arcade server
#define MJ_EVO_VBLANK_WAIT  1  // use one real HLE vblank instead of deadlocking on two

// ── XBE hash constants ───────────────────────────────────────────

static const uint64_t kMJ2Hashes[] = {
	0xC247BC88048BB52DULL, // MJ2 (GDX-0006G) raw file hash
	0x0447D06E7FB8EDDBULL, // MJ2 (GDX-0006G) runtime hash
};
static const uint64_t kMJ3Hashes[] = {
	0x7FA91F91C2C61B70ULL, // MJ3 (GDX-0017F) raw file hash
	0xE28A59989196562BULL, // MJ3 (GDX-0017F) runtime hash
};
static const uint64_t kMJ3EvoHashes[] = {
	0xA7ED052E617DC07EULL, // MJ3 Evo (GDX-0021B) raw file hash
	0x44310A3536875E46ULL, // MJ3 Evo (GDX-0021B) runtime hash
};

enum MjGame { MJ_NONE, MJ_2, MJ_3, MJ_3EVO };

static MjGame IdentifyMjGame(uint64_t xbeHash)
{
	for (auto h : kMJ2Hashes)    if (xbeHash == h) return MJ_2;
	for (auto h : kMJ3Hashes)    if (xbeHash == h) return MJ_3;
	for (auto h : kMJ3EvoHashes) if (xbeHash == h) return MJ_3EVO;
	return MJ_NONE;
}

// ── Detection ────────────────────────────────────────────────────

bool IsMahjongXbe(uint64_t xbeHash)
{
	return IdentifyMjGame(xbeHash) != MJ_NONE;
}

// ── Hook functions ────────────────────────────────────────────────

static int __stdcall MjMbRecvHook(uint32_t a1, uint32_t a2, uint32_t a3) {
	return 0; // no data
}

static int __stdcall MjMbSendHook(uint32_t a1, uint32_t a2, uint32_t a3) {
	return 0;
}

static int __stdcall MjLinkOkHook(uint32_t a1, uint32_t a2, uint32_t a3) {
	return 1; // link is OK
}

static void* __cdecl MjGetBoardInfoHook()
{
	uint8_t* boardInfo = nullptr;
	if (g_ChihiroMjGame == MJ_3) {
		boardInfo = reinterpret_cast<uint8_t*>(0x00B5A77C);
	} else if (g_ChihiroMjGame == MJ_3EVO) {
		boardInfo = reinterpret_cast<uint8_t*>(0x00D9BF94);
	}

	if (boardInfo != nullptr) {
		// The arcade DIMM query is unavailable without the physical board.
		// Its library still reaches the ready state, but leaves the reported
		// firmware at 0.0. Both titles require at least 19.5 and wait for
		// these exact two bytes during startup.
		if (boardInfo[20] < 19 || (boardInfo[20] == 19 && boardInfo[21] < 5)) {
			boardInfo[20] = 19;
			boardInfo[21] = 5;
		}
	}

	return boardInfo;
}

// CRI ADXF_GetStat hook — drive CRI exec servers between poll iterations
static int __cdecl MjGetStatHook(int handle) {
	int stat = -3;
	if (handle) {
		stat = *(char*)(handle + 1);
	}
	if (stat != 3) {
		if (handle) stat = *(char*)(handle + 1);
		Sleep(0);
	}
	return stat;
}

// ── Helper ───────────────────────────────────────────────────────

static void PatchDword(uintptr_t va, uint32_t value)
{
	PatchXbeBytes(va, reinterpret_cast<const uint8_t*>(&value), sizeof(value));
}

static bool PatchExpectedBytes(
	uintptr_t va,
	const uint8_t* expected,
	const uint8_t* replacement,
	size_t size,
	const char* label)
{
	if (std::memcmp(reinterpret_cast<const void*>(va), expected, size) != 0) {
		MJ_LOG("%s validation failed at 0x%08X", label, (unsigned)va);
		return false;
	}

	PatchXbeBytes(va, replacement, size);
	MJ_LOG("%s patched at 0x%08X", label, (unsigned)va);
	return true;
}

static uintptr_t LookupSymbolAddress(const char* name)
{
	auto it = g_SymbolAddresses.find(name);
	if (it != g_SymbolAddresses.end() && it->second != 0) {
		return it->second;
	}
	return 0;
}

static void InitXboxCriticalSection(uintptr_t address)
{
	// The Sega arcade library normally initializes this object from its DIMM
	// bring-up path. Our hardware-poll bypass can start acThread without that
	// initializer, leaving the embedded event's wait list invalid.
	std::memset((void*)address, 0, 0x1C);
	*(uint8_t*)(address + 0x00) = 1; // SynchronizationEvent
	*(uint8_t*)(address + 0x02) = 4; // sizeof(KEVENT) / sizeof(LONG)
	*(uint32_t*)(address + 0x08) = (uint32_t)(address + 0x08);
	*(uint32_t*)(address + 0x0C) = (uint32_t)(address + 0x08);
	*(int32_t*)(address + 0x10) = -1; // unlocked
}

// ── Main patch entry point ───────────────────────────────────────

void ApplyMahjongPatches(uint64_t xbeHash, uint32_t imageSize)
{
	MjGame game = IdentifyMjGame(xbeHash);
	g_ChihiroMjGame = (int)game;  // expose for D3D swap hook
	const char* gameName = "???";
	if (game == MJ_2)    gameName = "MJ2 (GDX-0006G)";
	if (game == MJ_3)    gameName = "MJ3 (GDX-0017F)";
	if (game == MJ_3EVO) gameName = "MJ3 Evo (GDX-0021B)";
	MJ_LOG("Applying patches for %s (imageSize=0x%X)", gameName, imageSize);

	if (game == MJ_2) {
		// acThread_Update enters/leaves these library critical sections.
		// Without the skipped hardware initializers, KeSetEvent follows a
		// corrupt WaitListHead and faults inside KiWaitTest.
		const uintptr_t criticalSections[] = {
			0x007758E8,
			0x00776274,
		};
		for (uintptr_t address : criticalSections) {
			InitXboxCriticalSection(address);
			MJ_LOG("Initialized MJ2 acThread critical section at 0x%08X",
				(unsigned)address);
		}
	}

	// === Symbol diagnostics ===
	{
		const char* syms[] = {
			"D3DDevice_Swap", "D3DDevice_Swap_0",
			"D3DDevice_Present", "D3DDevice_BeginScene",
			"D3DDevice_EndScene", "D3DDevice_Clear",
			"D3D_BlockOnResource", "D3D_BlockOnResource_0__LTCG_eax1",
			"D3DDevice_BlockUntilVerticalBlank",
			"CMiniport_IsFlipPending",
			"D3DDevice_SetRenderTarget",
			"Direct3D_CreateDevice", "Direct3D_CreateDevice_16__LTCG_eax_x",
			nullptr
		};
		for (int i = 0; syms[i]; i++) {
			uintptr_t a = LookupSymbolAddress(syms[i]);
			if (a) MJ_LOG("  sym %-48s = 0x%08X", syms[i], (unsigned)a);
		}
	}

	// ═══════════════════════════════════════════════════════════════
	// CRITICAL: Force Chihiro board detection
	// The game uses sub_8AF80() → return (dword_4D4248 == 1)
	// dword_4D4248 is set from dword_4D5CEC which is set by a memory
	// size check (>64MB = Chihiro). If the emulator reports standard
	// Xbox memory, the baseboard init is completely skipped.
	// Pattern: MOV ECX,[dword_4D4248]; XOR EAX,EAX; CMP ECX,1; SETE AL; RET
	// ═══════════════════════════════════════════════════════════════
	{
		static const uint8_t kBoardDetectPat[] = {
			0x8B, 0x0D, 0xFF,0xFF,0xFF,0xFF,  // MOV ECX, [xxxx]
			0x33, 0xC0,                         // XOR EAX, EAX
			0x83, 0xF9, 0x01,                   // CMP ECX, 1
			0x0F, 0x94, 0xC0,                   // SETE AL
			0xC3                                 // RET
		};
		uintptr_t boardDetectVA = ScanXbe(kBoardDetectPat, sizeof(kBoardDetectPat), imageSize);
		if (boardDetectVA) {
			// Patch to: MOV EAX,1; RET; NOP...
			static const uint8_t kAlways1[] = {
				0xB8, 0x01, 0x00, 0x00, 0x00,  // MOV EAX, 1
				0xC3,                            // RET
				0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90
			};
			PatchXbeBytes(boardDetectVA, kAlways1, sizeof(kAlways1));
			// Verify patch was applied
			uint8_t verify0 = ((volatile uint8_t*)boardDetectVA)[0];
			uint8_t verify1 = ((volatile uint8_t*)boardDetectVA)[1];
			MJ_LOG("Chihiro board detection forced at 0x%08X (verify: %02X %02X)", (unsigned)boardDetectVA, verify0, verify1);
		} else {
			MJ_LOG("Chihiro board detection pattern not found!");
		}
	}

#if MJ_BASEBOARD_INIT
	// ═══════════════════════════════════════════════════════════════
	// Baseboard init error bypass
	// When sub_11CD0 (DIMM/JVS init) fails, the caller sets
	// dword_4D4284 = -2 which permanently blocks rendering.
	// By changing the JZ (skip error) to JMP (always skip), we leave
	// dword_4D4284 at 0. The periodic check function then sees 0,
	// calls sub_8AF80 (which we already patched to return 1), and
	// transitions dword_4D4284 to 2 (fully initialized).
	// Pattern: CALL sub_11CD0; TEST EAX,EAX; JZ +17; PUSH str;
	//          MOV [dword_4D4284], -2
	// ═══════════════════════════════════════════════════════════════
	{
		static const uint8_t kBaseboardInitPat[] = {
			0xE8, 0xFF,0xFF,0xFF,0xFF,         // CALL sub_11CD0
			0x85, 0xC0,                         // TEST EAX, EAX
			0x74, 0x17,                         // JZ +0x17 (skip error)
			0x68, 0xFF,0xFF,0xFF,0xFF,          // PUSH "Fatal error..."
			0xC7, 0x05, 0xFF,0xFF,0xFF,0xFF,    // MOV [dword_4D4284],
			0xFE                                // -2 (low byte)
		};
		uintptr_t bbInitVA = ScanXbe(kBaseboardInitPat, sizeof(kBaseboardInitPat), imageSize);
		if (bbInitVA) {
			// Patch byte at offset 7 (JZ 0x74) → JMP (0xEB)
			uint8_t jmp = 0xEB;
			PatchXbeBytes(bbInitVA + 7, &jmp, 1);
			MJ_LOG("Baseboard init error bypassed at 0x%08X", (unsigned)bbInitVA);
		} else {
			MJ_LOG("Baseboard init error pattern not found!");
		}
	}
#endif

#if MJ_BOARD_READY
	// ═══════════════════════════════════════════════════════════════
	// Board-ready bit bypass
	// The periodic update (sub_8B1C0) checks dword_4D4288 & 0x20.
	// Bit 0x20 is set by callback sub_8AFA0 running in the JVS
	// background thread (sub_11BB0). If sub_11CD0 fails, the thread
	// is never created → bit never set → rendering never enters.
	// Fix: NOP the JZ so the update always continues.
	// Pattern: TEST BL,0x20; JZ near; CALL sub_165E30;
	//          MOV [dword_4D428C], EAX; MOV [dword_4D424C], 1
	// ═══════════════════════════════════════════════════════════════
	{
		static const uint8_t kBoardReadyPat[] = {
			0xF6, 0xC3, 0x20,                     // TEST BL, 0x20
			0x0F, 0x84, 0xFF,0xFF,0xFF,0xFF,       // JZ near (to skip)
			0xE8, 0xFF,0xFF,0xFF,0xFF,              // CALL sub_165E30
			0xA3, 0xFF,0xFF,0xFF,0xFF,              // MOV [dword_4D428C], EAX
			0xC7, 0x05, 0xFF,0xFF,0xFF,0xFF,        // MOV [dword_4D424C],
			0x01, 0x00, 0x00, 0x00                  // 1
		};
		uintptr_t boardReadyVA = ScanXbe(kBoardReadyPat, sizeof(kBoardReadyPat), imageSize);
		if (boardReadyVA) {
			// NOP the JZ near (6 bytes at offset 3)
			static const uint8_t kNop6[] = { 0x90,0x90,0x90,0x90,0x90,0x90 };
			PatchXbeBytes(boardReadyVA + 3, kNop6, sizeof(kNop6));
			MJ_LOG("Board-ready check bypassed at 0x%08X", (unsigned)boardReadyVA);
		} else {
			MJ_LOG("Board-ready check pattern not found!");
		}
	}
#endif

#if MJ_DIMM_INIT_SKIP
	// ═══════════════════════════════════════════════════════════════
	// DIMM/JVS init skip (sub_11CD0 → return 0)
	// sub_11CD0 calls sub_1647A0 which polls DIMM board hardware.
	// This always fails in emulation, preventing the JVS background
	// thread from being created. By returning 0 immediately, the
	// caller proceeds as if init succeeded. The background thread
	// won't exist but our other patches compensate.
	// Pattern: MOV EAX,[acLib]; TEST EAX,EAX; JZ far; MOV ECX,[ESP+C]; MOV EDX,[ESP+8]; XOR EAX,EAX
	// ═══════════════════════════════════════════════════════════════
	{
		static const uint8_t kDimmInitPat[] = {
			0xA1, 0xFF,0xFF,0xFF,0xFF,         // MOV EAX, [off_27CEFC]
			0x85, 0xC0,                         // TEST EAX, EAX
			0x0F, 0x84, 0xFF,0xFF,0xFF,0xFF,    // JZ far
			0x8B, 0x4C, 0x24, 0x0C,             // MOV ECX, [ESP+0C]
			0x8B, 0x54, 0x24, 0x08,             // MOV EDX, [ESP+08]
			0x33, 0xC0                           // XOR EAX, EAX
		};
		uintptr_t dimmInitVA = ScanXbe(kDimmInitPat, sizeof(kDimmInitPat), imageSize);
		if (dimmInitVA) {
			// Patch to: XOR EAX,EAX; RET 0Ch (__stdcall, 3 params)
			static const uint8_t kRet0[] = { 0x33, 0xC0, 0xC2, 0x0C, 0x00 };
			PatchXbeBytes(dimmInitVA, kRet0, sizeof(kRet0));
			MJ_LOG("DIMM/JVS init skipped at 0x%08X", (unsigned)dimmInitVA);
		} else {
			MJ_LOG("DIMM/JVS init pattern not found!");
		}
	}
#endif

#if MJ_DIMM_POLL_SKIP
	// ═══════════════════════════════════════════════════════════════
	// DIMM poll skip (sub_1647A0 → return 0)
	// sub_1647A0 polls physical DIMM board hardware for ~4.8 seconds
	// then fails. By returning 0 (success), sub_11CD0 proceeds to:
	//  - run JVS discovery (patched to exit immediately)
	//  - get JVS node count (0)
	//  - create background thread sub_11BB0
	// The thread sets bit 0x20 in dword_4D4288, properly transitioning
	// the board state machine. This eliminates the 4.8s startup delay
	// and creates proper JVS state.
	// sub_1647A0 is __stdcall with 3 params.
	// Pattern: SUB ESP,38h; PUSH EBX; PUSH EBP; PUSH ESI; MOV ESI,[ESP+50h];
	//          PUSH EDI; XOR EAX,EAX; XOR EBP,EBP
	// ═══════════════════════════════════════════════════════════════
	{
		static const uint8_t kDimmPollPat[] = {
			0x83, 0xEC, 0x38,                   // SUB ESP, 38h
			0x53,                                // PUSH EBX
			0x55,                                // PUSH EBP
			0x56,                                // PUSH ESI
			0x8B, 0x74, 0x24, 0x50,             // MOV ESI, [ESP+50h]
			0x57,                                // PUSH EDI
			0x33, 0xC0,                          // XOR EAX, EAX
			0x33, 0xED,                          // XOR EBP, EBP
		};
		uintptr_t dimmPollVA = ScanXbe(kDimmPollPat, sizeof(kDimmPollPat), imageSize);
		if (dimmPollVA) {
			// Patch to: XOR EAX,EAX; RET 0Ch (__stdcall, 3 params)
			static const uint8_t kRet0[] = { 0x33, 0xC0, 0xC2, 0x0C, 0x00 };
			PatchXbeBytes(dimmPollVA, kRet0, sizeof(kRet0));
			MJ_LOG("DIMM poll skipped at 0x%08X", (unsigned)dimmPollVA);
		} else {
			MJ_LOG("DIMM poll pattern not found!");
		}
	}
#endif

#if MJ_DIMM_VERSION
	// MJ3/MJ3 Evolution complete their library state machine without the
	// physical DIMM board, but its firmware query returns 0.0. Hook the tiny
	// board-info accessor so every consumer sees the minimum accepted 19.5
	// version while preserving all other asynchronously populated fields.
	if (game == MJ_3) {
		PatchWithJmp(0x00232FE0, (const void*)&MjGetBoardInfoHook);
		MJ_LOG("MJ3 virtual DIMM firmware accessor hooked at 0x00232FE0");
	} else if (game == MJ_3EVO) {
		PatchWithJmp(0x002F1180, (const void*)&MjGetBoardInfoHook);
		MJ_LOG("MJ3 Evo virtual DIMM firmware accessor hooked at 0x002F1180");
	}
#endif

#if MJ_EVO_VBLANK_WAIT
	// Evolution registers sub_265CF0 as its vblank callback and then asks the
	// renderer to wait for two callbacks before continuing. CXBXR's current HLE
	// path supplies the first callback from Swap, but the title blocks before it
	// can return to the next Swap and request the second one. Keep the real
	// callback/event pacing and change only the startup target to one vblank.
	if (game == MJ_3EVO) {
		static const uint8_t kTwoVblanks[] = { 0x6A, 0x02 };
		static const uint8_t kOneVblank[] = { 0x6A, 0x01 };
		PatchExpectedBytes(
			0x00265E7C,
			kTwoVblanks,
			kOneVblank,
			sizeof(kOneVblank),
			"MJ3 Evolution vblank wait target");
	}
#endif

	// ═══════════════════════════════════════════════════════════════
	// Pattern-based patches (shared Sega Chihiro library code)
	// These use the same byte patterns as Gundam/Golf since all
	// Chihiro games link the same MediaBoard/DIMM/JVS library.
	// ═══════════════════════════════════════════════════════════════

#if MJ_LINKOK
	// === 1. LinkOK — JMP hook to return "link is OK" ===
	// Pattern: TEST EAX,EAX; JNZ +8; MOV EAX,-2; RET 0Ch
	// The function starts 5 bytes earlier with MOV EAX,[addr].
	{
		static const uint8_t kLinkOkPat[] = {
			0x85,0xC0, 0x75,0x08, 0xB8,0xFE,0xFF,0xFF,0xFF, 0xC2,0x0C,0x00
		};
		auto linkOkHits = ScanXbeAll(kLinkOkPat, sizeof(kLinkOkPat), imageSize);
		for (auto linkOkVA : linkOkHits) {
			if (linkOkVA >= 5 && ((const uint8_t*)(linkOkVA - 5))[0] == 0xA1) {
				PatchWithJmp(linkOkVA - 5, (const void*)&MjLinkOkHook);
				MJ_LOG("LinkOK patched at 0x%08X", (unsigned)(linkOkVA - 5));
			}
		}
		if (linkOkHits.empty()) MJ_LOG("LinkOK pattern not found");
	}
#endif

#if MJ_MBRECVSEND
	// === 2. MbRecvPacket / MbSendPacket — JMP hooks ===
	// Blocks on LPC bus handshake with MediaBoard. Stub to prevent spin.
	{
		static const uint8_t kMbFuncPat[] = {
			0x83,0xEC,0x08, 0x8D,0x44,0x24,0x04, 0x50, 0x6A,0x00,
			0xE8,0xFF,0xFF,0xFF,0xFF,
			0x8D,0x0C,0x24, 0x51, 0x6A,0x01, 0xE8
		};
		auto mbHits = ScanXbeAll(kMbFuncPat, sizeof(kMbFuncPat), imageSize);
		if (mbHits.size() >= 2) {
			PatchWithJmp(mbHits[0], (const void*)&MjMbRecvHook);
			PatchWithJmp(mbHits[1], (const void*)&MjMbSendHook);
			MJ_LOG("MbRecv/MbSend hooked at 0x%08X / 0x%08X",
				(unsigned)mbHits[0], (unsigned)mbHits[1]);
		} else {
			MJ_LOG("MbRecv/MbSend not found (%zu hits)", mbHits.size());
		}
	}
#endif

#if MJ_DIMM_READY
	// === 3. DIMM-board ready bypass (always return 1) ===
	// Pattern: CMP dword ptr [addr], 4; SBB EAX,EAX; INC EAX; RET
	{
		static const uint8_t kDimmReadyPat[] = {
			0x83,0x3D, 0xFF,0xFF,0xFF,0xFF, 0x04,
			0x1B,0xC0, 0x40, 0xC3
		};
		uintptr_t dimmReadyVA = ScanXbe(kDimmReadyPat, sizeof(kDimmReadyPat), imageSize);
		if (dimmReadyVA) {
			static const uint8_t kAlwaysTrue[] = {
				0xB8,0x01,0x00,0x00,0x00, 0xC3,
				0x90,0x90,0x90,0x90,0x90
			};
			PatchXbeBytes(dimmReadyVA, kAlwaysTrue, sizeof(kAlwaysTrue));
			MJ_LOG("DIMM-ready patched at 0x%08X", (unsigned)dimmReadyVA);
		} else {
			MJ_LOG("DIMM-ready pattern not found");
		}
	}
#endif

#if MJ_KICKOFF_IDLE
	// === 4. D3D_KickOffAndWaitForIdle NOP-stub ===
	// Polls NV2A GPU status registers — infinite busy-wait in HLE mode.
	{
		static const uint8_t kKickIdlePat[] = {
			0xA1, 0xFF,0xFF,0xFF,0xFF, 0x8B, 0x48, 0x2C
		};
		uintptr_t kickIdleVA = ScanXbe(kKickIdlePat, sizeof(kKickIdlePat), imageSize);
		if (kickIdleVA) {
			static const uint8_t kRet = 0xC3;
			PatchXbeBytes(kickIdleVA, &kRet, 1);
			MJ_LOG("D3D_KickOffAndWaitForIdle stubbed at 0x%08X", (unsigned)kickIdleVA);
		} else {
			MJ_LOG("D3D_KickOffAndWaitForIdle pattern not found");
		}
	}
#endif

#if MJ_CRI_GETSTAT
	// === 5. CRI ADXF_GetStat hook ===
	//
	// MJ2/MJ3 use a tiny leaf implementation, while Evolution wraps the same
	// state read in the CRI critical-section enter/leave calls. The old
	// nine-byte scan also occurs inside Evolution's DirectSound-buffer
	// constructor at 0x002BE211. Hooking that interior instruction bypassed the
	// constructor epilogue and corrupted ESP, eventually returning through the
	// mbcom handle at 0x00DA1504. Validate the complete leaf shape for MJ2/MJ3
	// and the complete wrapper shape at Evolution's known entry before writing.
	{
		uintptr_t getStatVA = 0;
		if (game == MJ_3EVO) {
			const uintptr_t candidateVA = 0x002B8BF0;
			const uint8_t* const candidate =
				reinterpret_cast<const uint8_t*>(candidateVA);
			static const uint8_t kWrapperHead[] = {
				0x56, 0xE8
			};
			static const uint8_t kWrapperBody[] = {
				0x8B, 0x44, 0x24, 0x08,
				0x85, 0xC0,
				0x75, 0x1B,
				0x68
			};
			static const uint8_t kNullTail[] = {
				0x83, 0xC4, 0x04,
				0xBE, 0xFD, 0xFF, 0xFF, 0xFF,
				0xE8
			};
			static const uint8_t kReturnTail[] = {
				0x8B, 0xC6, 0x5E, 0xC3,
				0x0F, 0xBE, 0x70, 0x01,
				0xE8
			};
			static const uint8_t kFinalReturn[] = {
				0x8B, 0xC6, 0x5E, 0xC3
			};
			const bool validWrapper =
				std::memcmp(candidate, kWrapperHead, sizeof(kWrapperHead)) == 0 &&
				std::memcmp(candidate + 6, kWrapperBody, sizeof(kWrapperBody)) == 0 &&
				candidate[19] == 0xE8 &&
				std::memcmp(candidate + 24, kNullTail, sizeof(kNullTail)) == 0 &&
				std::memcmp(candidate + 37, kReturnTail, sizeof(kReturnTail)) == 0 &&
				std::memcmp(candidate + 50, kFinalReturn, sizeof(kFinalReturn)) == 0;
			if (validWrapper) {
				getStatVA = candidateVA;
			} else {
				MJ_LOG("MJ3 Evolution CRI GetStat validation failed at 0x%08X",
					(unsigned)candidateVA);
			}
		} else {
			static const uint8_t kGetStatLeafPattern[] = {
				0x8B, 0x44, 0x24, 0x04,
				0x85, 0xC0,
				0x75, 0x13,
				0x68
			};
			auto candidates = ScanXbeAll(
				kGetStatLeafPattern,
				sizeof(kGetStatLeafPattern),
				imageSize,
				8);
			static const uint8_t kLeafNullTail[] = {
				0x83, 0xC4, 0x04,
				0xB8, 0xFD, 0xFF, 0xFF, 0xFF,
				0xC3,
				0x0F, 0xBE, 0x40, 0x01,
				0xC3
			};
			for (uintptr_t candidateVA : candidates) {
				const uint8_t* const candidate =
					reinterpret_cast<const uint8_t*>(candidateVA);
				if (candidate[13] == 0xE8 &&
					std::memcmp(
						candidate + 18,
						kLeafNullTail,
						sizeof(kLeafNullTail)) == 0) {
					getStatVA = candidateVA;
					break;
				}
			}
		}

		if (getStatVA) {
			PatchWithJmp(getStatVA, (const void*)&MjGetStatHook);
			MJ_LOG("CRI GetStat hooked at 0x%08X", (unsigned)getStatVA);
		} else {
			MJ_LOG("CRI GetStat pattern not found");
		}
	}
#endif

#if MJ_ERROR_CHECKER
	// === 6. Error checker bypass ===
	// Pattern: SUB ESP,14h; PUSH ESI; CALL xx; MOV ESI,EAX; MOV EAX,[xx]; TEST EAX,EAX
	{
		static const uint8_t kErrChkPat[] = {
			0x83,0xEC,0x14, 0x56,
			0xE8, 0xFF,0xFF,0xFF,0xFF,
			0x8B,0xF0,
			0xA1, 0xFF,0xFF,0xFF,0xFF,
			0x85,0xC0
		};
		uintptr_t errChkVA = ScanXbe(kErrChkPat, sizeof(kErrChkPat), imageSize);
		if (errChkVA) {
			static const uint8_t kRet0[] = { 0x33,0xC0, 0xC3 };
			PatchXbeBytes(errChkVA, kRet0, sizeof(kRet0));
			MJ_LOG("Error checker stubbed at 0x%08X", (unsigned)errChkVA);
		} else {
			MJ_LOG("Error checker pattern not found");
		}
	}
#endif

#if MJ_CARD_READER
	// === 7. Card reader state machine stub ===
	// Pattern: MOV EAX,[card_state]; SUB ESP,8; DEC EAX; CMP EAX,5; JA
	{
		static const uint8_t kCardReaderPat[] = {
			0xA1, 0xFF,0xFF,0xFF,0xFF,
			0x83,0xEC,0x08,
			0x48,
			0x83,0xF8,0x05,
			0x0F,0x87
		};
		uintptr_t cardReaderVA = ScanXbe(kCardReaderPat, sizeof(kCardReaderPat), imageSize);
		if (cardReaderVA) {
			static const uint8_t kRet0[] = { 0x33,0xC0, 0xC3 };
			PatchXbeBytes(cardReaderVA, kRet0, sizeof(kRet0));
			MJ_LOG("Card reader stubbed at 0x%08X", (unsigned)cardReaderVA);
		} else {
			MJ_LOG("Card reader pattern not found");
		}
	}
#endif

#if MJ_BLOCK_RESOURCE
	// === 8. D3D_BlockOnResource NOP-stub ===
	{
		uintptr_t blockResVA = LookupSymbolAddress("D3D_BlockOnResource");
		if (!blockResVA) blockResVA = LookupSymbolAddress("D3D_BlockOnResource_0__LTCG_eax1");
		if (!blockResVA) blockResVA = LookupSymbolAddress("D3D_BlockOnResource_0__LTCG_ecx1");
		if (blockResVA) {
			const uint8_t* probe = (const uint8_t*)blockResVA;
			if (probe[0] != 0xC2 && probe[0] != 0xC3 && probe[0] != 0xCC) {
				static const uint8_t kRet4[] = { 0xC2, 0x04, 0x00 };
				PatchXbeBytes(blockResVA, kRet4, sizeof(kRet4));
				MJ_LOG("D3D_BlockOnResource stubbed at 0x%08X", (unsigned)blockResVA);
			}
		} else {
			MJ_LOG("D3D_BlockOnResource symbol not found");
		}
	}
#endif

#if MJ_BLOCK_VBLANK
	// === 9. D3DDevice_BlockUntilVerticalBlank NOP-stub ===
	{
		uintptr_t blockVBlankVA = LookupSymbolAddress("D3DDevice_BlockUntilVerticalBlank");
		if (blockVBlankVA) {
			const uint8_t* probe = (const uint8_t*)blockVBlankVA;
			if (probe[0] != 0xC2 && probe[0] != 0xC3) {
				static const uint8_t kRet = 0xC3;
				PatchXbeBytes(blockVBlankVA, &kRet, 1);
				MJ_LOG("BlockUntilVerticalBlank stubbed at 0x%08X", (unsigned)blockVBlankVA);
			}
		} else {
			MJ_LOG("BlockUntilVerticalBlank symbol not found");
		}
	}
#endif

#if MJ_FLIP_PENDING
	// === 10. CMiniport_IsFlipPending NOP-stub ===
	{
		uintptr_t flipPendVA = LookupSymbolAddress("CMiniport_IsFlipPending");
		if (flipPendVA) {
			const uint8_t* probe = (const uint8_t*)flipPendVA;
			if (probe[0] != 0x33 && probe[0] != 0xC2 && probe[0] != 0xC3) {
				static const uint8_t kRet0[] = { 0x33, 0xC0, 0xC3 };
				PatchXbeBytes(flipPendVA, kRet0, sizeof(kRet0));
				MJ_LOG("CMiniport_IsFlipPending stubbed at 0x%08X", (unsigned)flipPendVA);
			}
		} else {
			MJ_LOG("CMiniport_IsFlipPending symbol not found");
		}
	}
#endif

#if MJ_WARN_TIMER
	// === 11. WARNING timer shortcut (600 frames → 1) ===
	{
		static const uint8_t kWarnTimerPat[] = {
			0x8B, 0x15, 0xFF, 0xFF, 0xFF, 0xFF,
			0xC7, 0x02, 0x58, 0x02, 0x00, 0x00,
			0xC3
		};
		uintptr_t warnTimerVA = ScanXbe(kWarnTimerPat, sizeof(kWarnTimerPat), imageSize);
		if (warnTimerVA) {
			static const uint8_t kOne[] = { 0x01, 0x00, 0x00, 0x00 };
			PatchXbeBytes(warnTimerVA + 8, kOne, sizeof(kOne));
			MJ_LOG("WARNING timer shortened at 0x%08X", (unsigned)warnTimerVA);
		} else {
			MJ_LOG("WARNING timer pattern not found");
		}
	}
#endif

#if MJ_JVS_DISCOVERY
	// === 12. JVS discovery bypass ===
	// Pattern: PUSH EBX; PUSH ESI; PUSH EDI; CALL xx; MOV EDI,EAX; CALL xx; MOV ESI,EAX;
	//          MOV EAX,[dword_776310]; XOR EBX,EBX; CMP EAX,EBX
	// This is the JVS state machine (sub_1663C0) — make it return 0 immediately.
	{
		static const uint8_t kJvsDiscPat[] = {
			0x53, 0x56, 0x57,
			0xE8, 0xFF,0xFF,0xFF,0xFF,
			0x8B, 0xF8,
			0xE8, 0xFF,0xFF,0xFF,0xFF,
			0x8B, 0xF0,
			0xA1, 0xFF,0xFF,0xFF,0xFF,
			0x33, 0xDB,
			0x3B, 0xC3
		};
		uintptr_t jvsDiscVA = ScanXbe(kJvsDiscPat, sizeof(kJvsDiscPat), imageSize);
		if (jvsDiscVA) {
			static const uint8_t kRet0[] = { 0x33,0xC0, 0xC3 }; // XOR EAX,EAX; RET
			PatchXbeBytes(jvsDiscVA, kRet0, sizeof(kRet0));
			MJ_LOG("JVS discovery bypassed at 0x%08X", (unsigned)jvsDiscVA);
		} else {
			MJ_LOG("JVS discovery pattern not found");
		}
	}
#endif

	// ═══════════════════════════════════════════════════════════════
	// Game-specific memory patches (512MB version addresses)
	// ═══════════════════════════════════════════════════════════════

#if MJ_STARTUP_RESOURCE
	// ═══════════════════════════════════════════════════════════════
	// DHCP state machine bypass (sub_106D40 → set done immediately)
	// The STARTUP mode calls sub_106F00 which calls sub_106D40 to
	// run a DHCP state machine. In emulation, the DHCP init calls
	// sub_106930 which initializes AllNet networking, causing a hang.
	// By making sub_106D40 immediately set a1[3]=1 (done flag) and
	// return, sub_106F00 can proceed with resource loading (cases 2-5)
	// without getting stuck on DHCP.
	// Function is __usercall with a1 in ESI register.
	// Pattern: MOV EAX,[ESI+8]; TEST EAX,EAX; JLE +4; DEC EAX;
	//          MOV [ESI+8],EAX; MOV EAX,[ESI+8]; TEST EAX,EAX
	// (MJ2 follows with JG near; MJ3/MJ3Evo push regs first)
	// ═══════════════════════════════════════════════════════════════
	{
		static const uint8_t kDhcpStatePat[] = {
			0x8B, 0x46, 0x08,                   // MOV EAX, [ESI+8]
			0x85, 0xC0,                          // TEST EAX, EAX
			0x7E, 0x04,                          // JLE +4
			0x48,                                // DEC EAX
			0x89, 0x46, 0x08,                    // MOV [ESI+8], EAX
			0x8B, 0x46, 0x08,                    // MOV EAX, [ESI+8]
			0x85, 0xC0,                          // TEST EAX, EAX
		};
		uintptr_t dhcpStateVA = ScanXbe(kDhcpStatePat, sizeof(kDhcpStatePat), imageSize);
		if (dhcpStateVA) {
			uintptr_t dhcpPatchVA = dhcpStateVA;

			// MJ2's match is the first instruction in the function. MJ3 and
			// Evolution place a stack-cookie prologue immediately before the
			// shared state-machine body. Returning from the body match would
			// leave its 0x130-byte stack frame allocated, so RET would fetch a
			// zero local as the return address and execute a NULL function
			// pointer. Patch the verified function entry for those titles.
			if (game == MJ_3 || game == MJ_3EVO) {
				const uintptr_t candidateVA = dhcpStateVA - 0x12;
				const uint8_t* prologue = reinterpret_cast<const uint8_t*>(candidateVA);
				const bool hasExpectedPrologue =
					prologue[0] == 0x81 && prologue[1] == 0xEC &&
					prologue[2] == 0x30 && prologue[3] == 0x01 &&
					prologue[4] == 0x00 && prologue[5] == 0x00 &&
					prologue[6] == 0xA1 &&
					prologue[11] == 0x89 && prologue[12] == 0x84 &&
					prologue[13] == 0x24 && prologue[14] == 0x2C &&
					prologue[15] == 0x01 && prologue[16] == 0x00 &&
					prologue[17] == 0x00;
				if (!hasExpectedPrologue) {
					MJ_LOG("DHCP state machine prologue validation failed at 0x%08X",
						(unsigned)candidateVA);
					dhcpStateVA = 0;
				} else {
					dhcpPatchVA = candidateVA;
				}
			}

			// Patch to: MOV DWORD [ESI+0Ch],1; XOR EAX,EAX; RET; NOP...
			static const uint8_t kSetDone[] = {
				0xC7, 0x46, 0x0C, 0x01, 0x00, 0x00, 0x00,  // MOV DWORD [ESI+0Ch], 1
				0x33, 0xC0,                                  // XOR EAX, EAX
				0xC3,                                        // RET
				0x90, 0x90, 0x90, 0x90, 0x90, 0x90,          // NOP padding (16 bytes total)
			};
			if (dhcpStateVA) {
				PatchXbeBytes(dhcpPatchVA, kSetDone, sizeof(kSetDone));
				MJ_LOG("DHCP state machine bypassed at 0x%08X", (unsigned)dhcpPatchVA);
			}
		} else {
			MJ_LOG("DHCP state machine pattern not found!");
		}
	}
#endif

#if MJ_STARTUP_OFFLINE
	// MJ3 and Evolution have two title-local waits after the common network
	// library has initialized:
	//   state 6 waits for the retired arcade matching server's "common" object;
	//   state 8 waits for that server's startup response.
	// Without the original service neither condition can become true. Skipping
	// only these two startup branches preserves the initialized network objects
	// and all later gameplay/network routines, then lets the existing state
	// machine enter its normal offline-complete state (-1).
	if (game == MJ_3 || game == MJ_3EVO) {
		const uintptr_t commonObjectWaitVA =
			(game == MJ_3) ? 0x001B4C5E : 0x0026669F;
		const uintptr_t startupResponseWaitVA =
			(game == MJ_3) ? 0x001B4CAC : 0x002666ED;
		static const uint8_t kCommonObjectWait[] = {
			0x0F, 0x84, 0xBC, 0x00, 0x00, 0x00
		};
		static const uint8_t kStartupResponseWait[] = {
			0x74, 0x72
		};
		static const uint8_t kNop6[] = {
			0x90, 0x90, 0x90, 0x90, 0x90, 0x90
		};
		static const uint8_t kNop2[] = {
			0x90, 0x90
		};

		const bool commonWaitPatched = PatchExpectedBytes(
			commonObjectWaitVA,
			kCommonObjectWait,
			kNop6,
			sizeof(kNop6),
			"MJ startup matching-server wait");
		const bool responseWaitPatched = PatchExpectedBytes(
			startupResponseWaitVA,
			kStartupResponseWait,
			kNop2,
			sizeof(kNop2),
			"MJ startup server-response wait");
		if (!commonWaitPatched || !responseWaitPatched) {
			MJ_LOG("MJ startup offline bypass was not fully applied");
		}
	}
#endif

#if MJ_NET_PATCHES
	if (game == MJ_2) {
		PatchDword(0x007DB170, 0xFFFFFFFF);  // Set NETWORK to OK
		PatchDword(0x002C575C, 0xFFFFFFFF);  // Disable in-game communication error
		MJ_LOG("MJ2 network patches applied");
	} else if (game == MJ_3) {
		// Do not pre-seed the startup globals. 0xC290D8 is the startup
		// controller's network state and setting it to -1 before
		// sub_266E70 runs makes the title skip initialization with every
		// neighboring state variable still zero.
		MJ_LOG("MJ3 network startup left to the state-machine bypasses");
	} else if (game == MJ_3EVO) {
		// 0xCB12E0 is the same startup-controller state in Evolution.
		// 0x5050C4 is only a byte-sized "live subsystem active" flag, not
		// an error latch; writing a DWORD of 0xFFFFFFFF also corrupts the
		// adjacent bytes. Let the targeted DHCP/link bypasses advance the
		// controller without mutating either global at image-load time.
		MJ_LOG("MJ3 Evo network startup left to the state-machine bypasses");
	}
#endif

	MJ_LOG("All patches applied for %s", gameName);
}
