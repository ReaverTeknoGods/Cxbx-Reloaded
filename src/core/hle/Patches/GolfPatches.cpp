// ******************************************************************
// *  Cxbx Virtua Golf / Sega Golf Club patches
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
#include <cstdio>

#define GOLF_LOG(fmt, ...) do { \
	FILE* _f = fopen("C:\\temp\\golf_patches.log", "a"); \
	if (_f) { fprintf(_f, fmt "\n", ##__VA_ARGS__); fclose(_f); } \
	printf(fmt "\n", ##__VA_ARGS__); \
} while(0)

// ── Hook functions ────────────────────────────────────────────────

// Fake device struct for state 0 handler — bytes at +0x14 and +0x15 must be non-zero
static uint8_t s_fakeDeviceStruct[0x20] = {};

// sub_F3910: hardware error check, called with arg=6 and arg=3 in state 0.
// Returns 1 if the indexed table entry is NULL (system not ready).
// Patch to always return 0 (system is ready).
static int __cdecl GolfErrorCheckHook(int arg) {
	return 0;
}

// Background thread to monitor init state progression and force past stuck states
static DWORD WINAPI GolfInitForceThread(LPVOID param) {
	uint32_t stateAddr = (uint32_t)(uintptr_t)param;
	volatile int* pState = (volatile int*)(uintptr_t)stateAddr;
	volatile int* pNetDone = (volatile int*)0x00DA6B64;
	volatile int* pSub1 = (volatile int*)0x00DA6B28;
	volatile int* pSub2 = (volatile int*)0x00DA6B2C;
	volatile int* pSub3 = (volatile int*)0x00DA6B30;
	volatile int* pSub4 = (volatile int*)0x00DA6B34;
	volatile int* pGmState = (volatile int*)0x00DA6B54;  // game-mode processing state
	volatile int* pGmDisp  = (volatile int*)0x00DA6B58;  // game-mode display state
	int lastState = -999;
	for (int i = 0; i < 600; i++) { // run for 60 seconds
		int cur = *pState;
		if (cur != lastState) {
			GOLF_LOG("GolfPatch: [Thread] tick=%d state changed %d->%d", i, lastState, cur);
			lastState = cur;
		}
		// Keep network-done flag forced (game may reset it)
		if (*pNetDone == 0) {
			*pNetDone = 1;
		}
		// Keep subsystem vars at -3 (done) — game code may reset these
		if (*pSub1 != -3) { *pSub1 = -3; }
		if (*pSub2 != -3) { *pSub2 = -3; }
		if (*pSub3 != -3) { *pSub3 = -3; }
		if (*pSub4 != -3) { *pSub4 = -3; }
		// Force state 3→4 (SYSTEM INITIALIZE waits on hardware)
		if (cur == 3) {
			*pState = 4;
			GOLF_LOG("GolfPatch: [Thread] tick=%d forced state 3->4", i);
		}
		// Keep game-mode state machine bypassed (init func may reset it)
		if (*pGmDisp != 0) {
			*pGmState = 8;
			*pGmDisp = 0;
		}
		Sleep(100);
	}
	GOLF_LOG("GolfPatch: [Thread] Done, final state=%d", *pState);
	return 0;
}

static int __stdcall GolfMbRecvHook(uint32_t a1, uint32_t a2, uint32_t a3) {
	return 0; // no data
}

static int __stdcall GolfMbSendHook(uint32_t a1, uint32_t a2, uint32_t a3) {
	return 0;
}

static int __stdcall GolfLinkOkHook(uint32_t a1, uint32_t a2, uint32_t a3) {
	return 1; // link is OK
}

// ── CRI ADXF_GetStat hook ────────────────────────────────────────
static int __cdecl GolfGetStatHook(int handle) {
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

// ── XBE hash constants ───────────────────────────────────────────
static const uint64_t kGolfHashes[] = {
	0xcc59629898491b64ULL, // golf.xbe (raw file hash)
	0xE5940C325FB8B2FBULL, // golf.xbe (runtime hash after emulator header patching)
};

bool IsGolfXbe(uint64_t xbeHash)
{
	for (auto h : kGolfHashes) {
		if (xbeHash == h) return true;
	}
	return false;
}

// ── Patch toggle flags ───────────────────────────────────────────
#define GOLF_LINKOK         1   // 1: LinkOK JMP hook
#define GOLF_MBRECVSEND     1   // 2: MbRecv/MbSend JMP hooks
#define GOLF_DIMM_READY     1   // 3: DIMM-board ready bypass
#define GOLF_KICKOFF_IDLE   1   // 4: D3D_KickOffAndWaitForIdle stub
#define GOLF_CRI_GETSTAT    1   // 5: CRI ADXF_GetStat hook
#define GOLF_CARD_READER    1   // 6: Card reader state machine stub
#define GOLF_DIMM_THREAD    0   // 7: DIMM update thread stubs (OFF - let threads run)
#define GOLF_DIMM_COMM      1   // 8: DIMM communication function stubs
#define GOLF_SKIP_INIT      1   // 9: Force-skip SYSTEM INITIALIZE state check

// ── Main patch entry point ───────────────────────────────────────

void ApplyGolfPatches(uint32_t imageSize)
{
	GOLF_LOG("GolfPatch: applying patches (imageSize=0x%X)\n", imageSize);

	// Write marker file so we know patches are running
	{
		FILE* f = fopen("C:\\temp\\golf_patches.log", "w");
		if (f) { fprintf(f, "GolfPatch: start (imageSize=0x%X)\n", imageSize); fclose(f); }
	}
#if GOLF_LINKOK
	// === LinkOK — JMP hook to GolfLinkOkHook ===
	// Pattern: TEST EAX,EAX; JNZ +8; MOV EAX,0xFFFFFFFE; RET 0Ch
	// Same Sega media board library as Gundam.
	{
		static const uint8_t kLinkOkPat[] = {
			0x85,0xC0, 0x75,0x08, 0xB8,0xFE,0xFF,0xFF,0xFF, 0xC2,0x0C,0x00
		};
		auto linkOkHits = ScanXbeAll(kLinkOkPat, sizeof(kLinkOkPat), imageSize);
		for (auto linkOkVA : linkOkHits) {
			if (linkOkVA >= 5 && ((const uint8_t*)(linkOkVA - 5))[0] == 0xA1) {
				PatchWithJmp(linkOkVA - 5, (const void*)&GolfLinkOkHook);
				GOLF_LOG("GolfPatch: LinkOK patched at VA 0x%08X\n", (unsigned)(linkOkVA - 5));
			}
		}
		if (linkOkHits.empty()) {
			GOLF_LOG("GolfPatch: LinkOK pattern not found!\n");
		}
	}
#endif

#if GOLF_MBRECVSEND
	// === MbRecvPacket / MbSendPacket — JMP hooks ===
	{
		static const uint8_t kMbFuncPat[] = {
			0x83,0xEC,0x08, 0x8D,0x44,0x24,0x04, 0x50, 0x6A,0x00,
			0xE8,0xFF,0xFF,0xFF,0xFF,
			0x8D,0x0C,0x24, 0x51, 0x6A,0x01, 0xE8
		};
		auto mbHits = ScanXbeAll(kMbFuncPat, sizeof(kMbFuncPat), imageSize);
		if (mbHits.size() >= 2) {
			PatchWithJmp(mbHits[0], (const void*)&GolfMbRecvHook);
			PatchWithJmp(mbHits[1], (const void*)&GolfMbSendHook);
			GOLF_LOG("GolfPatch: MbRecv/MbSend hooked at 0x%08X / 0x%08X\n", (unsigned)mbHits[0], (unsigned)mbHits[1]);
		} else {
			GOLF_LOG("GolfPatch: MbRecvPacket/MbSendPacket not found! (%zu hits)\n", mbHits.size());
		}
	}
#endif

#if GOLF_DIMM_READY
	// === DIMM-board ready bypass (always return 1) ===
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
			GOLF_LOG("GolfPatch: DIMM-ready patched at 0x%08X\n", (unsigned)dimmReadyVA);
		} else {
			GOLF_LOG("GolfPatch: DIMM-ready pattern not found\n");
		}
	}
#endif

#if GOLF_KICKOFF_IDLE
	// === D3D_KickOffAndWaitForIdle NOP-stub ===
	// Polls NV2A GPU status registers — infinite busy-wait in HLE mode.
	{
		static const uint8_t kKickIdlePat[] = {
			0xA1, 0xFF,0xFF,0xFF,0xFF, 0x8B, 0x48, 0x2C
		};
		uintptr_t kickIdleVA = ScanXbe(kKickIdlePat, sizeof(kKickIdlePat), imageSize);
		if (kickIdleVA) {
			static const uint8_t kRet = 0xC3;
			PatchXbeBytes(kickIdleVA, &kRet, 1);
			GOLF_LOG("GolfPatch: D3D_KickOffAndWaitForIdle stubbed at 0x%08X\n", (unsigned)kickIdleVA);
		} else {
			GOLF_LOG("GolfPatch: D3D_KickOffAndWaitForIdle pattern NOT FOUND\n");
		}
	}
#endif

#if GOLF_CRI_GETSTAT
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
			PatchWithJmp(getStatVA, (const void*)&GolfGetStatHook);
			GOLF_LOG("GolfPatch: CRI GetStat hooked at 0x%08X\n", (unsigned)getStatVA);
		} else {
			GOLF_LOG("GolfPatch: CRI GetStat pattern not found\n");
		}
	}
