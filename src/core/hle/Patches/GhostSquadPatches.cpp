// ******************************************************************
// *  Cxbx Ghost Squad patches
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
#include "devices\chihiro\JvsIo.h"

// Keep bring-up details available to CXBXR's normal debug logger without
// forcing synchronous C:\temp file writes in production or on Android.
#if defined(_DEBUG)
#define GS_LOG(...) EmuLog(LOG_LEVEL::DEBUG, __VA_ARGS__)
#else
#define GS_LOG(...) do {} while (0)
#endif

// ── Known hashes ──────────────────────────────────────────────────
static const uint64_t kGhostSquadHash = 0x6DEBC797D7621B7Full;

bool IsGhostSquadXbe(uint64_t xbeHash)
{
	return xbeHash == kGhostSquadHash;
}

// ── Hook functions ────────────────────────────────────────────────

static int __stdcall GsMbRecvHook(uint32_t a1, uint32_t a2, uint32_t a3)
{
	return 0; // no data available
}

static int __stdcall GsMbSendHook(uint32_t a1, uint32_t a2, uint32_t a3)
{
	return 0; // send always succeeds (discards)
}

static int __stdcall GsLinkOkHook(uint32_t a1, uint32_t a2, uint32_t a3)
{
	return 1; // link is OK
}

// ── QuickReboot interceptor ───────────────────────────────────────
// Called from HalReturnToFirmware. Return true to suppress the reboot.
static bool GsQuickRebootInterceptor()
{
	GS_LOG("GS: QuickReboot intercepted — suppressing reboot");
	return true;
}

static uintptr_t g_initDoneVar = 0;   // address of init-done check variable
static uintptr_t g_dimmReadyVar = 0;  // address of DIMM-ready check variable

#if defined(_DEBUG)
// ── Debug-only diagnostics ────────────────────────────────────────
static LONG WINAPI GsCrashHandler(EXCEPTION_POINTERS* ep)
{
	if (ep->ExceptionRecord->ExceptionCode == 0x80000003 ||  // breakpoint
		ep->ExceptionRecord->ExceptionCode == 0x406D1388) {  // thread name
		return EXCEPTION_CONTINUE_SEARCH;
	}
	GS_LOG("GS CRASH: code=0x%08X addr=0x%08X EIP=0x%08X ESP=0x%08X",
		(unsigned)ep->ExceptionRecord->ExceptionCode,
		(unsigned)(uintptr_t)ep->ExceptionRecord->ExceptionAddress,
		(unsigned)ep->ContextRecord->Eip,
		(unsigned)ep->ContextRecord->Esp);
	return EXCEPTION_CONTINUE_SEARCH;
}

// ── Exit handler ──────────────────────────────────────────────────
static void GsAtExitHandler()
{
	GS_LOG("GS: atexit handler called — game exiting normally");
}

static DWORD WINAPI GsMonitorThread(LPVOID)
{
	GS_LOG("GS: Monitor thread started");
	for (int i = 0; i < 300; i++) {
		Sleep(2000);
		// Read key game state variables
		uint32_t dimmState = *(volatile uint32_t*)0x4CCF24;
		uint32_t mediaFlag = *(volatile uint32_t*)0x4BF5D4;
		uint32_t initDoneVal = g_initDoneVar ? *(volatile uint32_t*)g_initDoneVar : 0xDEAD;
		uint32_t dimmReadyVal = g_dimmReadyVar ? *(volatile uint32_t*)g_dimmReadyVar : 0xDEAD;
		// Sample first few words of media status struct at 0x4BEE64
		uint32_t media0 = *(volatile uint32_t*)0x4BEE64;
		uint32_t media7 = *(volatile uint32_t*)(0x4BEE64 + 28); // v0[7] error code
		uint16_t media12w = *(volatile uint16_t*)(0x4BEE64 + 24); // v0+12 (word)
		// D3D state: dword_447AD8 = D3D8 object (from sub_178730/Direct3DCreate8)
		//            dword_447AF4 = D3DDevice ptr (from Direct3D_CreateDevice)
		//            dword_447A30 = Present params width (640 if D3D init reached)
		uint32_t d3d8Obj = *(volatile uint32_t*)0x447AD8;
		uint32_t d3dDev  = *(volatile uint32_t*)0x447AF4;
		uint32_t ppWidth = *(volatile uint32_t*)0x447A30;
		// off_2ABEA8 = game context pointer set in sub_89A20
		uint32_t gameCtx = *(volatile uint32_t*)0x2ABEA8;
		GS_LOG("GS: tick %d  dimm=0x%X mflag=%u initV=0x%X dimmV=0x%X  "
			"m0=0x%X m7=0x%X m12w=%u  "
			"d3d8=0x%X dev=0x%X ppW=%u ctx=0x%X",
			i + 1, dimmState, mediaFlag, initDoneVal, dimmReadyVal,
			media0, media7, media12w,
			d3d8Obj, d3dDev, ppWidth, gameCtx);
	}
	GS_LOG("GS: Monitor thread done (60s elapsed)");
	return 0;
}
#endif

