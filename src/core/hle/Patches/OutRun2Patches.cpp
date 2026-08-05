// ******************************************************************
// *  Cxbx OutRun 2 / OutRun 2 SP patches
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

#if defined(_DEBUG)
#define OR2_LOG(fmt, ...) do { \
	printf("OutRun2Patch: " fmt "\n", ##__VA_ARGS__); \
} while(0)
#else
#define OR2_LOG(...) do {} while(0)
#endif

// ── Hook functions ────────────────────────────────────────────────

static int __stdcall Or2MbRecvHook(uint32_t a1, uint32_t a2, uint32_t a3) {
	return 0;
}
static int __stdcall Or2MbSendHook(uint32_t a1, uint32_t a2, uint32_t a3) {
	return 0;
}
static int __stdcall Or2LinkOkHook(uint32_t a1, uint32_t a2, uint32_t a3) {
	return 1;
}
static int __cdecl Or2GetStatHook(int handle) {
	int stat = -3;
	if (handle) stat = *(char*)(handle + 1);
	if (stat != 3) {
		if (handle) stat = *(char*)(handle + 1);
		Sleep(0);
	}
	return stat;
}

// ── XBE hash constants ───────────────────────────────────────────

// OutRun 2 (GDX-0004A)
static const uint64_t kOR2Hashes[] = {
	0xCF00805970F689C8ULL, // raw
	0xB9B6863F9436F39DULL, // runtime
};
// OutRun 2 Rev B (GDX-0004B)
static const uint64_t kOR2BHashes[] = {
	0x581E087AF72DFFF1ULL, // raw
	0xA869E0DD1378BCE0ULL, // runtime
};
// OutRun 2 Special Tours (GDX-0011)
static const uint64_t kOR2SPHashes[] = {
	0xE18A426FFCDE2540ULL, // raw
	0x2D6CAA8CA616A176ULL, // runtime
};

enum Or2Game { OR2_NONE, OR2_A, OR2_B, OR2_SP };

bool g_ChihiroOutRun2Game = false;

static Or2Game IdentifyOr2Game(uint64_t xbeHash)
{
	for (auto h : kOR2Hashes)   if (xbeHash == h) return OR2_A;
	for (auto h : kOR2BHashes)  if (xbeHash == h) return OR2_B;
	for (auto h : kOR2SPHashes) if (xbeHash == h) return OR2_SP;
	return OR2_NONE;
}

bool IsOutRun2Xbe(uint64_t xbeHash)
{
	return IdentifyOr2Game(xbeHash) != OR2_NONE;
}

// ── Main patch entry point ───────────────────────────────────────

