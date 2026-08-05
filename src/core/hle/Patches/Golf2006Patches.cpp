// ******************************************************************
// *  Cxbx Sega Golf Club 2006: Next Tours patches
// *
// *  Separated from GolfPatches.cpp (SGC1 Pro Tour) to avoid
// *  cross-contamination of hardcoded addresses between versions.
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
#include "core\kernel\init\CxbxKrnl.h"
#include "devices\chihiro\JvsIo.h"
#include <cstdio>
#include <Windows.h>
#include <intrin.h>

#if defined(_DEBUG)
#define G2006_LOG(fmt, ...) do { \
	if (!CxbxrKrnlDebugLoggingEnabled()) break; \
	FILE* _f = fopen("C:\\temp\\golf2006_patches.log", "a"); \
	if (_f) { fprintf(_f, fmt "\n", ##__VA_ARGS__); fclose(_f); } \
	printf(fmt "\n", ##__VA_ARGS__); \
} while(0)
#else
#define G2006_LOG(...) do {} while(0)
#endif

// ── SGC2006 Address Constants ────────────────────────────────────

// Init state machine
static constexpr uintptr_t kStateVar       = 0x00DA6B18;
static constexpr uintptr_t kSub1           = 0x00DA6B28; // touch panel
static constexpr uintptr_t kSub2           = 0x00DA6B2C; // card system
static constexpr uintptr_t kSub3           = 0x00DA6B30; // network
static constexpr uintptr_t kSub4           = 0x00DA6B34; // database
static constexpr uintptr_t kFrameCounter   = 0x00DA6B48;
static constexpr uintptr_t kGmProcState    = 0x00DA6B54;
static constexpr uintptr_t kGmDispState    = 0x00DA6B58;
static constexpr uintptr_t kGmTimer        = 0x00DA6B5C;
static constexpr uintptr_t kGmFrameCount   = 0x00DA6B60;
static constexpr uintptr_t kNetDone        = 0x00DA6B64;
static constexpr uintptr_t kStartupDone    = 0x00DA6B70;

// DIMM / baseboard
static constexpr uintptr_t kDimmInitDone   = 0x00EE6988;
static constexpr uintptr_t kDimmState      = 0x00EE6998;

// Communication
static constexpr uintptr_t kCommError      = 0x0031D340;

// State machine functions (from analysis notes)
static constexpr uintptr_t kUpdateFunc     = 0x001018B0;
static constexpr uintptr_t kDisplayFunc    = 0x00101BA0;

// Mode system (SGC2006 uses DIFFERENT addresses from SGC1!)
// The "inner" task/mode system uses 0x7701E8-0x7702BC.
// Key: [0x7701F4] = current active mode entry pointer (set by activation at 0xAB6E0)
//      [0x7701F8] = pending mode entry pointer (set by 0xAB800, consumed by activation)
//      0xAB5D0 = transition handler function (PUSH ESI; PUSH EDI prologue)
static constexpr uintptr_t kCurModeEntry   = 0x007701F4; // active mode entry ptr (THE key variable)
static constexpr uintptr_t kPendModeEntry  = 0x007701F8; // pending mode entry ptr (set by sub-init)
static constexpr uintptr_t kModePtr        = 0x007702F0; // outer mode system pointer
static constexpr uintptr_t kNextMode       = 0x007702E0; // outer mode flags/state
static constexpr uintptr_t kFlag81         = 0x007702B0; // set when update func returns 1
static constexpr uintptr_t kGate88         = 0x007702B8; // gate byte checked before flag81
static constexpr uintptr_t kTransHandler   = 0x000AB5D0; // transition handler (called when flag81 set)

// Mode table
static constexpr uintptr_t kModeTableBase  = 0x002CE758; // mode entry table (40 bytes per entry, 16 entries)
static constexpr uint32_t  kModeEntrySize  = 40;
static constexpr int       kModeCount      = 16;
// Mode IDs: 0=DUMMY 1=SYSTEM_STARTUP 2=WARNING 3=TOP_LOGO 4=TITLE
//   5=RATING 6=SRVR_MOVIE 7=RANKING 8=SEGA_LOGO 9=TITLE2
//   10=ADVERTISE 11=ENTRY 12=GAME 13=TEST_MODE 14=DEBUG 15=RECOVER_CARD

// Init gate flags — attract mode updates check these before proceeding
static constexpr uintptr_t kInitGate1      = 0x006E603C; // returned by 0x85E90; must be != 0
static constexpr uintptr_t kInitGate2      = 0x007FB8CC; // returned by 0xD4430; must be != 0
static constexpr uintptr_t kInitGate3Inv   = 0x005CFF54; // returned by 0x4DF00; must be == 0

// Coin / credit system (candidate addresses from pattern scan)
static constexpr uintptr_t kCreditCounter  = 0x00559CF0; // DEC'd at 0x42A60, returned by 0x42A80
static constexpr uintptr_t kCoinModeFlag   = 0x00559C24; // returned by 0x42C20 (like SGC1's 677988)

// JVS node functions — in XPP section (not .text), found by pattern match
// Same code structure as SGC1 (0x22E700/0x22E7E0 in XPP section 9)
static constexpr uintptr_t kJvsNodeRecvVA  = 0x00264B60; // XPP: raw 0x2559E0
static constexpr uintptr_t kJvsNodeSendVA  = 0x00264C40; // XPP: raw 0x255AC0

// JVS Input Buffer — game's internal JVS struct populated by init at 0xBBE40.
// The reader at 0xBBD11 checks [kJvsStructBase] != 0, then reads analogs.
// Since the FPGA periodic JVS exchange isn't emulated, we write directly here.
static constexpr uintptr_t kJvsStructBase  = 0x007B29F8; // non-zero = initialized
static constexpr uintptr_t kJvsInitFlag    = 0x007B29FC; // set to 1 by init
static constexpr uintptr_t kJvsAnalog0     = 0x007B2B2C; // 32-bit analog ch 0
static constexpr uintptr_t kJvsAnalog1     = 0x007B2B30; // 32-bit analog ch 1
static constexpr uintptr_t kJvsAnalog2     = 0x007B2B34; // 32-bit analog ch 2
static constexpr uintptr_t kJvsAnalog3     = 0x007B2B38; // 32-bit analog ch 3
static constexpr uintptr_t kJvsAnalog4     = 0x007B2B3C; // 32-bit analog ch 4
static constexpr uintptr_t kJvsAnalog5     = 0x007B2B40; // 32-bit analog ch 5
static constexpr uintptr_t kJvsRetStatus   = 0x007B2B44; // return status
static constexpr uintptr_t kJvsSystemByte  = 0x007B2B48; // system data byte
static constexpr uintptr_t kJvsSwitchByte0 = 0x007B2C6C; // system switch
static constexpr uintptr_t kJvsSwitchByte1 = 0x007B2C7D; // P1 switch byte 0
static constexpr uintptr_t kJvsSwitchByte2 = 0x007B2C8E; // P1 switch byte 1
static constexpr uintptr_t kJvsSwitchByte3 = 0x007B2C9F; // P2 switch byte 0

// Force-mode mechanism: guardian sets this, transition handler consumes it
static volatile int g_forceModeId = -1;

// ── XBE Hash Constants ──────────────────────────────────────────

static const uint64_t kSGC2006Hashes[] = {
	0xCC59629898491B64ULL, // golf.xbe raw file hash
	0xE5940C325FB8B2FBULL, // golf.xbe runtime hash
};

bool IsGolf2006Xbe(uint64_t xbeHash)
{
	for (auto h : kSGC2006Hashes) {
		if (xbeHash == h) return true;
	}
	return false;
}

// ── Hook Functions ──────────────────────────────────────────────

static int __stdcall G2006_LinkOkHook(uint32_t a1, uint32_t a2, uint32_t a3) {
	return 1;
}

static int __stdcall G2006_MbRecvHook(uint32_t a1, uint32_t a2, uint32_t a3) {
	static int callCount = 0;
	if (++callCount <= 20)
		G2006_LOG("MbRecv: a1=0x%08X a2=0x%08X a3=0x%08X (#%d)", a1, a2, a3, callCount);
	return 0;
}

static int __stdcall G2006_MbSendHook(uint32_t a1, uint32_t a2, uint32_t a3) {
	static int callCount = 0;
	if (++callCount <= 20) {
		G2006_LOG("MbSend: a1=0x%08X a2=0x%08X a3=0x%08X (#%d)", a1, a2, a3, callCount);
		if (a1 > 0x10000 && a1 < 0x10000000) {
			uint8_t* d = (uint8_t*)a1;
			G2006_LOG("  data: %02X %02X %02X %02X %02X %02X %02X %02X",
				d[0],d[1],d[2],d[3],d[4],d[5],d[6],d[7]);
		}
	}
	return 0;
}

static int __stdcall G2006_JvsNodeSendHook(uint8_t* Buffer, uint32_t Length, uint32_t a3) {
	if (!g_pJvsIo || !Buffer || Length < 3) return 0;
	unsigned packetCount = Buffer[1];
	static int sendCount = 0;
	if (++sendCount <= 50) {
		G2006_LOG("JvsNodeSend: len=%u pkts=%u data=%02X %02X %02X %02X %02X", Length, packetCount,
			Buffer[0], Buffer[1], Length > 2 ? Buffer[2] : 0, Length > 3 ? Buffer[3] : 0, Length > 4 ? Buffer[4] : 0);
	}
	uint8_t* packetPtr = &Buffer[2];
	for (unsigned i = 0; i < packetCount; i++) {
		packetPtr++;
		size_t bytes = g_pJvsIo->SendPacket(packetPtr);
		packetPtr += bytes;
	}
	return 0;
}

static int __stdcall G2006_JvsNodeRecvHook(uint8_t* Buffer, uint32_t* Length, uint32_t a3) {
	if (!g_pJvsIo || !Buffer || !Length) return 0;
	uint8_t DeviceId = g_pJvsIo->GetDeviceId();
	uint16_t payloadSize = (uint16_t)g_pJvsIo->ReceivePacket(&Buffer[6]);
	static int recvCount = 0;
	if (payloadSize > 0) {
		Buffer[0] = 0;
		Buffer[1] = 1;
		Buffer[2] = DeviceId;
		Buffer[3] = 0;
		*Length = payloadSize + 6;
		*((uint16_t*)&Buffer[4]) = payloadSize;
		if (++recvCount <= 50) {
			G2006_LOG("JvsNodeRecv: payloadSize=%u devId=%u total=%u data=%02X %02X %02X %02X",
				payloadSize, DeviceId, (unsigned)*Length, Buffer[6], Buffer[7],
				payloadSize > 2 ? Buffer[8] : 0, payloadSize > 3 ? Buffer[9] : 0);
		}
	} else {
		if (++recvCount <= 50) {
			G2006_LOG("JvsNodeRecv: EMPTY (no response)");
		}
	}
	return 0;
}

static int __cdecl G2006_ErrorCheckHook(int arg) {
	return 0;
}

static int __cdecl G2006_GetStatHook(int handle) {
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

// Card reader hook — uses the state address from the pattern scan
static uintptr_t g_cardReaderStateAddr = 0;

static int __cdecl G2006_CardReaderHook() {
	static int callCount = 0;
	if (g_cardReaderStateAddr) {
		volatile int32_t* pState = (volatile int32_t*)(uintptr_t)g_cardReaderStateAddr;
		int state = *pState;
		if (++callCount <= 30 || (callCount % 1000) == 0) {
			G2006_LOG("[CARD-RDR] call #%d state=%d -> reset to 0", callCount, state);
		}
		*pState = 0;
	}
	return 1;
}

// Fake device struct for state 0 handler
static uint8_t s_fakeDeviceStruct2006[0x20] = {};

// ── Discovered Addresses (found by pattern scan) ────────────────
static uintptr_t g_jammaCounterAddr = 0;  // like 6779D4 in SGC1
static uintptr_t g_jammaRegisterAddr = 0; // like 6779A0 in SGC1
static uintptr_t g_testButtonVA = 0;
static uintptr_t g_coinButtonVA = 0;
static uintptr_t g_serviceButtonVA = 0;

// ── Input Support ───────────────────────────────────────────────

// Full shared-memory input reader (same format as SGC1)
struct G2006_InputState {
	uint32_t control = 0;
	uint32_t coinState = 0;
	uint8_t analogBytes[4] = {};
	uint8_t switchBytes[5] = {};
	uint16_t analogValues[4] = {};
};

static uint16_t G2006_Expand8To16(uint8_t value)
{
	return (static_cast<uint16_t>(value) << 8) | value;
}

static bool G2006_ReadSharedInput(G2006_InputState* state) {
	extern void* g_jvs_view_ptr;
	if (!g_jvs_view_ptr || !state) return false;
	uint8_t* shared = static_cast<uint8_t*>(g_jvs_view_ptr);
	state->control = *reinterpret_cast<volatile uint32_t*>(shared + 8);
	state->coinState = *reinterpret_cast<volatile uint32_t*>(shared + 32);
	state->analogBytes[0] = *(shared + 12);
	state->analogBytes[1] = *(shared + 13);
	state->analogBytes[2] = *(shared + 14);
	state->analogBytes[3] = *(shared + 15);

	// Parse control bits into JVS switch bytes (same mapping as SGC1)
	jvs_switch_system_inputs_t systemInputs = {};
	jvs_switch_player_inputs_t playerInputs[2] = {};
	systemInputs.test = (state->control & 0x01) != 0;

	playerInputs[0].start     = (state->control & 0x02) != 0;
	playerInputs[0].service   = (state->control & 0x40) != 0;
	playerInputs[0].up        = (state->control & 0x800) != 0;
	playerInputs[0].down      = (state->control & 0x2000) != 0;
	playerInputs[0].left      = (state->control & 0x400) != 0;
	playerInputs[0].right     = (state->control & 0x1000) != 0;
	playerInputs[0].button[0] = (state->control & 0x04) != 0;
	playerInputs[0].button[1] = (state->control & 0x20) != 0;
	playerInputs[0].button[2] = (state->control & 0x200) != 0;
	playerInputs[0].button[3] = (state->control & 0x80000) != 0;
	playerInputs[0].button[4] = (state->control & 0x100000) != 0;
	playerInputs[0].button[5] = (state->control & 0x200000) != 0;
	playerInputs[0].button[8] = (state->control & 0x400000) != 0;
	playerInputs[0].button[9] = (state->control & 0x8000000) != 0;

	playerInputs[1].start     = (state->control & 0x08) != 0;
	playerInputs[1].service   = (state->control & 0x100) != 0;
	playerInputs[1].up        = (state->control & 0x10000) != 0;
	playerInputs[1].down      = (state->control & 0x40000) != 0;
	playerInputs[1].left      = (state->control & 0x8000) != 0;
	playerInputs[1].right     = (state->control & 0x20000) != 0;
	playerInputs[1].button[0] = (state->control & 0x10) != 0;
	playerInputs[1].button[1] = (state->control & 0x80) != 0;
	playerInputs[1].button[2] = (state->control & 0x4000) != 0;
	playerInputs[1].button[3] = (state->control & 0x800000) != 0;
	playerInputs[1].button[4] = (state->control & 0x1000000) != 0;
	playerInputs[1].button[5] = (state->control & 0x2000000) != 0;
	playerInputs[1].button[6] = (state->control & 0x4000000) != 0;
	playerInputs[1].button[7] = (state->control & 0x20000000) != 0;

	state->switchBytes[0] = systemInputs.GetByte0();
	state->switchBytes[1] = playerInputs[0].GetByte0();
	state->switchBytes[2] = playerInputs[0].GetByte1();
	state->switchBytes[3] = playerInputs[1].GetByte0();
	state->switchBytes[4] = playerInputs[1].GetByte1();

	state->analogValues[0] = G2006_Expand8To16(state->analogBytes[0]);
	state->analogValues[1] = G2006_Expand8To16(state->analogBytes[1]);
	state->analogValues[2] = G2006_Expand8To16(state->analogBytes[2]);
	state->analogValues[3] = G2006_Expand8To16(state->analogBytes[3]);
	return true;
}

// Test button hook — edge-detected F1 / shared memory
static int __cdecl G2006_TestButtonHook() {
	static bool s_testWasDown = false;
	G2006_InputState state = {};
	G2006_ReadSharedInput(&state);
	bool testDown = (state.control & 0x01) != 0 || (GetAsyncKeyState(VK_F1) & 0x8000) != 0;
	int result = 0;
	if (testDown && !s_testWasDown) {
		result = 0x04;
		G2006_LOG("[TEST] pressed");
	}
	s_testWasDown = testDown;
	return result;
}

// Service button hook — edge-detected F2 / shared memory
static int __cdecl G2006_ServiceButtonHook() {
	static bool s_serviceWasDown = false;
	G2006_InputState state = {};
	G2006_ReadSharedInput(&state);
	bool serviceDown = (state.control & 0x40) != 0 || (GetAsyncKeyState(VK_F2) & 0x8000) != 0;
	int result = 0;
	if (serviceDown && !s_serviceWasDown) {
		result = 0x08;
		G2006_LOG("[SERVICE] pressed");
	}
	s_serviceWasDown = serviceDown;
	return result;
}

// Coin button hook — edge-detected '5' key / shared memory
static int __cdecl G2006_CoinButtonHook() {
	static bool s_coinWasDown = false;
	G2006_InputState state = {};
	bool coinDown = (G2006_ReadSharedInput(&state) && state.coinState != 0)
		|| (GetAsyncKeyState('5') & 0x8000) != 0;
	int result = 0;
	if (coinDown && !s_coinWasDown) {
		result = 1;
		G2006_LOG("[COIN] pressed");
	}
	s_coinWasDown = coinDown;
	return result;
}

// ── Direct JVS Buffer Mirror ────────────────────────────────────
// Writes TeknoParrot shared memory input directly into the game's
// JVS struct at 0x7B29F8. The FPGA periodic JVS exchange isn't
// emulated, so this is the only way input reaches the game engine.
// Called every tick (~16ms) from the guardian thread.

static void G2006_MirrorInputToGameMemory()
{
	G2006_InputState state = {};
	if (!G2006_ReadSharedInput(&state)) return;

	// Mark JVS struct as initialized so reader at 0xBBD11 doesn't return -2
	*(volatile uint32_t*)kJvsStructBase = 1;
	*(volatile uint32_t*)kJvsInitFlag   = 1;

	// Write switch bytes (17-byte stride within the struct)
	*(volatile uint8_t*)kJvsSwitchByte0 = state.switchBytes[0]; // system
	*(volatile uint8_t*)kJvsSwitchByte1 = state.switchBytes[1]; // P1 byte 0
	*(volatile uint8_t*)kJvsSwitchByte2 = state.switchBytes[2]; // P1 byte 1
	*(volatile uint8_t*)kJvsSwitchByte3 = state.switchBytes[3]; // P2 byte 0

	// Write analog values (32-bit, zero-extended from 16-bit)
	// SGC golf uses: ch0=steering, ch1=gas(?), ch2-5=extra axes
	*(volatile uint32_t*)kJvsAnalog0 = state.analogValues[0];
	*(volatile uint32_t*)kJvsAnalog1 = state.analogValues[1];
	*(volatile uint32_t*)kJvsAnalog2 = state.analogValues[0]; // mirror ch0
	*(volatile uint32_t*)kJvsAnalog3 = state.analogValues[2];
	*(volatile uint32_t*)kJvsAnalog4 = state.analogValues[3];
	*(volatile uint32_t*)kJvsAnalog5 = state.analogValues[0]; // mirror ch0

	// Return status = success (reader checks this)
	*(volatile uint32_t*)kJvsRetStatus = 1;

	// System byte: test=bit6, service=bit7 (same as SGC1 raw system byte)
	uint8_t sysByte = 0;
	if (state.control & 0x01) sysByte |= 0x40; // test
	if (state.control & 0x40) sysByte |= 0x80; // service
	*(volatile uint8_t*)kJvsSystemByte = sysByte;
}

// ── Transition Handler Hook ─────────────────────────────────────
// Replaces the original transition handler at 0xAB5D0.
// Called by the dispatch when flag81 is set.
// Uses the mode entry's 'next' field to determine the next mode,
// then calls sub-init (0xAB800) to set up the pending mode entry.
// Returns 0 so the dispatch continues to the activation function.

static int __cdecl G2006_TransitionHandlerHook() {
	uint32_t curEntry = *(volatile uint32_t*)kCurModeEntry;
	if (!curEntry) return 0;

	int curModeId = *(volatile int*)(curEntry + 8);
	int nextModeId = *(volatile int*)(curEntry + 0x0C);

	// Check if the guardian forced a specific mode (hotkey or coin-in)
	int forced = g_forceModeId;
	if (forced >= 0 && forced < kModeCount) {
		nextModeId = forced;
		g_forceModeId = -1;
		G2006_LOG("[TRANS] FORCED mode %d (was cur=%d next=%d)", nextModeId, curModeId,
			*(volatile int*)(curEntry + 0x0C));
	} else {
		// Normal transition: use current mode's 'next' field
		// Modes with next=0 loop back to attract start (mode 2)
		if (nextModeId == 0) {
			nextModeId = 2;
		}
		G2006_LOG("[TRANS] curMode=%d nextMode=%d curEntry=0x%X", curModeId, nextModeId, curEntry);
	}

	if (nextModeId > 0 && nextModeId < kModeCount) {
		// Call sub-init with nextMode in EAX (register calling convention)
		uintptr_t subInitAddr = 0x000AB800;
		__asm {
			mov eax, nextModeId
			call subInitAddr
		}
		G2006_LOG("[TRANS] sub-init called with mode %d", nextModeId);
	}

	// Clear flag81 so the dispatch doesn't call us again next frame
	*(volatile uint8_t*)kFlag81 = 0;

	return 0; // dispatch continues to activation
}

// ── Guardian Thread ─────────────────────────────────────────────

static DWORD WINAPI G2006_GuardianThread(LPVOID param) {
	volatile int* pState    = (volatile int*)kStateVar;
	volatile int* pSub1     = (volatile int*)kSub1;
	volatile int* pSub2     = (volatile int*)kSub2;
	volatile int* pSub3     = (volatile int*)kSub3;
	volatile int* pSub4     = (volatile int*)kSub4;
	volatile int* pNetDone  = (volatile int*)kNetDone;
	volatile int* pGmState  = (volatile int*)kGmProcState;
	volatile int* pGmDisp   = (volatile int*)kGmDispState;
	volatile int* pDimmInit = (volatile int*)kDimmInitDone;
	volatile int* pDimmSt   = (volatile int*)kDimmState;

	int lastState = -999;
	int lastMode = -999;
	int tick = 0;

	// Phase 1: Drive state machine to completion
	for (int i = 0; i < 2500; i++) {
		int cur = *pState;
		if (cur != lastState) {
			G2006_LOG("[GUARD] tick=%d state %d->%d", i, lastState, cur);
			lastState = cur;
		}

		// Keep DIMM ready
		*pDimmInit = 1;
		*pDimmSt = 4;

		// Keep network done
		*pNetDone = 1;

		// Keep subsystem vars forced
		*pSub1 = -3;
		*pSub2 = -3;
		*pSub3 = -3;
		*pSub4 = -3;

		// Track game-mode state for diagnostics (but don't force it anymore)
		int gmPVal = *pGmState;
		int gmDVal = *pGmDisp;

		// Force state 3→4 if stuck
		if (cur == 3) {
			*pState = 4;
			G2006_LOG("[GUARD] tick=%d forced state 3->4", i);
		}

		// If state is stuck at same value for too long, try forcing forward
		static int stuckCount = 0;
		if (cur == lastState) {
			stuckCount++;
		} else {
			stuckCount = 0;
		}
		// After 200 ticks (~3.2s) stuck at same state, try advancing
		if (stuckCount > 200 && cur >= 0 && cur < 7) {
			*pState = cur + 1;
			G2006_LOG("[GUARD] tick=%d state stuck at %d, forcing to %d", i, cur, cur + 1);
			stuckCount = 0;
		}

		// Log key addresses every 100 ticks for diagnostics
		if ((i % 100) == 0) {
			uint32_t curEntry = *(volatile uint32_t*)kCurModeEntry;
			uint32_t pendEntry = *(volatile uint32_t*)kPendModeEntry;
			uint32_t modeP = *(volatile uint32_t*)kModePtr;
			int dimmSt = *pDimmSt;
			int dimmInit = *pDimmInit;
			uint8_t flag81 = *(volatile uint8_t*)kFlag81;
			uint8_t gate88 = *(volatile uint8_t*)kGate88;
			uint32_t modeFlags = *(volatile uint32_t*)kNextMode;
			G2006_LOG("[GUARD] DIAG tick=%d state=%d curEntry=0x%X pendEntry=0x%X modeP=0x%X f81=%d gate=%d modeFlags=0x%X subs=[%d,%d,%d,%d] net=%d gmP=%d gmD=%d",
				i, cur, curEntry, pendEntry, modeP, flag81, gate88, modeFlags,
				*pSub1, *pSub2, *pSub3, *pSub4, *pNetDone, *pGmState, *pGmDisp);
		}

		// Check mode transition (use curModeEntry, not the outer modePtr)
		uint32_t curEntry = *(volatile uint32_t*)kCurModeEntry;
		int modeId = -999;
		if (curEntry) {
			// The mode entry at runtime has the mode ID; the exact offset
			// depends on the runtime table structure. Log the raw pointer for now.
			modeId = (int)curEntry; // just use pointer as pseudo-ID
		}
		if (modeId != lastMode) {
			uint32_t pendEntry = *(volatile uint32_t*)kPendModeEntry;
			G2006_LOG("[GUARD] tick=%d curEntry changed: 0x%X->0x%X (pendEntry=0x%X)", i, lastMode, modeId, pendEntry);
			lastMode = modeId;
		}

		// Do NOT force flag81 — the previous stub at wrong address (0xAB5D2)
		// caused stack corruption when the transition handler was called.
		// Now with the fixed stub at 0xAB5D0, flag81 can be set naturally.
		// The mode activation at 0xAB6E0 should activate the pending mode
		// from [0x7701F8] without needing flag81 forcing.

		// If curEntry is active (non-null), startup succeeded
		if (curEntry != 0) {
			G2006_LOG("[GUARD] tick=%d mode entry activated (curEntry=0x%X), exiting Phase 1", i, curEntry);
			break;
		}

		Sleep(16);
		tick++;
	}

	G2006_LOG("[GUARD] Phase 1 done, state=%d. Entering permanent guardian.", *pState);

	// Phase 2: Permanent guardian
	for (;;) {
		// Keep subsystem vars forced
		*pSub1 = -3;
		*pSub2 = -3;
		*pSub3 = -3;
		*pSub4 = -3;
		*pNetDone = 1;
		*pDimmInit = 1;
		*pDimmSt = 4;

		// Force init gate flags so attract mode updates can proceed naturally
		*(volatile uint8_t*)kInitGate1 = 1;   // system ready
		*(volatile uint8_t*)kInitGate2 = 1;   // subsystem ready
		*(volatile uint32_t*)kInitGate3Inv = 0; // cleanup done (must be 0)

		// Force coin mode active
		*(volatile uint8_t*)kCoinModeFlag = 1;

		// ── JVS Input Mirror — write TP shared memory to game's JVS buffer ──
		G2006_MirrorInputToGameMemory();

		// Clear gate88
		uint8_t gate = *(volatile uint8_t*)kGate88;
		if (gate != 0) {
			*(volatile uint8_t*)kGate88 = 0;
		}

		// Track mode changes
		uint32_t ce = *(volatile uint32_t*)kCurModeEntry;
		int mid = (int)ce;
		if (mid != lastMode) {
			G2006_LOG("[GUARD] MODE CHANGE: 0x%X -> 0x%X at tick=%d", lastMode, mid, tick);
			lastMode = mid;
		}

		// Mode transition is now handled by the transition handler hook at 0xAB5D0.
		// Log entry details on mode change
		if (ce != 0) {
			static uint32_t lastDumpedEntry = 0;
			if (lastDumpedEntry != ce) {
				lastDumpedEntry = ce;
				uint32_t* raw = (uint32_t*)(uintptr_t)ce;
				G2006_LOG("[GUARD] MODE ENTRY at 0x%X: id=%d next=%d init=0x%X upd=0x%X draw=0x%X exit=0x%X",
					ce, raw[2], raw[3], raw[6], raw[7], raw[8], raw[9]);
			}
		}

		// ── Hotkeys for mode forcing ──
		// F5 = MODE-ENTRY (11), F6 = MODE-GAME (12)
		{
			static bool f5was = false, f6was = false;
			bool f5 = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
			bool f6 = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
			if (f5 && !f5was && ce != 0) {
				g_forceModeId = 11;
				*(volatile uint8_t*)kFlag81 = 1;
				G2006_LOG("[GUARD] F5 -> forcing MODE-ENTRY (11)");
			}
			if (f6 && !f6was && ce != 0) {
				g_forceModeId = 12;
				*(volatile uint8_t*)kFlag81 = 1;
				G2006_LOG("[GUARD] F6 -> forcing MODE-GAME (12)");
			}
			f5was = f5;
			f6was = f6;
		}

		// ── Coin input ──
		// '5' key -> increment game credit counter + force to MODE-ENTRY if in attract
		{
			static bool s_coinWasDown = false;
			G2006_InputState coinState = {};
			bool sharedCoin = G2006_ReadSharedInput(&coinState) && coinState.coinState != 0;
			bool coinDown = sharedCoin || (GetAsyncKeyState('5') & 0x8000) != 0;
			if (coinDown && !s_coinWasDown) {
				volatile int32_t* pCredits = (volatile int32_t*)kCreditCounter;
				(*pCredits)++;
				G2006_LOG("[COIN] credits=%d (wrote to 0x%X)", *pCredits, kCreditCounter);
				// If in attract mode (2-10), force to ENTRY
				if (ce != 0) {
					int modeId = *(volatile int*)(ce + 8);
					if (modeId >= 2 && modeId <= 10) {
						g_forceModeId = 11;
						*(volatile uint8_t*)kFlag81 = 1;
						G2006_LOG("[COIN] attract mode %d -> forcing MODE-ENTRY", modeId);
					}
				}
			}
			s_coinWasDown = coinDown;
		}

		// Auto-advance mode 1 (SYSTEM_STARTUP) — this screen shows subsystem
		// errors (TP/Card/DB/Net) that can never clear in emulation. After a
		// brief delay, force transition to mode 2 (WARNING) like SGC1 does.
		if (ce != 0) {
			int modeId = *(volatile int*)(ce + 8);
			if (modeId == 1 && *pState >= 7) {
				static int m1_delay = 0;
				m1_delay++;
				if (m1_delay > 120) { // ~2 seconds
					g_forceModeId = 2;
					*(volatile uint8_t*)kFlag81 = 1;
					G2006_LOG("[GUARD] auto-advancing past SYSTEM_STARTUP -> WARNING");
					m1_delay = 0;
				}
			}
		}

		// Touch panel emulation via mouse
		// SGC1 TP addresses may not be valid in SGC2006 — guard with SEH
		{
			static bool tpSafe = true; // set false on first access violation
			if (tpSafe) {
				HWND hWnd = g_hEmuWindow;
				if (hWnd) {
					POINT pt;
					GetCursorPos(&pt);
					ScreenToClient(hWnd, &pt);
					RECT rc;
					GetClientRect(hWnd, &rc);
					float fx = (rc.right > 0) ? (float)pt.x * 4095.0f / (float)rc.right : 0.0f;
					float fy = (rc.bottom > 0) ? (float)pt.y * 4095.0f / (float)rc.bottom : 0.0f;
					if (fx < 0.0f) fx = 0.0f; if (fx > 4095.0f) fx = 4095.0f;
					if (fy < 0.0f) fy = 0.0f; if (fy > 4095.0f) fy = 4095.0f;

					__try {
						*(volatile float*)0x00718D70 = fx;
						*(volatile float*)0x00718D74 = fy;
						*(volatile float*)0x00718D6C = 1.0f;
						*(volatile float*)0x00718D48 = fx;
						*(volatile float*)0x00718D4C = fy;
						*(volatile float*)0x00718D50 = 1.0f;
						bool mouseDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
						*(volatile uint8_t*)0x00718D64 = mouseDown ? 0x13 : 0x00;
						*(volatile uint32_t*)0x00718E6C = 2;
					} __except(EXCEPTION_EXECUTE_HANDLER) {
						G2006_LOG("[GUARD] TP address access violation — disabling TP writes");
						tpSafe = false;
					}
				}
			}
		}

		// Periodic diagnostics
		if ((tick % 500) == 0) {
			uint8_t g1 = *(volatile uint8_t*)kInitGate1;
			uint8_t g2 = *(volatile uint8_t*)kInitGate2;
			uint32_t g3 = *(volatile uint32_t*)kInitGate3Inv;
			int32_t cred = *(volatile int32_t*)kCreditCounter;
			uint8_t f81 = *(volatile uint8_t*)kFlag81;
			int modeId = ce ? *(volatile int*)(ce + 8) : -1;
			G2006_LOG("[GUARD] tick=%d modeId=%d state=%d f81=%d gates=[%d,%d,%d] credits=%d sub=[%d,%d,%d,%d]",
				tick, modeId, *pState, f81, g1, g2, g3, cred, *pSub1, *pSub2, *pSub3, *pSub4);
		}

		tick++;
		Sleep(16);
	}
	return 0;
}

// ── Button Function Scanner ─────────────────────────────────────
// Scans for the JVS button read functions by looking for:
//   MOV EAX, [counter]; CMP EAX, 0Ah; JL ...; MOV EAX, [register]
// Returns up to maxHits function addresses. Also extracts the counter
// and register addresses from the pattern.

static std::vector<uintptr_t> ScanForButtonFunctions(uint32_t imageSize)
{
	std::vector<uintptr_t> results;
	uintptr_t base = 0x10000;
	uintptr_t end = base + imageSize;

	for (uintptr_t addr = base; addr < end - 20; addr++) {
		uint8_t* p = (uint8_t*)addr;
		// Pattern: A1 xx xx xx xx 83 F8 0A 7C xx A1 xx xx xx xx
		if (p[0] == 0xA1 && p[5] == 0x83 && p[6] == 0xF8 && p[7] == 0x0A
			&& p[8] == 0x7C && p[10] == 0xA1) {
			// Walk back to find function start (look for preceding RET/INT3)
			uintptr_t funcStart = addr;
			for (uintptr_t back = addr - 1; back > addr - 0x20; back--) {
				uint8_t b = *(uint8_t*)back;
				if (b == 0xCC || b == 0xC3 || b == 0xC2) {
					funcStart = back + 1;
					break;
				}
			}

			// Extract addresses
			uint32_t counterAddr = *(uint32_t*)(p + 1);
			uint32_t registerAddr = *(uint32_t*)(p + 11);

			// Save first match's counter and register addresses
			if (results.empty()) {
				g_jammaCounterAddr = counterAddr;
				g_jammaRegisterAddr = registerAddr;
			}

			G2006_LOG("[SCAN] Button func at 0x%08X (pattern at 0x%08X) counter=0x%08X reg=0x%08X",
				(unsigned)funcStart, (unsigned)addr, counterAddr, registerAddr);
			results.push_back(funcStart);

			// Skip past this match
			addr += 14;
		}
	}
	return results;
}

// ── Board Type Scanner ──────────────────────────────────────────
// Scans for CMP [board_type], 2; JE pattern to find board_type address
// and satellite skip instructions.

struct BoardTypeInfo {
	uintptr_t boardTypeAddr = 0;
	std::vector<uintptr_t> skipJeAddrs;
};

static BoardTypeInfo ScanForBoardType(uint32_t imageSize)
{
	BoardTypeInfo info;
	uintptr_t base = 0x10000;
	uintptr_t end = base + imageSize;

	for (uintptr_t addr = base; addr < end - 10; addr++) {
		uint8_t* p = (uint8_t*)addr;
		// Pattern: 83 3D xx xx xx xx 02 74 xx (CMP [addr], 2; JE short)
		if (p[0] == 0x83 && p[1] == 0x3D && p[6] == 0x02 && p[7] == 0x74) {
			uint32_t btAddr = *(uint32_t*)(p + 2);
			// Validate: board_type should be in .data section (> 0x100000)
			if (btAddr > 0x100000 && btAddr < 0x10000000) {
				if (info.boardTypeAddr == 0) {
					info.boardTypeAddr = btAddr;
					G2006_LOG("[SCAN] Board type addr = 0x%08X (first CMP at 0x%08X)", btAddr, (unsigned)addr);
				}
				if (btAddr == info.boardTypeAddr) {
					info.skipJeAddrs.push_back(addr + 7); // address of the JE instruction
					G2006_LOG("[SCAN] Satellite skip JE at 0x%08X", (unsigned)(addr + 7));
				}
			}
		}
	}
	return info;
}

// ── Helper: write a 4-byte value at a guest VA ───────────────────

static void PatchDword(uintptr_t va, uint32_t value)
{
	PatchXbeBytes(va, reinterpret_cast<const uint8_t*>(&value), sizeof(value));
}

// ── Main Apply Function ─────────────────────────────────────────

void ApplyGolf2006Patches(uint64_t xbeHash, uint32_t imageSize)
{
	g_jvs_game_type = JvsGameType::SegaGolfClub;
	G2006_LOG("=== SGC 2006 Next Tours patches (imageSize=0x%X) ===", imageSize);

	// Backbuffer override to 800x600
	g_ChihiroBackbufferOverrideW = 800;
	g_ChihiroBackbufferOverrideH = 600;

	// Write marker file
#if defined(_DEBUG)
	if (CxbxrKrnlDebugLoggingEnabled()) {
		FILE* f = fopen("C:\\temp\\golf2006_patches.log", "w");
		if (f) { fprintf(f, "Golf2006: start (imageSize=0x%X)\n", imageSize); fclose(f); }
	}
#endif

	// ══════════════════════════════════════════════════════════════
	// SECTION 1: Pattern-based patches (shared Sega library code)
	// ══════════════════════════════════════════════════════════════

	// --- LinkOK ---
	{
		static const uint8_t kLinkOkPat[] = {
			0x85,0xC0, 0x75,0x08, 0xB8,0xFE,0xFF,0xFF,0xFF, 0xC2,0x0C,0x00
		};
		auto hits = ScanXbeAll(kLinkOkPat, sizeof(kLinkOkPat), imageSize);
		for (auto va : hits) {
			if (va >= 5 && ((const uint8_t*)(va - 5))[0] == 0xA1) {
				PatchWithJmp(va - 5, (const void*)&G2006_LinkOkHook);
				G2006_LOG("LinkOK patched at 0x%08X", (unsigned)(va - 5));
			}
		}
	}

	// --- MbRecv/MbSend + JvsNode detection ---
	{
		static const uint8_t kMbFuncPat[] = {
			0x83,0xEC,0x08, 0x8D,0x44,0x24,0x04, 0x50, 0x6A,0x00,
			0xE8,0xFF,0xFF,0xFF,0xFF,
			0x8D,0x0C,0x24, 0x51, 0x6A,0x01, 0xE8
		};
		auto mbHits = ScanXbeAll(kMbFuncPat, sizeof(kMbFuncPat), imageSize);
		if (mbHits.size() >= 2) {
			uintptr_t mbRecvVA = mbHits[0];
			uintptr_t mbSendVA = mbHits[1];
			PatchWithJmp(mbRecvVA, (const void*)&G2006_MbRecvHook);
			PatchWithJmp(mbSendVA, (const void*)&G2006_MbSendHook);
			G2006_LOG("MbRecv/MbSend hooked at 0x%08X / 0x%08X", (unsigned)mbRecvVA, (unsigned)mbSendVA);

			// Scan wider range for JvsNode wrappers
			uintptr_t jvsNodeSendVA = 0, jvsNodeRecvVA = 0;
			uintptr_t scanStart = (mbRecvVA > 0x10000) ? mbRecvVA - 0x10000 : 0x10000;
			uintptr_t scanEnd = mbSendVA + 0x10000;
			if (scanEnd > 0x10000 + imageSize) scanEnd = 0x10000 + imageSize;

			for (uintptr_t addr = scanStart; addr < scanEnd - 5; addr++) {
				uint8_t* p = (uint8_t*)addr;
				if (p[0] == 0xE8) {
					int32_t rel = *(int32_t*)(p + 1);
					uintptr_t target = addr + 5 + rel;
					if (target == mbSendVA && !jvsNodeSendVA) {
						for (uintptr_t back = addr - 1; back > addr - 0x100; back--) {
							uint8_t b = *(uint8_t*)back;
							if ((b == 0x55 && *(uint8_t*)(back+1) == 0x8B && *(uint8_t*)(back+2) == 0xEC)
								|| (b == 0x83 && *(uint8_t*)(back+1) == 0xEC)
								|| b == 0xCC || b == 0xC3 || b == 0xC2) {
								jvsNodeSendVA = (b == 0xCC || b == 0xC3 || b == 0xC2) ? back + 1 : back;
								break;
							}
						}
					}
					if (target == mbRecvVA && !jvsNodeRecvVA) {
						for (uintptr_t back = addr - 1; back > addr - 0x100; back--) {
							uint8_t b = *(uint8_t*)back;
							if ((b == 0x55 && *(uint8_t*)(back+1) == 0x8B && *(uint8_t*)(back+2) == 0xEC)
								|| (b == 0x83 && *(uint8_t*)(back+1) == 0xEC)
								|| b == 0xCC || b == 0xC3 || b == 0xC2) {
								jvsNodeRecvVA = (b == 0xCC || b == 0xC3 || b == 0xC2) ? back + 1 : back;
								break;
							}
						}
					}
				}
			}

			if (jvsNodeSendVA && jvsNodeRecvVA) {
				PatchWithJmp(jvsNodeSendVA, (const void*)&G2006_JvsNodeSendHook);
				PatchWithJmp(jvsNodeRecvVA, (const void*)&G2006_JvsNodeRecvHook);
				G2006_LOG("JvsNode hooked (scanned): Send=0x%08X Recv=0x%08X", (unsigned)jvsNodeSendVA, (unsigned)jvsNodeRecvVA);
			} else {
				// Fallback: JVS node functions are in XPP section, not near MbRecv/MbSend.
				// These may already be HLE-hooked by the emulator (start with E9 JMP).
				// We overwrite them with our own hooks that properly call JvsIo.
				static const uint8_t kJvsRecvSig[] = { 0x53, 0x56, 0x8B, 0x74, 0x24, 0x10, 0x57, 0xC7, 0x06, 0x00, 0x00, 0x00, 0x00 };
				uint8_t* recvPtr = (uint8_t*)kJvsNodeRecvVA;
				uint8_t* sendPtr = (uint8_t*)kJvsNodeSendVA;
				bool recvOk = (memcmp(recvPtr, kJvsRecvSig, sizeof(kJvsRecvSig)) == 0);
				bool sendOk = (sendPtr[0] == 0x53 && sendPtr[1] == 0x56 && sendPtr[2] == 0x57);
				// Also accept if already HLE-hooked (E9 JMP) — we'll overwrite the HLE hook
				bool recvHLE = (recvPtr[0] == 0xE9);
				bool sendHLE = (sendPtr[0] == 0xE9);
				if ((recvOk || recvHLE) && (sendOk || sendHLE)) {
					PatchWithJmp(kJvsNodeRecvVA, (const void*)&G2006_JvsNodeRecvHook);
					PatchWithJmp(kJvsNodeSendVA, (const void*)&G2006_JvsNodeSendHook);
					G2006_LOG("JvsNode hooked (XPP fallback): Recv=0x%08X%s Send=0x%08X%s",
						(unsigned)kJvsNodeRecvVA, recvHLE ? " (was HLE)" : "",
						(unsigned)kJvsNodeSendVA, sendHLE ? " (was HLE)" : "");
				} else {
					G2006_LOG("JvsNode NOT FOUND: recv bytes=%02X %02X %02X send bytes=%02X %02X %02X",
						recvPtr[0], recvPtr[1], recvPtr[2], sendPtr[0], sendPtr[1], sendPtr[2]);
				}
			}
		}
	}

	// --- DIMM-ready bypass ---
	{
		static const uint8_t kDimmReadyPat[] = {
			0x83,0x3D, 0xFF,0xFF,0xFF,0xFF, 0x04,
			0x1B,0xC0, 0x40, 0xC3
		};
		uintptr_t va = ScanXbe(kDimmReadyPat, sizeof(kDimmReadyPat), imageSize);
		if (va) {
			static const uint8_t kAlwaysTrue[] = {
				0xB8,0x01,0x00,0x00,0x00, 0xC3,
				0x90,0x90,0x90,0x90,0x90
			};
			PatchXbeBytes(va, kAlwaysTrue, sizeof(kAlwaysTrue));
			G2006_LOG("DIMM-ready patched at 0x%08X", (unsigned)va);
		}
	}

	// --- D3D_KickOffAndWaitForIdle ---
	{
		static const uint8_t kKickIdlePat[] = {
			0xA1, 0xFF,0xFF,0xFF,0xFF, 0x8B, 0x48, 0x2C
		};
		uintptr_t va = ScanXbe(kKickIdlePat, sizeof(kKickIdlePat), imageSize);
		if (va) {
			static const uint8_t kRet = 0xC3;
			PatchXbeBytes(va, &kRet, 1);
			G2006_LOG("KickOffAndWaitForIdle stubbed at 0x%08X", (unsigned)va);
		}
	}

	// --- CRI ADXF_GetStat ---
	{
		static const uint8_t kGetStatPat[] = {
			0x8B, 0x44, 0x24, 0x04,
			0x85, 0xC0,
			0x75, 0x13,
			0x68
		};
		uintptr_t va = ScanXbe(kGetStatPat, sizeof(kGetStatPat), imageSize);
		if (va) {
			PatchWithJmp(va, (const void*)&G2006_GetStatHook);
			G2006_LOG("CRI GetStat hooked at 0x%08X", (unsigned)va);
		}
	}

	// --- Card reader ---
	{
		static const uint8_t kCardReaderPat[] = {
			0xA1, 0xFF,0xFF,0xFF,0xFF,
			0x83,0xEC,0x08,
			0x48,
			0x83,0xF8,0x05,
			0x0F,0x87
		};
		auto cardHits = ScanXbeAll(kCardReaderPat, sizeof(kCardReaderPat), imageSize);
		for (auto va : cardHits) {
			uint32_t stAddr = *(uint32_t*)(va + 1);
			// Skip if this matched the DIMM state variable — that's not the card reader
			if (stAddr == (uint32_t)kDimmState || stAddr == (uint32_t)kDimmInitDone) {
				G2006_LOG("Card reader pattern at 0x%08X skipped (matched DIMM addr 0x%08X)", (unsigned)va, stAddr);
				continue;
			}
			g_cardReaderStateAddr = stAddr;
			PatchWithJmp(va, (const void*)&G2006_CardReaderHook);
			G2006_LOG("Card reader hooked at 0x%08X (state addr=0x%08X)", (unsigned)va, stAddr);
			break;
		}
	}

	// --- DIMM communication stubs ---
	{
		static const uint8_t kDimmCommPat[] = {
			0x56, 0x6A,0x01, 0x33,0xF6, 0xE8
		};
		auto hits = ScanXbeAll(kDimmCommPat, sizeof(kDimmCommPat), imageSize);
		for (auto va : hits) {
			if (va >= 0x170000 && va <= 0x190000) {
				static const uint8_t kRet0[] = { 0x33,0xC0, 0xC3 };
				PatchXbeBytes(va, kRet0, sizeof(kRet0));
				G2006_LOG("DIMM comm stubbed at 0x%08X", (unsigned)va);
			}
		}
	}

	// --- Baseboard check ---
	{
		static const uint8_t kBaseboardPat[] = {
			0x53, 0x56, 0x57, 0xE8, 0xFF,0xFF,0xFF,0xFF,
			0x8B,0xF8, 0xA1, 0xFF,0xFF,0xFF,0xFF,
			0x83,0xCB,0xFF, 0x85,0xC0
		};
		uintptr_t va = ScanXbe(kBaseboardPat, sizeof(kBaseboardPat), imageSize);
		if (va) {
			static const uint8_t kRet0[] = { 0x33,0xC0, 0xC3 };
			PatchXbeBytes(va, kRet0, sizeof(kRet0));
			G2006_LOG("Baseboard check stubbed at 0x%08X", (unsigned)va);
		}
	}

	// ══════════════════════════════════════════════════════════════
	// SECTION 2: Init state machine bypass (pattern-scanned)
	// ══════════════════════════════════════════════════════════════

	{
		static const uint8_t kInitCheckPat[] = {
			0x39, 0x1D, 0xFF, 0xFF, 0xFF, 0xFF,
			0xBF, 0x04, 0x00, 0x00, 0x00,
			0x7D, 0x1E,
			0x39, 0x1D, 0xFF, 0xFF, 0xFF, 0xFF,
			0x7D, 0x16,
			0x39, 0x1D, 0xFF, 0xFF, 0xFF, 0xFF,
			0x7D, 0x0E,
			0x39, 0x1D, 0xFF, 0xFF, 0xFF, 0xFF,
			0x7D, 0x06,
			0x89, 0x3D, 0xFF, 0xFF, 0xFF, 0xFF
		};
		uintptr_t va = ScanXbe(kInitCheckPat, sizeof(kInitCheckPat), imageSize);
		if (va) {
			uint32_t stateAddr = *(uint32_t*)(va + 39);
			G2006_LOG("Init state var at 0x%08X", stateAddr);

			// Error check stub (sub_F3910)
			{
				static const uint8_t kErrCheckPat[] = {
					0x8B, 0x44, 0x24, 0x04,
					0x8D, 0x04, 0xC0,
					0x8B, 0x0C, 0x85,
					0xFF, 0xFF, 0xFF, 0xFF,
					0x85, 0xC9,
					0x0F, 0x94, 0xC0,
					0xC3
				};
				uintptr_t errVA = ScanXbe(kErrCheckPat, sizeof(kErrCheckPat), imageSize);
				if (errVA) {
					static const uint8_t kRet0[] = { 0x33, 0xC0, 0xC3 };
					PatchXbeBytes(errVA, kRet0, sizeof(kRet0));
					G2006_LOG("Error check stubbed at 0x%08X", (unsigned)errVA);
				}
			}

			// Version check stub (sub_F3930)
			{
				static const uint8_t kVerCheckPat[] = {
					0x8B, 0x44, 0x24, 0x04,
					0x8D, 0x04, 0xC0,
					0x83, 0x3C, 0x85,
					0xFF, 0xFF, 0xFF, 0xFF,
					0x02,
					0x0F, 0x94, 0xC0,
					0xC3
				};
				uintptr_t verVA = ScanXbe(kVerCheckPat, sizeof(kVerCheckPat), imageSize);
				if (verVA) {
					static const uint8_t kRet1[] = { 0xB0, 0x01, 0xC3 };
					PatchXbeBytes(verVA, kRet1, sizeof(kRet1));
					G2006_LOG("Version check stubbed at 0x%08X", (unsigned)verVA);
				}
			}

			// Subsystem 4 monitor stub
			{
				static const uint8_t kSubsys4Pat[] = {
					0xA1, 0x34, 0x6B, 0xDA, 0x00,
					0x8D, 0x48, 0x07,
					0x83, 0xF9, 0x09,
					0x0F, 0x87
				};
				uintptr_t sub4VA = ScanXbe(kSubsys4Pat, sizeof(kSubsys4Pat), imageSize);
				if (sub4VA) {
					static const uint8_t kRet = 0xC3;
					PatchXbeBytes(sub4VA, &kRet, 1);
					G2006_LOG("Subsystem 4 monitor stubbed at 0x%08X", (unsigned)sub4VA);
				}
			}

			// IsTestMode → always return 0
			{
				static const uint8_t kIsTestPat[] = {
					0x8B, 0x0D, 0x24, 0xD3, 0x31, 0x00,
					0x8A, 0x51, 0x04,
					0x32, 0xC0,
					0x84, 0xD2,
					0x74, 0x02,
					0xB0, 0x01,
					0xC3
				};
				uintptr_t testVA = ScanXbe(kIsTestPat, sizeof(kIsTestPat), imageSize);
				if (testVA) {
					static const uint8_t kRet0[] = { 0x32, 0xC0, 0xC3 };
					PatchXbeBytes(testVA, kRet0, sizeof(kRet0));
					G2006_LOG("IsTestMode stubbed at 0x%08X", (unsigned)testVA);
				}
			}

			// Device wait bypass
			{
				static const uint8_t kDevWaitPat[] = {
					0x53,
					0xE8, 0xFF, 0xFF, 0xFF, 0xFF,
					0x83, 0xC4, 0x04,
					0x3B, 0xC3,
					0x0F, 0x84
				};
				uintptr_t devVA = ScanXbe(kDevWaitPat, sizeof(kDevWaitPat), imageSize);
				if (devVA) {
					memset(s_fakeDeviceStruct2006, 0, sizeof(s_fakeDeviceStruct2006));
					s_fakeDeviceStruct2006[0x14] = 1;
					s_fakeDeviceStruct2006[0x15] = 1;
					uint8_t movEax[9] = { 0xB8, 0,0,0,0, 0x90,0x90,0x90,0x90 };
					uint32_t fakeAddr = (uint32_t)(uintptr_t)s_fakeDeviceStruct2006;
					memcpy(movEax + 1, &fakeAddr, 4);
					PatchXbeBytes(devVA, movEax, sizeof(movEax));
					G2006_LOG("Device wait bypassed at 0x%08X", (unsigned)devVA);
				}
			}
		}

		// Subsystem update function stubs
		{
			static const uint8_t kUpd1Pat[] = {
				0xA1, 0x28, 0x6B, 0xDA, 0x00,
				0x56,
				0x83, 0xC0, 0x03,
				0x33, 0xF6,
				0x83, 0xF8, 0x03,
				0x77
			};
			uintptr_t va = ScanXbe(kUpd1Pat, sizeof(kUpd1Pat), imageSize);
			if (va) {
				static const uint8_t kPatch[] = {
					0xC7, 0x05, 0x28, 0x6B, 0xDA, 0x00, 0xFD, 0xFF, 0xFF, 0xFF,
					0x33, 0xC0, 0xC3
				};
				PatchXbeBytes(va, kPatch, sizeof(kPatch));
				G2006_LOG("Subsys update 1 (DA6B28) patched at 0x%08X", (unsigned)va);
			}
		}
		{
			static const uint8_t kUpd2Pat[] = {
				0x83, 0xEC, 0x10,
				0x8D, 0x44, 0x24, 0x04,
				0x50,
				0x8D, 0x4C, 0x24, 0x0C,
				0x51
			};
			uintptr_t va = ScanXbe(kUpd2Pat, sizeof(kUpd2Pat), imageSize);
			if (va) {
				static const uint8_t kPatch[] = {
					0xC7, 0x05, 0x2C, 0x6B, 0xDA, 0x00, 0xFD, 0xFF, 0xFF, 0xFF,
					0x33, 0xC0, 0xC3
				};
				PatchXbeBytes(va, kPatch, sizeof(kPatch));
				G2006_LOG("Subsys update 2 (DA6B2C) patched at 0x%08X", (unsigned)va);
			}
		}
		{
			static const uint8_t kUpd3Pat[] = {
				0xE8, 0xFF, 0xFF, 0xFF, 0xFF,
				0x84, 0xC0,
				0x0F, 0x85, 0xC9, 0x01, 0x00, 0x00,
				0xA1, 0x30, 0x6B, 0xDA, 0x00
			};
			uintptr_t va = ScanXbe(kUpd3Pat, sizeof(kUpd3Pat), imageSize);
			if (va) {
				static const uint8_t kPatch[] = {
					0xC7, 0x05, 0x30, 0x6B, 0xDA, 0x00, 0xFD, 0xFF, 0xFF, 0xFF,
					0x33, 0xC0, 0xC3
				};
				PatchXbeBytes(va, kPatch, sizeof(kPatch));
				G2006_LOG("Subsys update 3 (DA6B30) patched at 0x%08X", (unsigned)va);
			}
		}
		{
			static const uint8_t kUpd4Pat[] = {
				0xE8, 0xFF, 0xFF, 0xFF, 0xFF,
				0x84, 0xC0,
				0x75, 0x18,
				0xA1, 0x30, 0x6B, 0xDA, 0x00
			};
			uintptr_t va = ScanXbe(kUpd4Pat, sizeof(kUpd4Pat), imageSize);
			if (va) {
				static const uint8_t kRet = 0xC3;
				PatchXbeBytes(va, &kRet, 1);
				G2006_LOG("Subsys update 4 (DA6B30 supp) stubbed at 0x%08X", (unsigned)va);
			}
		}
	}

	// ══════════════════════════════════════════════════════════════
	// SECTION 3: SGC2006 hardcoded patches
	// ══════════════════════════════════════════════════════════════

	// DIMM state
	*(volatile int*)kDimmState = 4;
	*(volatile int*)kDimmInitDone = 1;

	// Subsystem status → done
	*(volatile int*)kSub1 = -3;
	*(volatile int*)kSub2 = -3;
	*(volatile int*)kSub3 = -3;
	*(volatile int*)kSub4 = -3;

	// Network done
	*(volatile int*)kNetDone = 1;

	// Game-mode state machine — don't force initially, let the update function drive it
	// Only force these if the display state gets stuck showing overlay
	*(volatile int*)kGmTimer = 0;
	*(volatile int*)kGmFrameCount = 0;
	*(volatile int*)kStartupDone = 1;

	// Disable comm error
	PatchDword(kCommError, 0x00000000);

	// ══════════════════════════════════════════════════════════════
	// SECTION 4: State machine functions — left UNSTUBBED
	// The update function (kUpdateFunc) drives the state machine.
	// The display function (kDisplayFunc) is the draw callback for
	// mode 1 (init) — it calls the update func and handles the
	// return value to trigger mode transition via flag81.
	// Both MUST run. We already nulled "SYSTEM STARTUP" strings
	// so the overlay text is invisible even if the draw runs.
	// ══════════════════════════════════════════════════════════════

	G2006_LOG("Update func at 0x%08X left UNSTUBBED (state machine driver)", (unsigned)kUpdateFunc);
	G2006_LOG("Display func at 0x%08X left UNSTUBBED (triggers mode transition)", (unsigned)kDisplayFunc);

	// Hook mode transition handler to our C++ function.
	// The original at 0xAB5D0 starts with PUSH ESI; PUSH EDI; CALL 0xAB540...
	// We replace with a JMP to our hook which calls 0xAB540, 0xAB800,
	// clears flag81, and returns 0.
	{
		PatchWithJmp(kTransHandler, (const void*)&G2006_TransitionHandlerHook);
		G2006_LOG("Transition handler HOOKED at 0x%08X -> G2006_TransitionHandlerHook", (unsigned)kTransHandler);
	}

	// ══════════════════════════════════════════════════════════════
	// SECTION 5: Board type + satellite bypass
	// ══════════════════════════════════════════════════════════════

	{
		BoardTypeInfo btInfo = ScanForBoardType(imageSize);
		if (btInfo.boardTypeAddr) {
			*(volatile uint32_t*)(uintptr_t)btInfo.boardTypeAddr = 0;
			G2006_LOG("Board type forced to 0 (main board) at 0x%08X", (unsigned)btInfo.boardTypeAddr);
		}
		for (auto jeAddr : btInfo.skipJeAddrs) {
			PatchNop(jeAddr, 2);
			G2006_LOG("Satellite skip JE NOP'd at 0x%08X", (unsigned)jeAddr);
		}
	}

	// ══════════════════════════════════════════════════════════════
	// SECTION 6: Input button hooks (dynamically scanned)
	// ══════════════════════════════════════════════════════════════

	{
		auto buttonFuncs = ScanForButtonFunctions(imageSize);
		if (buttonFuncs.size() >= 3) {
			// The JVS library has test, coin, service in order
			g_testButtonVA = buttonFuncs[0];
			g_coinButtonVA = buttonFuncs[1];
			g_serviceButtonVA = buttonFuncs[2];

			PatchWithJmp(g_testButtonVA, (const void*)&G2006_TestButtonHook);
			G2006_LOG("Test button hooked at 0x%08X", (unsigned)g_testButtonVA);

			PatchWithJmp(g_coinButtonVA, (const void*)&G2006_CoinButtonHook);
			G2006_LOG("Coin button hooked at 0x%08X", (unsigned)g_coinButtonVA);

			PatchWithJmp(g_serviceButtonVA, (const void*)&G2006_ServiceButtonHook);
			G2006_LOG("Service button hooked at 0x%08X", (unsigned)g_serviceButtonVA);

			// Seed JAMMA counter past the gate threshold
			if (g_jammaCounterAddr) {
				*(volatile int32_t*)(uintptr_t)g_jammaCounterAddr = 100;
				G2006_LOG("JAMMA counter seeded to 100 at 0x%08X", (unsigned)g_jammaCounterAddr);
			}
		} else {
			G2006_LOG("WARNING: Only found %zu button functions (need 3)", buttonFuncs.size());
		}
	}

	// ══════════════════════════════════════════════════════════════
	// SECTION 7: Error string nullification
	// ══════════════════════════════════════════════════════════════

	{
		auto nullStr = [&](const uint8_t* pat, size_t len, const char* name) {
			uintptr_t va = ScanXbe(pat, len, imageSize);
			if (va) {
				static const uint8_t kNull = 0;
				PatchXbeBytes(va, &kNull, 1);
				G2006_LOG("%s string nulled at 0x%08X", name, (unsigned)va);
			}
		};
		static const uint8_t kErr14[] = { 'E','r','r','o','r',' ','1','4',':' };
		static const uint8_t kErr11[] = { 'E','r','r','o','r',' ','1','1',':' };
		static const uint8_t kErr12[] = { 'E','r','r','o','r',' ','1','2',':' };
		nullStr(kErr14, sizeof(kErr14), "Error 14");
		nullStr(kErr11, sizeof(kErr11), "Error 11");
		nullStr(kErr12, sizeof(kErr12), "Error 12");

		static const uint8_t kSysStartup[] = { 'S','Y','S','T','E','M',' ','S','T','A','R','T','U','P' };
		static const uint8_t kSysInit[] = { 'S','Y','S','T','E','M',' ','I','N','I','T','I','A','L','I','Z','E' };
		auto sysHits = ScanXbeAll(kSysStartup, sizeof(kSysStartup), imageSize, 4);
		for (auto va : sysHits) {
			static const uint8_t kNull = 0;
			PatchXbeBytes(va, &kNull, 1);
			G2006_LOG("SYSTEM STARTUP string nulled at 0x%08X", (unsigned)va);
		}
		auto initHits = ScanXbeAll(kSysInit, sizeof(kSysInit), imageSize, 4);
		for (auto va : initHits) {
			static const uint8_t kNull = 0;
			PatchXbeBytes(va, &kNull, 1);
			G2006_LOG("SYSTEM INITIALIZE string nulled at 0x%08X", (unsigned)va);
		}
	}

	// ══════════════════════════════════════════════════════════════
	// SECTION 8: TP subsystem init
	// ══════════════════════════════════════════════════════════════

	// Try SGC1's TP addresses (shared Sega TP library)
	*(volatile uint32_t*)0x00718E6C = 2; // TP subsystem ready

	// ══════════════════════════════════════════════════════════════
	// SECTION 9: Launch guardian thread
	// ══════════════════════════════════════════════════════════════

	CreateThread(NULL, 0, G2006_GuardianThread, NULL, 0, NULL);
	G2006_LOG("Guardian thread launched");

	G2006_LOG("=== SGC 2006 patches complete ===");
}