#endif

#if GOLF_CARD_READER
	// === Card reader state machine stub ===
	// 6-state switch on dword_EE6998, with 0x3C (60) retry loops.
	// Pattern: MOV EAX,[EE6998h]; SUB ESP,8; DEC EAX; CMP EAX,5; JA ...
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
			GOLF_LOG("GolfPatch: Card reader stubbed at 0x%08X\n", (unsigned)cardReaderVA);
		} else {
			GOLF_LOG("GolfPatch: Card reader pattern not found\n");
		}
	}
#endif

#if GOLF_DIMM_THREAD
	// === DIMM update thread stubs ===
	// Two thread entry points run DIMM I/O loops with many busy-waits.
	// 0x11320 and 0x18AE70 are thread entries (no CALL references).
	// Pattern for 0x11320: PUSH EBP; MOV EBP,ESP; AND ESP,-8; SUB ESP,28h; PUSH ESI; PUSH EDI; PUSH 002676B0h
	{
		static const uint8_t kDimmThread1Pat[] = {
			0x55, 0x8B,0xEC, 0x83,0xE4,0xF8, 0x83,0xEC,0x28,
			0x56, 0x57, 0x68, 0xB0,0x76,0x26,0x00, 0xE8
		};
		uintptr_t dimmThread1VA = ScanXbe(kDimmThread1Pat, sizeof(kDimmThread1Pat), imageSize);
		if (dimmThread1VA) {
			// Replace with: XOR EAX,EAX; RET 4 (thread entry takes one param)
			static const uint8_t kRetThread[] = { 0x33,0xC0, 0xC2,0x04,0x00 };
			PatchXbeBytes(dimmThread1VA, kRetThread, sizeof(kRetThread));
			GOLF_LOG("GolfPatch: DIMM thread 1 stubbed at 0x%08X\n", (unsigned)dimmThread1VA);
		} else {
			GOLF_LOG("GolfPatch: DIMM thread 1 pattern not found\n");
		}
	}
	// Pattern for 0x18AE70: PUSH EBP; MOV EBP,ESP; AND ESP,-8; SUB ESP,1Ch; PUSH ESI; PUSH 002676B0h
	{
		static const uint8_t kDimmThread2Pat[] = {
			0x55, 0x8B,0xEC, 0x83,0xE4,0xF8, 0x83,0xEC,0x1C,
			0x56, 0x68, 0xB0,0x76,0x26,0x00, 0xE8
		};
		uintptr_t dimmThread2VA = ScanXbe(kDimmThread2Pat, sizeof(kDimmThread2Pat), imageSize);
		if (dimmThread2VA) {
			static const uint8_t kRetThread[] = { 0x33,0xC0, 0xC2,0x04,0x00 };
			PatchXbeBytes(dimmThread2VA, kRetThread, sizeof(kRetThread));
			GOLF_LOG("GolfPatch: DIMM thread 2 stubbed at 0x%08X\n", (unsigned)dimmThread2VA);
		} else {
			GOLF_LOG("GolfPatch: DIMM thread 2 pattern not found\n");
		}
	}