// ── DIMM init hook ────────────────────────────────────────────────
// Not used — hooking sub_14DB00 skips critical section init.
// Instead we call the media lib init (sub_14DE70) directly.

// ── Apply patches ─────────────────────────────────────────────────
void ApplyGhostSquadPatches(uint64_t xbeHash, uint32_t imageSize)
{
	g_jvs_game_type = JvsGameType::GhostSquad;
	GS_LOG("GS: Applying patches (imageSize=0x%X, hash=0x%016llX)",
		imageSize, (unsigned long long)xbeHash);

	// The crash handler, atexit logger, and two-second memory sampler are useful
	// while developing a new patch, but they add a permanent thread and
	// synchronous logging overhead to every production game session.
#if defined(_DEBUG)
	AddVectoredExceptionHandler(1, GsCrashHandler);
	atexit(GsAtExitHandler);
	CreateThread(nullptr, 0, GsMonitorThread, nullptr, 0, nullptr);
#endif

	// === QuickReboot interceptor ===
	extern bool (*g_pfnQuickRebootInterceptor)();
	g_pfnQuickRebootInterceptor = &GsQuickRebootInterceptor;
	GS_LOG("GS: QuickReboot interceptor installed");

	// === LinkOK — JMP hook ===
	// Pattern: TEST EAX,EAX; JNZ +8; MOV EAX,0xFFFFFFFE; RET 0Ch
	// Preceded by MOV EAX,[addr] — hook from that point.
	{
		static const uint8_t kLinkOkPat[] = {
			0x85,0xC0, 0x75,0x08, 0xB8,0xFE,0xFF,0xFF,0xFF, 0xC2,0x0C,0x00
		};
		auto linkOkHits = ScanXbeAll(kLinkOkPat, sizeof(kLinkOkPat), imageSize);
		int hooked = 0;
		for (auto va : linkOkHits) {
			if (va >= 5 && ((const uint8_t*)(va - 5))[0] == 0xA1) {
				PatchWithJmp(va - 5, (const void*)&GsLinkOkHook);
				GS_LOG("GS: LinkOK patched at 0x%08X", (unsigned)(va - 5));
				hooked++;
			}
		}
		if (!hooked) {
			GS_LOG("GS: LinkOK pattern not found!");
		}
	}

	// === MbRecvPacket / MbSendPacket — JMP hooks ===
	{
		static const uint8_t kMbFuncPat[] = {
			0x83,0xEC,0x08, 0x8D,0x44,0x24,0x04, 0x50, 0x6A,0x00,
			0xE8,0xFF,0xFF,0xFF,0xFF,
			0x8D,0x0C,0x24, 0x51, 0x6A,0x01, 0xE8
		};
		auto mbHits = ScanXbeAll(kMbFuncPat, sizeof(kMbFuncPat), imageSize);
		if (mbHits.size() >= 2) {
			PatchWithJmp(mbHits[0], (const void*)&GsMbRecvHook);
			PatchWithJmp(mbHits[1], (const void*)&GsMbSendHook);
			GS_LOG("GS: MbRecv/MbSend hooked at 0x%08X / 0x%08X",
				(unsigned)mbHits[0], (unsigned)mbHits[1]);
		} else {
			GS_LOG("GS: MbRecvPacket/MbSendPacket not found! (%zu hits)", mbHits.size());
		}
	}

	// === DIMM-board ready bypass (always return 1) ===
	// CMP DWORD PTR [addr], 4; SBB EAX,EAX; INC EAX; RET
	// Also check for Init-done right before it (MOV EAX,[addr]; RET; CC CC CC CC)
	{
		static const uint8_t kDimmReadyPat[] = {
			0x83,0x3D, 0xFF,0xFF,0xFF,0xFF, 0x04,
			0x1B,0xC0, 0x40, 0xC3
		};
		uintptr_t dimmReadyVA = ScanXbe(kDimmReadyPat, sizeof(kDimmReadyPat), imageSize);
		if (dimmReadyVA) {
			// Patch init-done FIRST (before dimm-ready overwrites the area)
			for (uintptr_t check = dimmReadyVA - 0x10; check < dimmReadyVA; check++) {
				uint8_t* p = (uint8_t*)check;
				if (p[0] == 0xA1 && p[5] == 0xC3 && p[6] == 0xCC) {
					g_initDoneVar = *(uint32_t*)(check + 1); // MOV EAX,[addr]
					static const uint8_t kRet1[] = { 0xB8,0x01,0x00,0x00,0x00, 0xC3 };
					PatchXbeBytes(check, kRet1, sizeof(kRet1));
					GS_LOG("GS: Init-done patched at 0x%08X (var=0x%08X)", (unsigned)check, (unsigned)g_initDoneVar);
					break;
				}
			}

			// Extract DIMM-ready variable address before patching
			g_dimmReadyVar = *(uint32_t*)(dimmReadyVA + 2); // CMP [addr], 4

			static const uint8_t kAlwaysTrue[] = {
				0xB8,0x01,0x00,0x00,0x00, 0xC3,
				0x90,0x90,0x90,0x90,0x90
			};
			PatchXbeBytes(dimmReadyVA, kAlwaysTrue, sizeof(kAlwaysTrue));
			GS_LOG("GS: DIMM-ready patched at 0x%08X", (unsigned)dimmReadyVA);
		} else {
			GS_LOG("GS: DIMM-ready pattern not found");
		}
	}

	GS_LOG("GS: All patches applied");

	// === Initialize media library critical section directly ===
	// sub_14DE70 initializes a critical section at 0x4BF5B8, clears 0x88 bytes
	// at 0x4BF550, and sets 0x4BF5D4 = 1. We can't call the XBE function
	// (import thunks may not be resolved yet), so do it manually.
	// Pattern: find the REP STOSD init to locate the struct addresses.
	{
		static const uint8_t kMediaInitPat[] = {
			0x57,                       // PUSH EDI
			0x33, 0xC0,                 // XOR EAX, EAX
			0xB9, 0x22, 0x00, 0x00, 0x00, // MOV ECX, 0x22
			0xBF                        // MOV EDI, imm32...
		};
		uintptr_t mediaInitVA = ScanXbe(kMediaInitPat, sizeof(kMediaInitPat), imageSize);
		if (mediaInitVA) {
			// Extract addresses from the function body:
			// +9: MOV EDI, <struct_addr>  (4 bytes imm32)
			// +15: PUSH <critsec_addr>     (0x68 + 4 bytes imm32)
			// +20: MOV [flag_addr], 1      (C7 05 + 4 bytes addr + 4 bytes val)
			uintptr_t structAddr = *(uint32_t*)(mediaInitVA + 9);
			uintptr_t critsecAddr = *(uint32_t*)(mediaInitVA + 16); // skip 0x68 opcode
			uintptr_t flagAddr = *(uint32_t*)(mediaInitVA + 22);
			*(volatile uint32_t*)flagAddr = 1;

			// Initialize the critical section
			InitializeCriticalSection((LPCRITICAL_SECTION)critsecAddr);

			GS_LOG("GS: Media lib init: struct=0x%08X critsec=0x%08X flag=0x%08X",
				(unsigned)structAddr, (unsigned)critsecAddr, (unsigned)flagAddr);
		} else {
			GS_LOG("GS: Media init pattern not found");
		}
	}
}
