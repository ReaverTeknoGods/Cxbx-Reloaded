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
#include "common\xbox_types.h"
#include "core\kernel\support\Emu.h"
#include "core\kernel\init\CxbxKrnl.h"

#include "devices\chihiro\JvsIo.h"

#include <initializer_list>
#include <map>
#include <string>
#include <cstdio>

#if !defined(_DEBUG)
#define printf(...) do {} while (0)
#define vprintf(...) do {} while (0)
#endif

extern std::map<std::string, xbox::addr_xt> g_SymbolAddresses;

#if 0 // Diagnostic logging — enable when debugging
// ── Gundam diagnostic log ─────────────────────────────────────────
static FILE* g_GundamDiagFile = nullptr;

static void GundamDiagOpen() {
	if (!g_GundamDiagFile) {
		g_GundamDiagFile = fopen("C:\\temp\\gundam_diag.txt", "w");
	}
}

static void GundamDiagLog(const char* fmt, ...) {
	GundamDiagOpen();
	if (g_GundamDiagFile) {
		va_list ap;
		va_start(ap, fmt);
		vfprintf(g_GundamDiagFile, fmt, ap);
		va_end(ap);
		fflush(g_GundamDiagFile);
	}
	va_list ap2;
	va_start(ap2, fmt);
	vprintf(fmt, ap2);
	va_end(ap2);
}

// ── MediaBoard diagnostic log ─────────────────────────────────────
static FILE* g_MbDiagFile = nullptr;
static DWORD g_MbRecvCount = 0;
static DWORD g_MbSendCount = 0;

static void MbDiagLog(const char* fmt, ...) {
	if (!g_MbDiagFile) {
		g_MbDiagFile = fopen("C:\\temp\\gundam_mb_diag.txt", "w");
	}
	if (g_MbDiagFile) {
		va_list ap;
		va_start(ap, fmt);
		vfprintf(g_MbDiagFile, fmt, ap);
		va_end(ap);
		fflush(g_MbDiagFile);
	}
}
#endif

// ── Card reader emulation ─────────────────────────────────────────
// sub_992F0 is a 6-state card reader state machine. It polls hardware
// for card insertion/read/write. Without real hardware, it loops forever.
// This hook provides file-based card I/O: reads/writes card data to
// card.bin alongside the XBE file.
//
// Card reader states (original sub_992F0 DEC EAX; CMP EAX,5; JA):
//   State 1: Initialize reader
//   State 2: Wait for card insertion
//   State 3: Read card data
//   State 4: Process card
//   State 5: Write card data
//   State 6: Eject/finalize
//
// Returning 0 = function complete (no operation pending)

static char g_gundamCardPath[MAX_PATH] = {};
static constexpr size_t GUNDAM_CARD_SIZE = 1024; // typical Sega IC card data size

static void GundamCardPathInit() {
	if (g_gundamCardPath[0] != '\0') return;
	char xbeDir[MAX_PATH];
	strncpy(xbeDir, szFilePath_Xbe, MAX_PATH - 1);
	xbeDir[MAX_PATH - 1] = '\0';
	char* lastSlash = strrchr(xbeDir, '\\');
	if (!lastSlash) lastSlash = strrchr(xbeDir, '/');
	if (lastSlash) *(lastSlash + 1) = '\0';
	else strcat(xbeDir, "\\");
	snprintf(g_gundamCardPath, MAX_PATH, "%scard.bin", xbeDir);
	printf("GundamCard: path=%s\n", g_gundamCardPath);
}

static bool GundamLoadCard(uint8_t* dest, size_t maxLen) {
	GundamCardPathInit();
	FILE* f = fopen(g_gundamCardPath, "rb");
	if (!f) return false;
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (sz <= 0 || (size_t)sz > maxLen) { fclose(f); return false; }
	size_t r = fread(dest, 1, sz, f);
	fclose(f);
	printf("GundamCard: loaded %zu bytes\n", r);
	return r > 0;
}