#endif

#if GOLF_DIMM_COMM
	// === DIMM communication function stubs ===
	// 0x17FD90: DIMM init/comm function with busy-wait loops
	// Pattern: PUSH ESI; PUSH 1; XOR ESI,ESI; CALL ...
	{
		static const uint8_t kDimmCommPat[] = {
			0x56, 0x6A,0x01, 0x33,0xF6, 0xE8
		};
		auto dimmCommHits = ScanXbeAll(kDimmCommPat, sizeof(kDimmCommPat), imageSize);
		for (auto va : dimmCommHits) {
			if (va >= 0x170000 && va <= 0x190000) {
				static const uint8_t kRet0[] = { 0x33,0xC0, 0xC3 };
				PatchXbeBytes(va, kRet0, sizeof(kRet0));
				GOLF_LOG("GolfPatch: DIMM comm stubbed at 0x%08X\n", (unsigned)va);
			}
		}
	}
	// 0x1849C0: Baseboard check function
	// Pattern: PUSH EBX; PUSH ESI; PUSH EDI; CALL ... (target in 0x1Cxxxx range)
	{
		static const uint8_t kBaseboardPat[] = {
			0x53, 0x56, 0x57, 0xE8, 0xFF,0xFF,0xFF,0xFF,
			0x8B,0xF8, 0xA1, 0xFF,0xFF,0xFF,0xFF,
			0x83,0xCB,0xFF, 0x85,0xC0
		};
		uintptr_t baseboardVA = ScanXbe(kBaseboardPat, sizeof(kBaseboardPat), imageSize);
		if (baseboardVA) {
			static const uint8_t kRet0[] = { 0x33,0xC0, 0xC3 };
			PatchXbeBytes(baseboardVA, kRet0, sizeof(kRet0));
			GOLF_LOG("GolfPatch: Baseboard check stubbed at 0x%08X\n", (unsigned)baseboardVA);
		} else {
			GOLF_LOG("GolfPatch: Baseboard check pattern not found\n");
		}
	}