void ApplyOutRun2Patches(uint64_t xbeHash, uint32_t imageSize)
{
	Or2Game game = IdentifyOr2Game(xbeHash);
	g_ChihiroOutRun2Game = game != OR2_NONE;
	const char* gameName = "???";
	if (game == OR2_A)  gameName = "OutRun 2 (GDX-0004A)";
	if (game == OR2_B)  gameName = "OutRun 2 Rev B (GDX-0004B)";
	if (game == OR2_SP) gameName = "OutRun 2 SP (GDX-0011)";
	OR2_LOG("Applying patches for %s (imageSize=0x%X)", gameName, imageSize);

	// ═══════════════════════════════════════════════════════════════
	// Chihiro board detection — force return 1
	// Pattern: MOV ECX,[dword]; XOR EAX,EAX; CMP ECX,1; SETE AL; RET
	// ═══════════════════════════════════════════════════════════════
	{
		static const uint8_t kBoardDetectPat[] = {
			0x8B, 0x0D, 0xFF,0xFF,0xFF,0xFF,
			0x33, 0xC0,
			0x83, 0xF9, 0x01,
			0x0F, 0x94, 0xC0,
			0xC3
		};
		uintptr_t va = ScanXbe(kBoardDetectPat, sizeof(kBoardDetectPat), imageSize);
		if (va) {
			static const uint8_t kAlways1[] = { 0xB8,0x01,0x00,0x00,0x00, 0xC3, 0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90 };
			PatchXbeBytes(va, kAlways1, sizeof(kAlways1));
			OR2_LOG("Board detection forced at 0x%08X", (unsigned)va);
		} else {
			OR2_LOG("Board detection pattern not found!");
		}
	}

	// ═══════════════════════════════════════════════════════════════
	// LinkOK — return 1 (link OK)
	// ═══════════════════════════════════════════════════════════════
	{
		static const uint8_t kLinkOkPat[] = {
			0x85,0xC0, 0x75,0x08, 0xB8,0xFE,0xFF,0xFF,0xFF, 0xC2,0x0C,0x00
		};
		auto hits = ScanXbeAll(kLinkOkPat, sizeof(kLinkOkPat), imageSize);
		for (auto va : hits) {
			if (va >= 5 && ((const uint8_t*)(va - 5))[0] == 0xA1) {
				PatchWithJmp(va - 5, (const void*)&Or2LinkOkHook);
				OR2_LOG("LinkOK patched at 0x%08X", (unsigned)(va - 5));
			}
		}
		if (hits.empty()) OR2_LOG("LinkOK pattern not found");
	}

	// ═══════════════════════════════════════════════════════════════
	// DIMM-board ready bypass (always return 1)
	// ═══════════════════════════════════════════════════════════════
	{
		static const uint8_t kDimmReadyPat[] = {
			0x83,0x3D, 0xFF,0xFF,0xFF,0xFF, 0x04,
			0x1B,0xC0, 0x40, 0xC3
		};
		uintptr_t va = ScanXbe(kDimmReadyPat, sizeof(kDimmReadyPat), imageSize);
		if (va) {
			static const uint8_t kAlwaysTrue[] = { 0xB8,0x01,0x00,0x00,0x00, 0xC3, 0x90,0x90,0x90,0x90,0x90 };
			PatchXbeBytes(va, kAlwaysTrue, sizeof(kAlwaysTrue));
			OR2_LOG("DIMM-ready patched at 0x%08X", (unsigned)va);
		} else {
			OR2_LOG("DIMM-ready pattern not found");
		}
	}

	// ═══════════════════════════════════════════════════════════════
	// D3D_KickOffAndWaitForIdle NOP-stub
	// Pattern: MOV EAX,[g_pD3DDevice]; MOV ECX,[EAX+2C]; PUSH 2; PUSH ECX; CALL ...
	// ═══════════════════════════════════════════════════════════════
	{
		static const uint8_t kKickIdlePat[] = {
			0xA1, 0xFF,0xFF,0xFF,0xFF, 0x8B, 0x48, 0x2C,
			0x6A, 0x02, 0x51, 0xE8
		};
		uintptr_t va = ScanXbe(kKickIdlePat, sizeof(kKickIdlePat), imageSize);
		if (va) {
			static const uint8_t kRet = 0xC3;
			PatchXbeBytes(va, &kRet, 1);
			OR2_LOG("KickOffAndWaitForIdle stubbed at 0x%08X", (unsigned)va);
		} else {
			OR2_LOG("KickOffAndWaitForIdle pattern not found");
		}
	}

	// ═══════════════════════════════════════════════════════════════
	// CRI ADXF_GetStat hook
	// ═══════════════════════════════════════════════════════════════
	// {
	// 	static const uint8_t kGetStatPat[] = {
	// 		0x8B, 0x44, 0x24, 0x04, 0x85, 0xC0, 0x75, 0x13, 0x68
	// 	};
	// 	uintptr_t va = ScanXbe(kGetStatPat, sizeof(kGetStatPat), imageSize);
	// 	if (va) {
	// 		PatchWithJmp(va, (const void*)&Or2GetStatHook);
	// 		OR2_LOG("CRI GetStat hooked at 0x%08X", (unsigned)va);
	// 	} else {
	// 		OR2_LOG("CRI GetStat pattern not found");
	// 	}
	// }

	// ═══════════════════════════════════════════════════════════════
	// Card reader state machine stub
	// ═══════════════════════════════════════════════════════════════
	// {
	// 	static const uint8_t kCardReaderPat[] = {
	// 		0xA1, 0xFF,0xFF,0xFF,0xFF, 0x83,0xEC,0x08, 0x48, 0x83,0xF8,0x05, 0x0F,0x87
	// 	};
	// 	uintptr_t va = ScanXbe(kCardReaderPat, sizeof(kCardReaderPat), imageSize);
	// 	if (va) {
	// 		static const uint8_t kRet0[] = { 0x33,0xC0, 0xC3 };
	// 		PatchXbeBytes(va, kRet0, sizeof(kRet0));
	// 		OR2_LOG("Card reader stubbed at 0x%08X", (unsigned)va);
	// 	} else {
	// 		OR2_LOG("Card reader pattern not found");
	// 	}
	// }

	// ═══════════════════════════════════════════════════════════════
	// JVS discovery bypass (return 0 immediately)
	// ═══════════════════════════════════════════════════════════════
	// {
	// 	static const uint8_t kJvsDiscPat[] = {
	// 		0x53, 0x56, 0x57,
	// 		0xE8, 0xFF,0xFF,0xFF,0xFF,
	// 		0x8B, 0xF8,
	// 		0xE8, 0xFF,0xFF,0xFF,0xFF,
	// 		0x8B, 0xF0,
	// 		0xA1, 0xFF,0xFF,0xFF,0xFF,
	// 		0x33, 0xDB,
	// 		0x3B, 0xC3
	// 	};
	// 	uintptr_t va = ScanXbe(kJvsDiscPat, sizeof(kJvsDiscPat), imageSize);
	// 	if (va) {
	// 		static const uint8_t kRet0[] = { 0x33,0xC0, 0xC3 };
	// 		PatchXbeBytes(va, kRet0, sizeof(kRet0));
	// 		OR2_LOG("JVS discovery bypassed at 0x%08X", (unsigned)va);
	// 	} else {
	// 		OR2_LOG("JVS discovery pattern not found");
	// 	}
	// }

	// ═══════════════════════════════════════════════════════════════
	// Driveboard init skip — return 1 immediately
	// OR2SP: PUSH ECX; MOV EAX,[state]; PUSH EBX; PUSH ESI; XOR ESI,ESI;
	//        TEST EAX,EAX; MOV EBX,7
	// OR2:   PUSH ECX; MOV EAX,[state]; PUSH EBP; XOR EBP,EBP; CMP EAX,8
	// OR2B:  No driveboard code
	// ═══════════════════════════════════════════════════════════════
	{
		static const uint8_t kRet1[] = { 0xB8,0x01,0x00,0x00,0x00, 0xC3 };

		// OR2SP pattern
		static const uint8_t kDriveInitSP[] = {
			0x51,
			0xA1, 0xFF,0xFF,0xFF,0xFF,
			0x53, 0x56, 0x33, 0xF6, 0x85, 0xC0,
			0xBB, 0x07, 0x00, 0x00, 0x00
		};
		uintptr_t va = ScanXbe(kDriveInitSP, sizeof(kDriveInitSP), imageSize);
		if (va) {
			PatchXbeBytes(va, kRet1, sizeof(kRet1));
			OR2_LOG("Driveboard init (SP) skipped at 0x%08X", (unsigned)va);
		}

		// OR2 pattern
		static const uint8_t kDriveInitOR2[] = {
			0x51,
			0xA1, 0xFF,0xFF,0xFF,0xFF,
			0x55, 0x33, 0xED, 0x83, 0xF8, 0x08
		};
		va = ScanXbe(kDriveInitOR2, sizeof(kDriveInitOR2), imageSize);
		if (va) {
			PatchXbeBytes(va, kRet1, sizeof(kRet1));
			OR2_LOG("Driveboard init (OR2) skipped at 0x%08X", (unsigned)va);
		}
	}

	// ═══════════════════════════════════════════════════════════════
	// Network firmware version check bypass (OR2SP)
	// CheckNetwork: SUB ESP,60; CALL getInfo; XOR ECX,ECX;
	// MOV CH,[EAX+14]; MOV CL,[EAX+15]; CMP ECX,1101
	// Fails with Error 14 when firmware version < 11.01
	// ═══════════════════════════════════════════════════════════════
	{
		static const uint8_t kNetFwPat[] = {
			0x83, 0xEC, 0x60,
			0xE8, 0xFF,0xFF,0xFF,0xFF,
			0x33, 0xC9, 0x8A, 0x68, 0x14, 0x8A, 0x48, 0x15,
			0x81, 0xF9, 0x01, 0x11, 0x00, 0x00
		};
		uintptr_t va = ScanXbe(kNetFwPat, sizeof(kNetFwPat), imageSize);
		if (va) {
			static const uint8_t kRet1[] = { 0xB8,0x01,0x00,0x00,0x00, 0xC3 };
			PatchXbeBytes(va, kRet1, sizeof(kRet1));
			OR2_LOG("Network firmware check bypassed at 0x%08X", (unsigned)va);
		} else {
			OR2_LOG("Network firmware check pattern not found (OK for OR2/OR2B)");
		}
	}

	// ═══════════════════════════════════════════════════════════════
	// ═══════════════════════════════════════════════════════════════
	// D3D::CResource::IsBusy — disable FENCE check, keep busy-bits
	// The function checks two things:
	//   1) Hardware busy bits (0x780000) — needed for resource sync
	//   2) Per-resource fence token vs D3DDevice PUT/GET — deadlocks
	// We patch only the fence comparison: change JZ (skip-if-no-token)
	// to JMP (always skip fence). Busy-bit checks remain intact.
	//
	// Function layout (sub_15C2B0 in OR2SP):
	//   +0x50: MOV EDX,[ECX+8]  — load fence token
	//   +0x53: TEST EDX,EDX
	//   +0x55: JZ  +0x1D        — 74 1D → skip fence if token==0
	//          ... fence comparison ...
	//          return 1 if pending
	//   +0x74: XOR EAX,EAX; MOV [ECX+8],0; POP EDI; RET 4
	//
	// Patch: change byte at +0x55 from 0x74 (JZ) to 0xEB (JMP)
	// ═══════════════════════════════════════════════════════════════
	{
		// Match function start: MOV ECX,[ESP+4]; MOV EAX,[ECX]; MOV EDX,EAX;
		//                       AND EDX,0x70000; CMP EDX,0x50000
		static const uint8_t kBusyFencePat[] = {
			0x8B, 0x4C, 0x24, 0x04,            // MOV ECX, [ESP+4]
			0x8B, 0x01,                         // MOV EAX, [ECX]
			0x8B, 0xD0,                         // MOV EDX, EAX
			0x81, 0xE2, 0x00, 0x00, 0x07, 0x00,// AND EDX, 0x00070000
			0x81, 0xFA, 0x00, 0x00, 0x05, 0x00 // CMP EDX, 0x00050000
		};
		// The fence token load+test+JZ is at offset +0x5D from function start:
		//   8B 51 08  85 D2  74 1D
		// We verify bytes at +0x5D before patching.
		static const uint8_t kFenceCheck[] = {
			0x8B, 0x51, 0x08,  // MOV EDX, [ECX+8]
			0x85, 0xD2,        // TEST EDX, EDX
			0x74, 0x1D         // JZ +0x1D
		};

		auto busyHits = ScanXbeAll(kBusyFencePat, sizeof(kBusyFencePat), imageSize, 8);
		for (auto va : busyHits) {
			// Verify the fence check bytes at +0x5D
			const uint8_t* fenceAddr = (const uint8_t*)(va + 0x5D);
			bool fenceMatch = true;
			for (size_t j = 0; j < sizeof(kFenceCheck); j++) {
				if (fenceAddr[j] != kFenceCheck[j]) { fenceMatch = false; break; }
			}
			if (fenceMatch) {
				// Patch JZ → JMP at offset +0x62 (the 0x74 byte)
				PatchXbeBytes(va + 0x62, (const uint8_t*)"\xEB", 1);
				OR2_LOG("D3D::CResource::IsBusy fence bypass at 0x%08X (+0x62: JZ->JMP)", (unsigned)va);
			} else {
				OR2_LOG("D3D::CResource::IsBusy at 0x%08X: fence check mismatch at +0x5D, skipping", (unsigned)va);
			}
		}
		if (busyHits.empty())
			OR2_LOG("D3D::CResource::IsBusy pattern not found");
	}

	OR2_LOG("All patches applied for %s", gameName);
}
