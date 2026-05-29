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

extern std::map<std::string, xbox::addr_xt> g_SymbolAddresses;

int g_ChihiroMjGame = 0;  // 0=none, 1=MJ2, 2=MJ3, 3=MJ3Evo

#define MJ_LOG(fmt, ...) do { \
	printf("MahjongPatch: " fmt "\n", ##__VA_ARGS__); \
	{ FILE* _mf = fopen("C:\\temp\\mj_patches.log","a"); \
	  if(_mf){ fprintf(_mf, "MahjongPatch: " fmt "\n", ##__VA_ARGS__); fclose(_mf); } } \
} while(0)

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
#define MJ_JVS_DISCOVERY    1  // bypass JVS discovery loop
#define MJ_NET_PATCHES      1
#define MJ_BASEBOARD_INIT   1  // skip fatal error on baseboard init failure
#define MJ_BOARD_READY      1  // skip bit-0x20 check in periodic update
#define MJ_DIMM_INIT_SKIP   0  // DISABLED: causes crash - game depends on sub_11CD0 state
#define MJ_DIMM_POLL_SKIP   0  // skip sub_1647A0 (DIMM poll) so sub_11CD0 can create thread
#define MJ_STARTUP_RESOURCE 1  // skip DHCP state machine (sub_106D40 → set done immediately)

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

static uintptr_t LookupSymbolAddress(const char* name)
{
	auto it = g_SymbolAddresses.find(name);
	if (it != g_SymbolAddresses.end() && it->second != 0) {
		return it->second;
	}
	return 0;
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
	{
		static const uint8_t kGetStatPat[] = {
			0x8B, 0x44, 0x24, 0x04,
			0x85, 0xC0,
			0x75, 0x13,
			0x68
		};
		uintptr_t getStatVA = ScanXbe(kGetStatPat, sizeof(kGetStatPat), imageSize);
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
			// Patch to: MOV DWORD [ESI+0Ch],1; XOR EAX,EAX; RET; NOP...
			static const uint8_t kSetDone[] = {
				0xC7, 0x46, 0x0C, 0x01, 0x00, 0x00, 0x00,  // MOV DWORD [ESI+0Ch], 1
				0x33, 0xC0,                                  // XOR EAX, EAX
				0xC3,                                        // RET
				0x90, 0x90, 0x90, 0x90, 0x90, 0x90,          // NOP padding (16 bytes total)
			};
			PatchXbeBytes(dhcpStateVA, kSetDone, sizeof(kSetDone));
			MJ_LOG("DHCP state machine bypassed at 0x%08X", (unsigned)dhcpStateVA);
		} else {
			MJ_LOG("DHCP state machine pattern not found!");
		}
	}
#endif

#if MJ_NET_PATCHES
	if (game == MJ_2) {
		PatchDword(0x007DB170, 0xFFFFFFFF);  // Set NETWORK to OK
		PatchDword(0x002C575C, 0xFFFFFFFF);  // Disable in-game communication error
		MJ_LOG("MJ2 network patches applied");
	} else if (game == MJ_3) {
		PatchDword(0x00C290D8, 0xFFFFFFFF);  // Set NETWORK to OK
		PatchDword(0x00476830, 0xFFFFFFFF);  // Disable in-game communication error
		MJ_LOG("MJ3 network patches applied");
	} else if (game == MJ_3EVO) {
		PatchDword(0x00CB12E0, 0xFFFFFFFF);  // Set NETWORK to OK
		PatchDword(0x005050C4, 0xFFFFFFFF);  // Disable in-game communication error
		MJ_LOG("MJ3 Evo network patches applied");
	}
#endif

	MJ_LOG("All patches applied for %s", gameName);
}