#endif

#if GOLF_SKIP_INIT
	// === Help state machine progress past hardware-blocking checks ===
	// The update function dispatches state handlers 0-6. State 0 blocks on
	// hardware checks that never pass in emulation. Instead of disabling the
	// state machine entirely (which skips all init), we fix the blocking calls
	// so states 0→1→2→3→... progress naturally.
	{
		// --- Part A: Find state variable address ---
		static const uint8_t kInitCheckPat[] = {
			0x39, 0x1D, 0xFF, 0xFF, 0xFF, 0xFF,  // CMP [subsys1], EBX
			0xBF, 0x04, 0x00, 0x00, 0x00,         // MOV EDI, 4
			0x7D, 0x1E,                            // JGE +0x1E
			0x39, 0x1D, 0xFF, 0xFF, 0xFF, 0xFF,   // CMP [subsys2], EBX
			0x7D, 0x16,                            // JGE +0x16
			0x39, 0x1D, 0xFF, 0xFF, 0xFF, 0xFF,   // CMP [subsys3], EBX
			0x7D, 0x0E,                            // JGE +0x0E
			0x39, 0x1D, 0xFF, 0xFF, 0xFF, 0xFF,   // CMP [subsys4], EBX
			0x7D, 0x06,                            // JGE +0x06
			0x89, 0x3D, 0xFF, 0xFF, 0xFF, 0xFF    // MOV [stateVar], EDI
		};
		uintptr_t initCheckVA = ScanXbe(kInitCheckPat, sizeof(kInitCheckPat), imageSize);
		if (initCheckVA) {
			uint32_t stateAddr = *(uint32_t*)(initCheckVA + 39);
			GOLF_LOG("GolfPatch: Init state var at 0x%08X", stateAddr);

			// --- Part B: Patch sub_F3910 (hardware error check) ---
			// Pattern: MOV EAX,[ESP+4]; LEA EAX,[EAX+EAX*8]; MOV ECX,[EAX*4+table]; TEST ECX,ECX; SETE AL; RET
			// This is a tiny function: 8B 44 24 04 8D 04 C0 8B 0C 85 ?? ?? ?? ?? 85 C9 0F 94 C0 C3
			{
				static const uint8_t kErrCheckPat[] = {
					0x8B, 0x44, 0x24, 0x04,   // MOV EAX, [ESP+4]
					0x8D, 0x04, 0xC0,         // LEA EAX, [EAX+EAX*8]
					0x8B, 0x0C, 0x85,         // MOV ECX, [EAX*4+...
					0xFF, 0xFF, 0xFF, 0xFF,   //   table_addr]
					0x85, 0xC9,               // TEST ECX, ECX
					0x0F, 0x94, 0xC0,         // SETE AL
					0xC3                       // RET
				};
				uintptr_t errCheckVA = ScanXbe(kErrCheckPat, sizeof(kErrCheckPat), imageSize);
				if (errCheckVA) {
					// Replace with: XOR EAX,EAX; RET (always return 0 = "no error")
					static const uint8_t kRet0[] = { 0x33, 0xC0, 0xC3 };
					PatchXbeBytes(errCheckVA, kRet0, sizeof(kRet0));
					GOLF_LOG("GolfPatch: Error check (sub_F3910) stubbed at 0x%08X", (unsigned)errCheckVA);
				} else {
					GOLF_LOG("GolfPatch: Error check pattern NOT FOUND!");
				}
			}

			// --- Part B2: Patch sub_F3930 (firmware version check) ---
			// Pattern: MOV EAX,[ESP+4]; LEA EAX,[EAX+EAX*2]; CMP [EAX*4+table2],2; SETE AL; RET
			// Returns 1 if firmware version == 2 for the given subsystem.
			// Stub to always return 1 so Error 14 ("Network firmware version...") is never raised.
			{
				static const uint8_t kVerCheckPat[] = {
					0x8B, 0x44, 0x24, 0x04,   // MOV EAX, [ESP+4]
					0x8D, 0x04, 0xC0,         // LEA EAX, [EAX+EAX*2]
					0x83, 0x3C, 0x85,         // CMP [EAX*4+...
					0xFF, 0xFF, 0xFF, 0xFF,   //   table2_addr], ...
					0x02,                     // 2
					0x0F, 0x94, 0xC0,         // SETE AL
					0xC3                       // RET
				};
				uintptr_t verCheckVA = ScanXbe(kVerCheckPat, sizeof(kVerCheckPat), imageSize);
				if (verCheckVA) {
					// Replace with: MOV AL,1; RET (always return 1 = "version OK")
					static const uint8_t kRet1[] = { 0xB0, 0x01, 0xC3 };
					PatchXbeBytes(verCheckVA, kRet1, sizeof(kRet1));
					GOLF_LOG("GolfPatch: Version check (sub_F3930) stubbed at 0x%08X", (unsigned)verCheckVA);
				} else {
					GOLF_LOG("GolfPatch: Version check pattern NOT FOUND!");
				}
			}

			// --- Part B3: Stub sub_100EB0 (subsystem 4 monitor / Error 14 display) ---
			// This function checks [DA6B34] each frame and can display Error 14/15.
			// Pattern: MOV EAX,[DA6B34]; LEA ECX,[EAX+7]; CMP ECX,9; JA ...
			// Stub to just return so it never displays Error 14.
			{
				static const uint8_t kSubsys4Pat[] = {
					0xA1, 0x34, 0x6B, 0xDA, 0x00,  // MOV EAX, [DA6B34]
					0x8D, 0x48, 0x07,               // LEA ECX, [EAX+7]
					0x83, 0xF9, 0x09,               // CMP ECX, 9
					0x0F, 0x87                       // JA ...
				};
				uintptr_t subsys4VA = ScanXbe(kSubsys4Pat, sizeof(kSubsys4Pat), imageSize);
				if (subsys4VA) {
					// Replace with RET (just return, don't process subsystem 4 at all)
					static const uint8_t kRet = 0xC3;
					PatchXbeBytes(subsys4VA, &kRet, 1);
					GOLF_LOG("GolfPatch: Subsystem 4 monitor stubbed at 0x%08X", (unsigned)subsys4VA);
				} else {
					GOLF_LOG("GolfPatch: Subsystem 4 monitor pattern NOT FOUND!");
				}
			}

			// --- Part B4: Patch sub_0A0840 (IsTestMode check) ---
			// Returns 1 if test mode, 0 if game mode.
			// The init function uses this to select test mode vs game mode startup.
			// Pattern: MOV ECX,[31D324h]; MOV DL,[ECX+4]; XOR AL,AL; TEST DL,DL
			// Patch to always return 0 to boot into GAME MODE (not test menu).
			{
				static const uint8_t kErrGuardPat[] = {
					0x8B, 0x0D, 0x24, 0xD3, 0x31, 0x00,  // MOV ECX, [31D324h]
					0x8A, 0x51, 0x04,                      // MOV DL, [ECX+4]
					0x32, 0xC0,                            // XOR AL, AL
					0x84, 0xD2,                            // TEST DL, DL
					0x74, 0x02,                            // JZ +2
					0xB0, 0x01,                            // MOV AL, 1
					0xC3                                   // RET
				};
				uintptr_t errGuardVA = ScanXbe(kErrGuardPat, sizeof(kErrGuardPat), imageSize);
				if (errGuardVA) {
					// Replace with: XOR AL,AL; RET (always return 0 = "game mode")
					static const uint8_t kRet0[] = { 0x32, 0xC0, 0xC3 };
					PatchXbeBytes(errGuardVA, kRet0, sizeof(kRet0));
					GOLF_LOG("GolfPatch: IsTestMode (sub_0A0840) → always 0 (game mode) at 0x%08X", (unsigned)errGuardVA);
				} else {
					GOLF_LOG("GolfPatch: IsTestMode pattern NOT FOUND!");
				}
			}

			// --- Part C: Patch sub_A1A10 (device wait) to return fake struct ---
			// State 0 handler calls sub_A1A10(0), expects non-NULL result with
			// bytes at offset +0x14 and +0x15 being non-zero.
			// Find the pattern: PUSH EBX(=0); CALL sub; ADD ESP,4; CMP EAX,EBX; JE ...
			// which is: 53 E8 ?? ?? ?? ?? 83 C4 04 3B C3 0F 84
			{
				static const uint8_t kDevWaitPat[] = {
					0x53,                               // PUSH EBX (=0)
					0xE8, 0xFF, 0xFF, 0xFF, 0xFF,       // CALL sub_A1A10
					0x83, 0xC4, 0x04,                   // ADD ESP, 4
					0x3B, 0xC3,                         // CMP EAX, EBX
					0x0F, 0x84                          // JE ... (bail if NULL)
				};
				uintptr_t devWaitVA = ScanXbe(kDevWaitPat, sizeof(kDevWaitPat), imageSize);
				if (devWaitVA) {
					// Initialize fake device struct
					memset(s_fakeDeviceStruct, 0, sizeof(s_fakeDeviceStruct));
					s_fakeDeviceStruct[0x14] = 1;  // must be non-zero
					s_fakeDeviceStruct[0x15] = 1;  // must be non-zero

					// NOP out the PUSH+CALL+ADDSP (9 bytes) and replace with:
					// MOV EAX, &s_fakeDeviceStruct (5 bytes) + 4 NOPs
					uint8_t movEax[9] = { 0xB8, 0,0,0,0, 0x90,0x90,0x90,0x90 };
					uint32_t fakeAddr = (uint32_t)(uintptr_t)s_fakeDeviceStruct;
					memcpy(movEax + 1, &fakeAddr, 4);
					PatchXbeBytes(devWaitVA, movEax, sizeof(movEax));
					GOLF_LOG("GolfPatch: Device wait bypassed at 0x%08X (fake=0x%08X)",
						(unsigned)devWaitVA, fakeAddr);
				} else {
					GOLF_LOG("GolfPatch: Device wait pattern NOT FOUND!");
				}
			}

			// --- Part D: Launch monitoring thread ---
			CreateThread(NULL, 0, GolfInitForceThread, (LPVOID)(uintptr_t)stateAddr, 0, NULL);
			GOLF_LOG("GolfPatch: Init force thread launched");

		} else {
			GOLF_LOG("GolfPatch: Init state check pattern NOT FOUND");
		}

		// --- Part E: Stub subsystem UPDATE functions ---
		// These 3 functions check hardware and update subsystem vars (DA6B28-DA6B30).
		// They're called from state 3 handler. If not stubbed, they reset subsystem
		// vars to 0 and run hardware checks that fail in emulation.
		// (sub_100EB0 for DA6B34 was already stubbed in Part B3 above.)
		// We make each set its subsystem var to -3 (done) and return.
		{
			// sub_100A00: updates DA6B28 (network check)
			// Pattern: MOV EAX,[DA6B28]; PUSH ESI; ADD EAX,3; XOR ESI,ESI; CMP EAX,3; JA
			static const uint8_t kUpd1Pat[] = {
				0xA1, 0x28, 0x6B, 0xDA, 0x00,  // MOV EAX, [DA6B28]
				0x56,                           // PUSH ESI
				0x83, 0xC0, 0x03,               // ADD EAX, 3
				0x33, 0xF6,                     // XOR ESI, ESI
				0x83, 0xF8, 0x03,               // CMP EAX, 3
				0x77                            // JA
			};
			uintptr_t upd1VA = ScanXbe(kUpd1Pat, sizeof(kUpd1Pat), imageSize);
			if (upd1VA) {
				// MOV dword [DA6B28], -3; XOR EAX,EAX; RET
				static const uint8_t kPatch[] = {
					0xC7, 0x05, 0x28, 0x6B, 0xDA, 0x00, 0xFD, 0xFF, 0xFF, 0xFF,
					0x33, 0xC0, 0xC3
				};
				PatchXbeBytes(upd1VA, kPatch, sizeof(kPatch));
				GOLF_LOG("GolfPatch: Subsys update 1 (DA6B28) patched at 0x%08X", (unsigned)upd1VA);
			}

			// sub_100A80: updates DA6B2C (database check)
			// Pattern: SUB ESP,10h; LEA EAX,[ESP+4]; PUSH EAX; LEA ECX,[ESP+0Ch]; PUSH ECX
			static const uint8_t kUpd2Pat[] = {
				0x83, 0xEC, 0x10,               // SUB ESP, 10h
				0x8D, 0x44, 0x24, 0x04,         // LEA EAX, [ESP+4]
				0x50,                           // PUSH EAX
				0x8D, 0x4C, 0x24, 0x0C,         // LEA ECX, [ESP+0Ch]
				0x51                            // PUSH ECX
			};
			uintptr_t upd2VA = ScanXbe(kUpd2Pat, sizeof(kUpd2Pat), imageSize);
			if (upd2VA) {
				// MOV dword [DA6B2C], -3; XOR EAX,EAX; RET
				static const uint8_t kPatch[] = {
					0xC7, 0x05, 0x2C, 0x6B, 0xDA, 0x00, 0xFD, 0xFF, 0xFF, 0xFF,
					0x33, 0xC0, 0xC3
				};
				PatchXbeBytes(upd2VA, kPatch, sizeof(kPatch));
				GOLF_LOG("GolfPatch: Subsys update 2 (DA6B2C) patched at 0x%08X", (unsigned)upd2VA);
			}

			// sub_100BA0: updates DA6B30 (touch panel / card system, big state machine)
			// Pattern: CALL IsTestMode; TEST AL; JNZ far; MOV EAX,[DA6B30]; ADD EAX,3; CMP EAX,0Bh; JA far
			static const uint8_t kUpd3Pat[] = {
				0xE8, 0xFF, 0xFF, 0xFF, 0xFF,       // CALL IsTestMode
				0x84, 0xC0,                          // TEST AL, AL
				0x0F, 0x85, 0xC9, 0x01, 0x00, 0x00, // JNZ +0x1C9
				0xA1, 0x30, 0x6B, 0xDA, 0x00         // MOV EAX, [DA6B30]
			};
			uintptr_t upd3VA = ScanXbe(kUpd3Pat, sizeof(kUpd3Pat), imageSize);
			if (upd3VA) {
				// MOV dword [DA6B30], -3; XOR EAX,EAX; RET
				static const uint8_t kPatch[] = {
					0xC7, 0x05, 0x30, 0x6B, 0xDA, 0x00, 0xFD, 0xFF, 0xFF, 0xFF,
					0x33, 0xC0, 0xC3
				};
				PatchXbeBytes(upd3VA, kPatch, sizeof(kPatch));
				GOLF_LOG("GolfPatch: Subsys update 3 (DA6B30) patched at 0x%08X", (unsigned)upd3VA);
			}

			// sub_100B60: updates DA6B30 supplementary (called later in state 3)
			// Pattern: CALL IsTestMode; TEST AL; JNZ +18h; MOV EAX,[DA6B30]; TEST EAX,EAX; JL +0Fh
			static const uint8_t kUpd4Pat[] = {
				0xE8, 0xFF, 0xFF, 0xFF, 0xFF,   // CALL IsTestMode
				0x84, 0xC0,                      // TEST AL, AL
				0x75, 0x18,                      // JNZ +18h
				0xA1, 0x30, 0x6B, 0xDA, 0x00    // MOV EAX, [DA6B30]
			};
			uintptr_t upd4VA = ScanXbe(kUpd4Pat, sizeof(kUpd4Pat), imageSize);
			if (upd4VA) {
				static const uint8_t kRet = 0xC3;
				PatchXbeBytes(upd4VA, &kRet, 1);
				GOLF_LOG("GolfPatch: Subsys update 4 (DA6B30 supp) stubbed at 0x%08X", (unsigned)upd4VA);
			}
		}
	}