static bool GundamSaveCard(const uint8_t* src, size_t len) {
	GundamCardPathInit();
	FILE* f = fopen(g_gundamCardPath, "wb");
	if (!f) return false;
	size_t w = fwrite(src, 1, len, f);
	fclose(f);
	printf("GundamCard: saved %zu bytes\n", w);
	return w == len;
}

// Card reader hook — replaces sub_992F0
// Returns 0 (idle/complete) to prevent the state machine from blocking.
// The game's main loop calls this periodically; returning 0 means
// "no card operation in progress" which is correct for attract mode.
static int __cdecl GundamCardReaderHook() {
	return 0;
}

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

// ── Test-menu state monitor + delayed flag-setter thread ──────────
// Monitors key state variables and logs them.  Also, once the
// MediaBoard init finishes (gate=1 at dword_C9BB8), sets sf14 and
// sf15 so the main-thread wait-loop exits naturally.
// We CANNOT NOP the JZ instructions — doing so lets the main thread
// into code paths that reduce rendering from 168 to 2 prims/frame.
static uintptr_t g_TestMenuStatusStruct = 0;  // addr of status struct from sub_36300
static DWORD WINAPI GundamTestMenuMonitor(LPVOID) {
	// Wait for gate=1 (init complete), then set ISR-ready flags so main thread proceeds
	for (int tick = 0; tick < 600; tick++) { // 60 seconds
		Sleep(100);
		uint32_t gate = *(volatile uint32_t*)0xC9BB8;
		if (gate == 1 && g_TestMenuStatusStruct) {
			Sleep(500); // let memset finish
			*(volatile uint8_t*)(g_TestMenuStatusStruct + 0x10) = 1;  // init-done
			*(volatile uint8_t*)(g_TestMenuStatusStruct + 0x14) = 1;  // ISR ready 1
			*(volatile uint8_t*)(g_TestMenuStatusStruct + 0x15) = 1;  // ISR ready 2
			printf("GundamMonitor: flags set at tick %d\n", tick);
			return 0;
		}
	}
	printf("GundamMonitor: timeout — gate never reached 1\n");
	return 1;
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
static constexpr uint64_t kGundamTestMenuHash = 0x955c9cb503f1520fULL; // gs_gtest.xbe
static constexpr uint64_t kGundamMainGameHash = 0x6b43ff6b6398a88fULL; // gs.xbe

static const uint64_t kGundamHashes[] = {
	kGundamTestMenuHash,
	kGundamMainGameHash,
};

static bool IsGundamTestMenuXbe(uint64_t xbeHash)
{
	return xbeHash == kGundamTestMenuHash;
}

static uintptr_t LookupSymbolAddress(std::initializer_list<const char*> symbolNames)
{
	for (const char* symbolName : symbolNames) {
		auto it = g_SymbolAddresses.find(symbolName);
		if (it != g_SymbolAddresses.end() && it->second != 0) {
			return it->second;
		}
	}

	return 0;
}

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

static void ApplyGundamTestMenuBootPatches(uint32_t imageSize)
{
	printf("GundamPatch: enabling test-menu boot profile\n");

	// The service/test menu still needs the older DIMM startup bypass set.
	// Keep these scoped to gs_gtest.xbe so the main game stays on the current profile.
	{
		static const uint8_t kErr5Pat[] = {
			0xE8, 0xFF,0xFF,0xFF,0xFF,
			0x80,0x78,0x10,0x01,
			0x74,0x0A,
			0xC7,0x05, 0xFF,0xFF,0xFF,0xFF,
			0x05,0x00,0x00,0x00
		};
		uintptr_t err5VA = ScanXbe(kErr5Pat, sizeof(kErr5Pat), imageSize);
		if (err5VA) {
			static const uint8_t kJmp = 0xEB;
			PatchXbeBytes(err5VA + 9, &kJmp, 1);
			printf("GundamPatch[test-menu]: Error-5 bypass at 0x%08X\n", (unsigned)(err5VA + 9));
		} else {
			printf("GundamPatch[test-menu]: Error-5 pattern not found\n");
		}
	}

	// NOTE: The "init-done" pattern (A1 XX XX XX XX C3 CC...) was REMOVED here.
	// In the test-menu XBE it matches sub_110E0 which returns dword_AA308 —
	// the font/renderer object pointer (NOT an init-done flag).  Patching it
	// to return 1 makes every sub_20F20 text-render call use this=1 as the
	// font object, breaking all text output.

	// NOTE: GundamDimmInitHook is NOT applied for the test menu.
	// The hook writes to hardcoded main-game absolute addresses (0x688540,
	// 0x688CE4, 0x67ABF8, etc.) which are wrong for the test-menu XBE and
	// corrupt its memory, causing a crash.  The working reference build never
	// applied this hook to the test menu (its hardcoded pattern didn't match).

	{
		static const uint8_t kThreadGatePat[] = {
			0x51, 0x8B, 0x0D, 0xFF,0xFF,0xFF,0xFF,
			0x8D, 0x04, 0x24, 0x50, 0x51,
			0xC7, 0x44, 0x24, 0x08, 0x03, 0x01, 0x00, 0x00
		};
		uintptr_t threadGateVA = ScanXbe(kThreadGatePat, sizeof(kThreadGatePat), imageSize);
		if (threadGateVA) {
			static const uint8_t kRetTrue[] = { 0xB8,0x01,0x00,0x00,0x00, 0xC3 };
			PatchXbeBytes(threadGateVA, kRetTrue, sizeof(kRetTrue));
			printf("GundamPatch[test-menu]: Thread-gate patched at 0x%08X\n", (unsigned)threadGateVA);
		} else {
			printf("GundamPatch[test-menu]: Thread-gate pattern not found\n");
		}
	}

	// === JVS discovery: patch sub_35650 to force-init with capabilities ===
	// The main thread spins: while(sub_35650()==5) sub_2EA20();
	// sub_35650 is the JVS node discovery state machine.  On real hardware it
	// enumerates nodes and queries capabilities; on emulated hardware it loops
	// forever because the low-level bus I/O doesn't complete.
	//
	// sub_35A00 (JVS request builder) takes ONE parameter — the node address
	// starting at 1.  It indexes: dword_C4E10[151 * nodeAddr] for capabilities
	// and unk_CF25C[224 * nodeAddr + offset] for per-node data.
	// So we must populate data at NODE INDEX 1 (not 0).
	//
	// We overwrite sub_35650's prologue to:
	//   1. Set dword_C4E08 = 1                     (JVS init complete)
	//   2. Set byte_CEE59  = 1                     (1 node discovered)
	//   3. Set dword_C4E10[151*1] = 7              (cap: switches|coins|analog)
	//        → address = 0xC4E10 + 604 = 0xC506C
	//   4. Populate node-1 capability data:
	//        unk_CF25C[224*1 + 0xCC] = 2  (2 players)    → 0xCF408
	//        unk_CF25C[224*1 + 0xCD] = 14 (14 bits/player) → 0xCF409
	//        unk_CF25C[224*1 + 0xCE] = 2  (2 coin slots) → 0xCF40A
	//        unk_CF25C[224*1 + 0xCF] = 8  (8 analog ch)  → 0xCF40B
	//   5. Return 0 (so the while-loop exits immediately)
	{
		static const uint8_t kJvsLoopPat[] = {
			0xE8, 0xFF,0xFF,0xFF,0xFF,       // call sub_35650
			0x83, 0xF8, 0x05,                 // cmp eax, 5
			0x75, 0x0F,                       // jne skip_loop
			0xE8, 0xFF,0xFF,0xFF,0xFF,        // call sub_2EA20 (sleep)
			0xE8, 0xFF,0xFF,0xFF,0xFF,        // call sub_35650
			0x83, 0xF8, 0x05,                 // cmp eax, 5
			0x74, 0xF1                        // je loop_back
		};
		uintptr_t jvsLoopVA = ScanXbe(kJvsLoopPat, sizeof(kJvsLoopPat), imageSize);
		if (jvsLoopVA) {
			// Resolve CALL rel32 at jvsLoopVA to get sub_35650 address
			const uint8_t* callSite = (const uint8_t*)jvsLoopVA;
			int32_t rel32;
			memcpy(&rel32, callSite + 1, 4);
			uintptr_t sub35650 = jvsLoopVA + 5 + rel32;

			// Overwrite sub_35650 prologue with force-init + capabilities
			// All capability/node data at index 1 (matches sub_35A00's nodeAddr=1)
			static const uint8_t kForceInit[] = {
				// dword_C4E08 = 1 (JVS init complete)
				0xC7, 0x05, 0x08,0x4E,0x0C,0x00, 0x01,0x00,0x00,0x00,  // MOV DWORD [0xC4E08], 1
				// byte_CEE59 = 1 (1 node discovered)
				0xC6, 0x05, 0x59,0xEE,0x0C,0x00, 0x01,                  // MOV BYTE [0xCEE59], 1
				// dword_C4E10[151*1] = 7 (switches | coins | analog)
				0xC7, 0x05, 0x6C,0x50,0x0C,0x00, 0x07,0x00,0x00,0x00,  // MOV DWORD [0xC506C], 7
				// nodeTable[224*1 + 0xCC] = 2 (2 players)
				0xC6, 0x05, 0x08,0xF4,0x0C,0x00, 0x02,                  // MOV BYTE [0xCF408], 2
				// nodeTable[224*1 + 0xCD] = 14 (14 bits/player)
				0xC6, 0x05, 0x09,0xF4,0x0C,0x00, 0x0E,                  // MOV BYTE [0xCF409], 14
				// nodeTable[224*1 + 0xCE] = 2 (2 coin slots)
				0xC6, 0x05, 0x0A,0xF4,0x0C,0x00, 0x02,                  // MOV BYTE [0xCF40A], 2
				// nodeTable[224*1 + 0xCF] = 8 (8 analog channels)
				0xC6, 0x05, 0x0B,0xF4,0x0C,0x00, 0x08,                  // MOV BYTE [0xCF40B], 8
				// return 0
				0x33, 0xC0,                                               // XOR EAX, EAX
				0xC3                                                      // RET
			};
			PatchXbeBytes(sub35650, kForceInit, sizeof(kForceInit));
			printf("GundamPatch[test-menu]: JVS force-init+caps at 0x%08X "
				"(init=1, nodes=1, caps@idx1=0x07, 2P/14b/2c/8a)\n",
				(unsigned)sub35650);
		} else {
			printf("GundamPatch[test-menu]: JVS discovery loop pattern not found\n");
		}
	}

	// === Resolve status struct address for the monitor thread ===
	// The main-thread wait-loop at ~0x11D30 busy-waits for [struct+14h]
	// and [+15h].  We do NOT NOP the JZ here — doing so causes the main
	// thread to enter a bad code path that drops rendering to 2 prims.
	// Instead, the monitor thread will set the flags once init completes.
	{
		static const uint8_t kWaitLoopPat[] = {
			0xE8, 0xFF,0xFF,0xFF,0xFF,       // CALL sub_36300 (get status struct)
			0xE8, 0xFF,0xFF,0xFF,0xFF,       // CALL sub_36300 (loop entry)
			0x8A, 0x48, 0x14,                 // MOV CL, [EAX+14h]
			0x84, 0xC9,                       // TEST CL, CL
			0x74, 0xFF,                       // JZ loop
			0x8A, 0x48, 0x15,                 // MOV CL, [EAX+15h]
			0x84, 0xC9,                       // TEST CL, CL
			0x74, 0xFF                        // JZ loop
		};
		uintptr_t waitLoopVA = ScanXbe(kWaitLoopPat, sizeof(kWaitLoopPat), imageSize);
		if (waitLoopVA) {
			const uint8_t* callSite = (const uint8_t*)waitLoopVA;
			int32_t rel32;
			memcpy(&rel32, callSite + 1, 4);
			uintptr_t sub36300 = waitLoopVA + 5 + rel32;
			const uint8_t* fn = (const uint8_t*)sub36300;
			if (fn[0] == 0xB8 && fn[5] == 0xC3) {
				uintptr_t statusStruct;
				memcpy(&statusStruct, fn + 1, 4);
				g_TestMenuStatusStruct = statusStruct;
				printf("GundamPatch[test-menu]: Status struct at 0x%08X "
					"(will set flags from monitor thread after init)\n",
					(unsigned)statusStruct);
			}
		} else {
			printf("GundamPatch[test-menu]: Wait-loop pattern not found\n");
		}
	}

	// Launch state monitor + delayed flag-setter thread
	CreateThread(nullptr, 0, GundamTestMenuMonitor, nullptr, 0, nullptr);
	printf("GundamPatch[test-menu]: Monitor thread started\n");
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

void ApplyGundamPatches(uint64_t xbeHash, uint32_t imageSize)
{
	const bool isTestMenu = IsGundamTestMenuXbe(xbeHash);
	printf("GundamPatch: applying patches (imageSize=0x%X, xbeHash=0x%016llX%s)\n",
		imageSize,
		(unsigned long long)xbeHash,
		isTestMenu ? ", test-menu profile" : "");

	if (isTestMenu) {
		ApplyGundamTestMenuBootPatches(imageSize);
	}

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
	// Must stay enabled for ALL XBEs including test menu.
	// Without hooks, the ISR thread spin-loops on LPC handshake,
	// starving the render thread (168 → 2 prims/frame).
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
	// CMP DWORD PTR [addr], 4; SBB EAX,EAX; INC EAX; RET
	// Address varies per XBE (main=0x688550, test-menu=0xC9BC8), so wildcard it.
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
			0xA1, 0xFF,0xFF,0xFF,0xFF,
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
			0xA1, 0xFF,0xFF,0xFF,0xFF,
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
			0x51, 0x8B, 0x0D, 0xFF, 0xFF, 0xFF, 0xFF,
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
			0x8B, 0x15, 0xFF, 0xFF, 0xFF, 0xFF,
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
		static const uint8_t kKickIdlePat[] = { 0xA1, 0xFF, 0xFF, 0xFF, 0xFF, 0x8B, 0x48, 0x2C };
		uintptr_t kickIdleVA = ScanXbe(kKickIdlePat, sizeof(kKickIdlePat), imageSize);
		if (kickIdleVA) {
			static const uint8_t kRet = 0xC3;
			PatchXbeBytes(kickIdleVA, &kRet, 1);
			printf("GundamPatch: D3D_KickOffAndWaitForIdle stubbed at 0x%08X\n", (unsigned)kickIdleVA);
		} else {
			printf("GundamPatch: D3D_KickOffAndWaitForIdle pattern not found, trying symbol lookup...\n");
			kickIdleVA = LookupSymbolAddress({ "D3D_KickOffAndWaitForIdle" });
			if (kickIdleVA) {
				const uint8_t* probe = (const uint8_t*)kickIdleVA;
				if (probe[0] != 0xC3 && probe[0] != 0xCC) {
					static const uint8_t kRet2 = 0xC3;
					PatchXbeBytes(kickIdleVA, &kRet2, 1);
					printf("GundamPatch: D3D_KickOffAndWaitForIdle stubbed via symbol at 0x%08X\n", (unsigned)kickIdleVA);
				}
			} else {
				printf("GundamPatch: D3D_KickOffAndWaitForIdle NOT FOUND (will cause ~1 FPS!)\n");
			}
		}
	}
#endif

#if GP_BLOCK_RESOURCE
	// === D3D_BlockOnResource NOP-stub ===
	{
		const uintptr_t blockResVA = LookupSymbolAddress({
			"D3D_BlockOnResource",
			"D3D_BlockOnResource_0__LTCG_eax1",
			"D3D_BlockOnResource_0__LTCG_ecx1"
		});
		if (blockResVA != 0) {
			const uint8_t* probe = (const uint8_t*)blockResVA;
			if (probe[0] != 0xC2 && probe[0] != 0xC3 && probe[0] != 0xCC) {
				static const uint8_t kRet4[] = { 0xC2, 0x04, 0x00 }; // RET 4
				PatchXbeBytes(blockResVA, kRet4, sizeof(kRet4));
				printf("GundamPatch: D3D_BlockOnResource stubbed at 0x%08X\n", (unsigned)blockResVA);
			} else {
				printf("GundamPatch: D3D_BlockOnResource prologue mismatch at 0x%08X\n", (unsigned)blockResVA);
			}
		} else {
			printf("GundamPatch: D3D_BlockOnResource symbol not found\n");
		}
	}
#endif

	// NOTE: D3D_SetFence, CDevice_KickOff, D3D_MakeRequestedSpace must NOT be
	// stubbed — the HLE rendering pipeline calls them internally.

#if GP_BLOCK_VBLANK
	// === D3DDevice_BlockUntilVerticalBlank NOP-stub ===
	{
		const uintptr_t blockVBlankVA = LookupSymbolAddress({ "D3DDevice_BlockUntilVerticalBlank" });
		if (blockVBlankVA != 0) {
			const uint8_t* probe = (const uint8_t*)blockVBlankVA;
			if (probe[0] != 0xC2 && probe[0] != 0xC3) {
				static const uint8_t kRet = 0xC3;
				PatchXbeBytes(blockVBlankVA, &kRet, 1);
				printf("GundamPatch: D3DDevice_BlockUntilVerticalBlank stubbed at 0x%08X\n", (unsigned)blockVBlankVA);
			}
		} else {
			printf("GundamPatch: D3DDevice_BlockUntilVerticalBlank symbol not found\n");
		}
	}
#endif

#if GP_FLIP_PENDING
	// === CMiniport_IsFlipPending NOP-stub ===
	{
		const uintptr_t flipPendVA = LookupSymbolAddress({ "CMiniport_IsFlipPending" });
		if (flipPendVA != 0) {
			const uint8_t* probe = (const uint8_t*)flipPendVA;
			if (probe[0] != 0x33 && probe[0] != 0xC2 && probe[0] != 0xC3) {
				static const uint8_t kRet0[] = { 0x33, 0xC0, 0xC3 }; // XOR EAX,EAX; RET
				PatchXbeBytes(flipPendVA, kRet0, sizeof(kRet0));
				printf("GundamPatch: CMiniport_IsFlipPending stubbed at 0x%08X\n", (unsigned)flipPendVA);
			}
		} else {
			printf("GundamPatch: CMiniport_IsFlipPending symbol not found\n");
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
	// === Card reader hook (sub_992F0 → GundamCardReaderHook) ===
	// 6-state machine with retry loops (up to 60 retries). Hook provides
	// file-based card I/O and returns 0 (idle) for attract mode.
	{
		if (isTestMenu) {
			printf("GundamPatch: Card reader hook skipped for test-menu (main-XBE static offset)\n");
		} else {
			const uintptr_t sub992F0 = 0x000992F0;
			const uint8_t* probe = (const uint8_t*)sub992F0;
			if (probe[0] != 0xC3 && probe[0] != 0xCC) {
				GundamCardPathInit();
				PatchWithJmp(sub992F0, (const void*)&GundamCardReaderHook);
				printf("GundamPatch: Card reader hooked at 0x%08X (card=%s)\n",
					(unsigned)sub992F0, g_gundamCardPath);
			} else {
				printf("GundamPatch: Card reader already patched at 0x%08X\n", (unsigned)sub992F0);
			}
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