#endif

	GOLF_LOG("GolfPatch: all patches applied");

	// Force DIMM state to ready (state 4) so main thread doesn't wait for threads
	*(volatile int*)0x00EE6998 = 4;  // DIMM state = ready
	*(volatile int*)0x00EE6988 = 1;  // Init-done flag

	// Subsystem status variables - set to -3 (0xFD = "completed successfully")
	// The state machine uses: -1=disabled, -2=error, -3=done, >=0 = in-progress
	// State 6 specifically checks DA6B28 == -3 to transition to state 7.
	*(volatile int*)0x00DA6B28 = -3;  // subsystem 1 complete
	*(volatile int*)0x00DA6B2C = -3;  // subsystem 2 complete
	*(volatile int*)0x00DA6B30 = -3;  // subsystem 3 complete
	*(volatile int*)0x00DA6B34 = -3;  // subsystem 4 complete

	// Network init completion flag - state 1 waits on this
	*(volatile int*)0x00DA6B64 = 1;

	// Game-mode state machine at DA6B54 — bypass hardware startup checks
	// The game has a second state machine (states 0-11) that runs during original state 1.
	// It checks touch panel, card system, database, network — all fail in emulation.
	// Force internal processing state past all checks, and display state to 0 (no overlay).
	*(volatile int*)0x00DA6B54 = 8;   // processing state → 8 (terminal/done, resets to 0)
	*(volatile int*)0x00DA6B58 = 0;   // display state → 0 (no "SYSTEM STARTUP" overlay)
	*(volatile int*)0x00DA6B5C = 0;   // timer → 0
	*(volatile int*)0x00DA6B60 = 0;   // frame counter
	*(volatile int*)0x00DA6B70 = 1;   // startup complete flag (DA6B54 + 0x1C)

	GOLF_LOG("GolfPatch: DIMM state forced to 4, subsystems + network + game-mode done");
}
