// ******************************************************************
// *  Cxbx Quest of D patches
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

// D3D swap diagnostic — defined in Direct3D9.cpp
#if defined(_DEBUG)
extern volatile uint32_t g_D3DSwapCounter;
#endif
#include <cstdio>
#include <windows.h>
#include <psapi.h>
#include <TlHelp32.h>

#if defined(_DEBUG)
#define QOD_LOG(fmt, ...) do { \
	printf("QuestOfDPatch: " fmt "\n", ##__VA_ARGS__); \
	{ FILE* _f = fopen("C:\\temp\\qod_patches.log","a"); \
	  if(_f){ SYSTEMTIME _st; GetLocalTime(&_st); \
	  fprintf(_f, "[%02d:%02d:%02d.%03d] QoD: " fmt "\n", _st.wHour,_st.wMinute,_st.wSecond,_st.wMilliseconds, ##__VA_ARGS__); fclose(_f); } } \
} while(0)
#else
#define QOD_LOG(...) do {} while(0)
#endif

static void QodAtExit() {
	QOD_LOG("*** atexit called — process is terminating ***");
}

// Helper: initialize an Xbox RTL_CRITICAL_SECTION at the given address
static void InitXboxCS(uint32_t csAddr) {
	uint8_t* cs = (uint8_t*)(uintptr_t)csAddr;
	memset(cs, 0, 0x1C);
	cs[0] = 1;  // SynchronizationEvent
	cs[2] = 4;  // Size
	*(uint32_t*)(cs + 0x08) = csAddr + 0x08;
	*(uint32_t*)(cs + 0x0C) = csAddr + 0x08;
	*(int32_t*)(cs + 0x10) = -1;
}

static LONG WINAPI QodCrashHandler(EXCEPTION_POINTERS* ep)
{
	DWORD code = ep->ExceptionRecord->ExceptionCode;

	// Handle NULL function pointer calls from uninitialized IC card objects.
	// When EIP=0 (call through NULL), recover by simulating a return with EAX=0.
	// The return address at [ESP] tells us who called NULL.
	if (code == 0xC0000005 && ep->ContextRecord->Eip == 0) {
		DWORD* esp = (DWORD*)(uintptr_t)ep->ContextRecord->Esp;
		if (!IsBadReadPtr(esp, 4)) {
			DWORD retAddr = *esp;
			// Only recover for XBE guest code (0x10000-0x400000 range)
			if (retAddr >= 0x10000 && retAddr < 0x400000) {
				QOD_LOG("NULL call recovery: ret=0x%08X ESP=0x%08X → returning 0",
					retAddr, ep->ContextRecord->Esp);
				ep->ContextRecord->Eip = retAddr;          // return to caller
				ep->ContextRecord->Esp += 4;               // pop return address
				ep->ContextRecord->Eax = 0;                // return value = 0
				return EXCEPTION_CONTINUE_EXECUTION;
			}
		}
	}

	// Handle AV crashes from uninitialized Xbox CS:
	// Kill the offending thread silently so the main game thread continues.
	if (code == 0xC0000005 && ep->ExceptionRecord->NumberParameters >= 2) {
		DWORD accessAddr = (DWORD)ep->ExceptionRecord->ExceptionInformation[1];
		if (accessAddr < 0x1000) {
			uint32_t csAddr = ep->ContextRecord->Ecx;
			if (csAddr == 0) csAddr = ep->ContextRecord->Ebx;
			if (csAddr >= 0xD00000 && csAddr < 0xF20000) {
				uint8_t* cs = (uint8_t*)(uintptr_t)csAddr;
				if (cs[0] == 0) {
					QOD_LOG("Killing thread TID=%d on uninit CS 0x%08X at EIP=0x%08X",
						GetCurrentThreadId(), csAddr, ep->ContextRecord->Eip);
					TerminateThread(GetCurrentThread(), 0);
				}
			}
		}
	}

	// Use Win32 API only — CRT may be corrupted
#if defined(_DEBUG)
	HANDLE hFile = CreateFileA("C:\\temp\\qod_crash.log", FILE_APPEND_DATA, FILE_SHARE_READ|FILE_SHARE_WRITE,
		NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile != INVALID_HANDLE_VALUE) {
		char buf[4096]; DWORD written;
		int len = wsprintfA(buf, "Exception 0x%08X at 0x%p\r\n"
			"  EIP=0x%08X ESP=0x%08X EBP=0x%08X\r\n"
			"  EAX=0x%08X EBX=0x%08X ECX=0x%08X EDX=0x%08X\r\n"
			"  ESI=0x%08X EDI=0x%08X\r\n",
			code, ep->ExceptionRecord->ExceptionAddress,
			ep->ContextRecord->Eip, ep->ContextRecord->Esp, ep->ContextRecord->Ebp,
			ep->ContextRecord->Eax, ep->ContextRecord->Ebx,
			ep->ContextRecord->Ecx, ep->ContextRecord->Edx,
			ep->ContextRecord->Esi, ep->ContextRecord->Edi);
		if (code == 0xC0000005 && ep->ExceptionRecord->NumberParameters >= 2) {
			len += wsprintfA(buf + len, "  Access: %s address 0x%08X\r\n",
				ep->ExceptionRecord->ExceptionInformation[0] ? "WRITE" : "READ",
				(DWORD)ep->ExceptionRecord->ExceptionInformation[1]);
		}
		// Log cxbxr-emu.dll base so we can resolve RVAs
		HMODULE hEmu = GetModuleHandleA("cxbxr-emu.dll");
		if (hEmu) {
			len += wsprintfA(buf + len, "  cxbxr-emu.dll base=0x%08X  crash RVA=0x%08X\r\n",
				(DWORD)(uintptr_t)hEmu,
				(DWORD)((uintptr_t)ep->ExceptionRecord->ExceptionAddress - (uintptr_t)hEmu));
		}
		// Raw stack dump from ESP (EBP chain may be invalid)
		len += wsprintfA(buf + len, "  Stack dump from ESP:\r\n");
		DWORD* esp = (DWORD*)ep->ContextRecord->Esp;
		for (int i = 0; i < 32 && !IsBadReadPtr(esp + i, 4); i++) {
			DWORD val = esp[i];
			len += wsprintfA(buf + len, "    ESP+%02X: 0x%08X", i * 4, val);
			// Flag likely guest code addresses (XBE range 0x10000-0x400000)
			if (val >= 0x10000 && val < 0x400000)
				len += wsprintfA(buf + len, " <-- XBE");
			if (hEmu && val >= (DWORD)(uintptr_t)hEmu && val < (DWORD)(uintptr_t)hEmu + 0x800000)
				len += wsprintfA(buf + len, " (emu+0x%X)", val - (DWORD)(uintptr_t)hEmu);
			len += wsprintfA(buf + len, "\r\n");
		}
		// EBP chain walk (Win32 only)
		len += wsprintfA(buf + len, "  EBP chain:\r\n");
		DWORD* ebp = (DWORD*)ep->ContextRecord->Ebp;
		for (int depth = 0; depth < 10 && !IsBadReadPtr(ebp, 8); depth++) {
			DWORD retAddr = ebp[1];
			len += wsprintfA(buf + len, "    frame %d: EBP=0x%08X ret=0x%08X",
				depth, (DWORD)(uintptr_t)ebp, retAddr);
			if (hEmu && retAddr >= (DWORD)(uintptr_t)hEmu && retAddr < (DWORD)(uintptr_t)hEmu + 0x800000) {
				len += wsprintfA(buf + len, " (emu+0x%X)", retAddr - (DWORD)(uintptr_t)hEmu);
			}
			len += wsprintfA(buf + len, "\r\n");
			DWORD* nextEbp = (DWORD*)(uintptr_t)ebp[0];
			if (nextEbp <= ebp) break;
			ebp = nextEbp;
		}
		WriteFile(hFile, buf, len, &written, NULL);
		CloseHandle(hFile);
	}
#endif
	return EXCEPTION_CONTINUE_SEARCH;
}

// ── Hook functions ────────────────────────────────────────────────

// JVS node hooks — forward JVS commands to the emulated I/O board
// (same approach as Golf/SGC1). QoD statically links the Sega JVS SDK
// so the standard HLE patches can't find the symbols.
static int __stdcall QodJvsNodeSendPacketHook(uint8_t* Buffer, uint32_t Length, uint32_t a3) {
	QOD_LOG("JvsNodeSend: buf=%p len=%u a3=%u", Buffer, Length, a3);
	if (!g_pJvsIo || !Buffer || Length < 3) return 0;
	unsigned packetCount = Buffer[1];
	uint8_t* packetPtr = &Buffer[2];
	for (unsigned i = 0; i < packetCount; i++) {
		packetPtr++; // skip separator byte (0x00)
		size_t bytes = g_pJvsIo->SendPacket(packetPtr);
		packetPtr += bytes;
	}
	return 0;
}

static int __stdcall QodJvsNodeRecvPacketHook(uint8_t* Buffer, uint32_t* Length, uint32_t a3) {
	QOD_LOG("JvsNodeRecv: buf=%p len=%p a3=%u", Buffer, Length, a3);
	if (!g_pJvsIo || !Buffer || !Length) return 0;
	uint8_t DeviceId = g_pJvsIo->GetDeviceId();
	uint16_t payloadSize = (uint16_t)g_pJvsIo->ReceivePacket(&Buffer[6]);
	QOD_LOG("JvsNodeRecv: devId=%u payloadSize=%u", DeviceId, payloadSize);
	if (payloadSize > 0) {
		Buffer[0] = 0;
		Buffer[1] = 1;
		Buffer[2] = DeviceId;
		Buffer[3] = 0;
		*Length = payloadSize + 6;
		*((uint16_t*)&Buffer[4]) = payloadSize;
	}
	return 0;
}
static int __stdcall QodLinkOkHook(uint32_t a1, uint32_t a2, uint32_t a3) {
	return 1;
}
static int __cdecl QodGetStatHook(int handle) {
	int stat = -3;
	if (handle) stat = *(char*)(handle + 1);
	if (stat != 3) {
		if (handle) stat = *(char*)(handle + 1);
		Sleep(0);
	}
	return stat;
}

// ── Peripheral Emulation ──────────────────────────────────────────
// Proper emulation of the Chihiro peripheral serial protocol layer.
// Instead of force-skipping the init, we emulate the serial I/O so
// the game's own init code runs naturally through all 7 steps.
// After init, a guard thread keeps statuses at OK and a touch-
// emulation thread injects mouse→touch coordinates.

// Touch Panel update hook (replaces 0xBC0D0)
// Original: __thiscall(void), ECX = subsystem object
// Object layout: +0 vtable, +4 status, +8 counter, +C step
//
// The original 8-step init:
//   Steps 0-1: Board detect (6E660/6E5E0)
//   Step 2: Init sub-obj 1 (8DA230) via BC320 → sets AD95E8=0
//   Step 3: Init sub-obj 2 (8DA248) via BC3E0 → sets AD95E8=3
//   Step 4: Open touch serial port via BC280
//   Step 5: Init sub-obj 3 (8DA254) via BC450 → sets AD95E8=5
//   Step 6: Status check
//   Step 7: Set ADAF08|=0x01, AD95EC=3
//
// After init, 3 sub-objects run runtime updates via their vtables.
// Each checks AD95E8 for its expected state value:
//   Sub-obj 1: state 2 → calls BC2F0 (touch serial read)
//   Sub-obj 2: state 4 → calls BD020 (check data ready via AD93FC)
//   Sub-obj 3: state 6 → calls BD020 (check data ready + AD9508==6)
//
// The runtime touch data path (BC2F0) reads serial bytes from hardware,
// which doesn't exist in emulation. Instead, QodTouchThread writes
// coordinates directly to the game's final touch output addresses:
//   AD9520: calibrated X float (0-4095 range)
//   AD9524: calibrated Y float (0-4095 range, bottom-to-top)
//   AD9530: pen status (bit 0 = pen touching)
//
// We set AD95E8=6 so sub-obj 3 runs and checks BD020 (AD93FC).
// With AD93FC=0, BD020 returns true (idle), so all sub-objects stay OK.
static void __fastcall QodTouchUpdateHook(void* thisPtr, void* /*edx*/) {
	uint32_t* obj = (uint32_t*)thisPtr;
	if (obj[1] >= 3) return; // already done
	int& step = *(int*)((uint8_t*)thisPtr + 0x0C);
	step++;
	if (step >= 3) {
		// Mark all touch sub-objects as OK
		*(volatile uint32_t*)0x8DA234 = 3; // sub-object 0
		*(volatile uint32_t*)0x8DA24C = 3; // sub-object 1
		*(volatile uint32_t*)0x8DA258 = 3; // sub-object 2

		// Set touch-available bit (original step 7 at BC202)
		*(volatile uint8_t*)0xADAF08 |= 0x01;

		// Set touch status to ready (original step 7 at BC209)
		*(volatile uint32_t*)0xAD95EC = 3;

		// Set touch state for sub-obj 3 runtime (BC460 expects state 6)
		// Sub-obj 3 checks: AD95E8==5 → init, AD95E8==6 → runtime
		*(volatile uint32_t*)0xAD95E8 = 6;

		// Set touch panel type byte (sub-obj 3 state 6 checks AD9508==6)
		*(volatile uint8_t*)0xAD9508 = 6;

		// Set data processing idle (BD020 checks AD93FC: 0=idle/done, 1=processing)
		*(volatile uint32_t*)0xAD93FC = 0;

		obj[1] = 3; // status = OK
		QOD_LOG("Touch panel init complete (emulated, step %d) ADAF08=0x%02X",
			step, *(volatile uint8_t*)0xADAF08);
	}
}

// ── IC Card Reader Serial Protocol ────────────────────────────────
// The IC card reader (SANWA CRD-4432 compatible) communicates via
// RS-232 serial.  The game's serial layer uses:
//   B0C60/B0CF0 — serial port open (calls Sega serial API 268F90)
//   B1310       — send command packet (sets ACDB64/68 = 1 = busy)
//   B1390       — check completion (returns [ACDB68])
//   B1900       — get port status (returns [ACDF98])
//   B0D80       — start transaction (sets [this+4]=1)
//   B0D90       — protocol state machine (5 states at [AD9A44])
//
// Our hooks intercept the serial layer so the game's own init and
// runtime code works naturally.  Commands complete immediately.

// Serial port open hook — replaces B0CF0 (mode 1: ADAB00/ADA500 buffers)
// Sets up config, zeros buffers, returns success without real hardware.
static uint8_t __cdecl QodSerialPortOpen1(uint8_t portNum) {
	// Mode-1 open: ACDFB0 config, ADA500 RX, ADAB00 TX
	memset((void*)0xACDFB0, 0, 8);
	*(volatile uint8_t*)0xACDFB0 = portNum;
	*(volatile uint8_t*)0xACDFB1 = 0;
	*(volatile uint8_t*)0xACDFB2 = 1;
	*(volatile uint8_t*)0xACDFB3 = 1;
	*(volatile uint8_t*)0xACDFB4 = 0;
	memset((void*)0xADA500, 0, 0x200); // RX buffer
	memset((void*)0xADAB00, 0, 0x200); // TX buffer
	// Set port ready status
	*(volatile uint32_t*)0xACDF98 = 2; // port ready
	QOD_LOG("IC serial port 1 opened (emulated): port=%d", portNum);
	return 1;
}

// Serial port open hook — replaces B0C60 (mode 0: ACDF8C config)
static uint8_t __cdecl QodSerialPortOpen0(uint8_t portNum) {
	// Mode-0 open: ACDF8C config
	memset((void*)0xACDF8C, 0, 8);
	*(volatile uint8_t*)0xACDF8C = portNum;
	*(volatile uint8_t*)0xACDF8D = 0;
	*(volatile uint8_t*)0xACDF8E = 1;
	*(volatile uint8_t*)0xACDF8F = 1;
	*(volatile uint8_t*)0xACDF90 = 1;
	memset((void*)0xADA500, 0, 0x200); // RX buffer
	memset((void*)0xADAB00, 0, 0x200); // TX buffer
	*(volatile uint32_t*)0xACDF98 = 2; // port ready
	QOD_LOG("IC serial port 0 opened (emulated): port=%d", portNum);
	return 1;
}

// IC protocol state machine hook — replaces B0D90
// The original polls serial hardware through 5 states at [AD9A44].
// We advance through all states immediately (commands complete instantly).
// ECX = this pointer (serial object at 0x8DA214)
static void __fastcall QodIcProtocolCheckHook(void* thisPtr, void* /*edx*/) {
	uint32_t* obj = (uint32_t*)thisPtr;
	if (obj[1] != 1) return; // not in processing state
	// Original goes through 5 states:
	//   0: open GPIO → 1
	//   1: send init cmd, check recv → 2
	//   2: check completion → 3
	//   3: send cmd2, check completion → 4
	//   4: final check → status=3 (complete)
	// We skip directly to complete.
	*(volatile uint32_t*)0xAD9A44 = 5; // past all states
	*(volatile uint32_t*)0xACDB64 = 0; // not busy
	*(volatile uint32_t*)0xACDB68 = 0; // command complete
	*(volatile uint32_t*)0xACDB6C = 0;
	obj[1] = 3; // protocol complete
}

// IC Card Reader update hook — replaces B0AD0 (the 7-step init function)
// Emulates the full initialization sequence that the game performs:
//   Step 0: Board detection + serial link check
//   Step 1: Open serial port (mode 0 + mode 1)
//   Step 2: Start serial transaction, run protocol state machine
//   Step 3: Check ACDF98 == 2 (port ready), reopen if needed
//   Step 4: Run protocol again, check 8DA218 status
//   Step 5: Call B1AB0 (zero IC card buffer at ACDFB8)
//   Step 6: Set ADAF08 |= 2, AD9A48 = 3
// All serial I/O is emulated — no real hardware needed.
static void __fastcall QodIcUpdateHook(void* thisPtr, void* /*edx*/) {
	uint32_t* obj = (uint32_t*)thisPtr;
	if (obj[1] != 1) {
		// Not in init state — check if we need to return success
		if (obj[1] > 1) return; // already done
		return; // status 0 = not started yet
	}
	int& step = *(int*)((uint8_t*)thisPtr + 0x0C);

	switch (step) {
	case 0:
		// Step 0: Board detection — our 6E660/6E5E0 patches handle the
		// board type.  On real hardware with IC sub-board, 6E5E0 returns 1
		// and we go to step 1.  Since we patched 6E5E0 to return 0 (needed
		// for other callers), we simulate the "sub-board present" path here.
		*(volatile uint32_t*)0xAD9A48 = 1; // progress marker
		step = 1;
		QOD_LOG("IC init step 0: board detected, starting serial init");
		break;
	case 1:
		// Step 1: Open serial port (mode 0 then mode 1)
		QodSerialPortOpen0(2); // port 2, mode 0
		QodSerialPortOpen1(2); // port 2, mode 1
		step = 2;
		QOD_LOG("IC init step 1: serial port opened");
		break;
	case 2: {
		// Step 2: Start serial transaction + protocol check
		// B0D80 sets [8DA214+4]=1, B0D90 runs protocol
		uint32_t* serialObj = (uint32_t*)0x8DA214;
		serialObj[1] = 1; // start transaction
		QodIcProtocolCheckHook(serialObj, nullptr);
		step = 3;
		QOD_LOG("IC init step 2: serial transaction 1 complete");
		break;
	}
	case 3:
		// Step 3: Check ACDF98 == 2 (port ready), reopen if needed
		if (*(volatile uint32_t*)0xACDF98 != 2) {
			QodSerialPortOpen0(2);
		}
		// Start second transaction
		{
			uint32_t* serialObj = (uint32_t*)0x8DA214;
			serialObj[1] = 1;
			QodIcProtocolCheckHook(serialObj, nullptr);
		}
		step = 4;
		QOD_LOG("IC init step 3: port verified, serial transaction 2 complete");
		break;
	case 4:
		// Step 4: Check IC sub-object status (8DA218)
		// The original checks [8DA218] != 4. Ensure the sub-object is
		// not in error state. The sub-object status is set by the serial
		// protocol completion — since we emulated it, force OK.
		*(volatile uint32_t*)0x8DA218 = 3; // IC sub-object = OK
		if (*(volatile uint32_t*)0x8DA218 == 4) {
			obj[1] = 4; // error
			QOD_LOG("IC init step 4: IC sub-object error (8DA218=4)");
			break;
		}
		step = 5;
		// Fall through to step 5
		// FALLTHROUGH
	case 5: {
		// Step 5: Call B1AB0 — zero 0xACC DWORDs at ACDFB8
		// (This is the IC card protocol buffer — state 0 = idle/no card)
		memset((void*)0xACDFB8, 0, 0xACC * 4);
		obj[1] = 3; // status = OK (ready for step 6)
		step = 6;
		QOD_LOG("IC init step 5: IC card buffer cleared (B1AB0 emulated)");
		// FALL THROUGH to step 6 — the original code at B0BA8 sets
		// obj[1]=3 then falls through to B0BBE (step 6 check) in the
		// same function call.  We must do the same.
	}
	// FALLTHROUGH
	case 6:
		// Step 6: Set IC-available bit and completion marker
		if (obj[1] == 3) {
			*(volatile uint8_t*)0xADAF08 |= 0x02; // IC-available bit
			*(volatile uint32_t*)0xAD9A48 = 3;     // IC ready
			step = 7;
			QOD_LOG("IC init step 6: ADAF08=0x%02X AD9A48=3 — IC reader ready",
				*(volatile uint8_t*)0xADAF08);
		} else {
			*(volatile uint32_t*)0xAD9A48 = 4; // error
			step = 7;
			QOD_LOG("IC init step 6: status=%d, AD9A48=4 — IC reader error", obj[1]);
		}
		break;
	default:
		// Init complete — nothing to do
		break;
	}
}

// ── Deck Reader Serial Protocol ──────────────────────────────────
// The deck reader (card dispenser) uses a separate serial protocol:
//   AFB70 — send command (sets ACD60C/ACD604 = 1 = busy)
//   AFD20 — check recv (polls [ACD604])
//   AF620 — deck protocol state machine at [AD9EA0]:
//     State 0: Set GPIO bit via BD310
//     State 1: Send init cmd 0x69 via AFB70
//     State 2: Check recv via AFD20 → set status
//
// The deck init (AF530) calls AF620 which drives this state machine.
// We emulate the serial responses so the init completes naturally.

// Deck Reader update hook — replaces AF530
static void __fastcall QodDeckUpdateHook(void* thisPtr, void* /*edx*/) {
	uint32_t* obj = (uint32_t*)thisPtr;
	if (obj[1] != 1) {
		if (obj[1] > 1) return; // already done
		return;
	}
	int& step = *(int*)((uint8_t*)thisPtr + 0x0C);

	switch (step) {
	case 0:
		// Step 0: Board detection (same as IC — 6E660/6E5E0)
		*(volatile uint32_t*)0xAD9EA4 = 1; // progress marker
		step = 1;
		QOD_LOG("Deck init step 0: board detected");
		break;
	case 1: {
		// Step 1: Deck serial init — the original calls B0D80 (start
		// transaction on 8DA1EC object) then AF620 (deck protocol).
		// AF620 state machine:
		//   State 0: BD310(eax=1) — set GPIO
		//   State 1: AFBE0 — send cmd 0x69 on port 3
		//   State 2: AFD20 — check recv, read result from ACD604

		// Emulate: set deck serial object ready
		uint32_t* deckSerial = (uint32_t*)0x8DA1EC;
		deckSerial[1] = 1; // start transaction

		// Emulate GPIO toggle (BD310 → BD260 sets bits in AD95E0)
		// Not critical for emulation — skip

		// Emulate AFBE0: send cmd 0x69 on port 3
		// AFB70 sets ACD60C=1 (busy), ACD604=1 (processing),
		// ACD618=1, ACD620=ecx(port), ACD62C=cmd(0x69)
		*(volatile uint32_t*)0xACD60C = 0; // not busy (instant complete)
		*(volatile uint32_t*)0xACD604 = 0; // recv done, result=0 (OK)
		*(volatile uint32_t*)0xACD618 = 0;
		*(volatile uint32_t*)0xAD9EA0 = 3; // past all protocol states

		// AFD20 checks ACD604: if != 1, it's done. Value 0 → deck OK.
		// AF620 reads result: edx=0 → setne=0 → eax=0+3=3 → status=3
		deckSerial[1] = 3; // deck serial complete

		// Now AF530 checks [8DA1F0] (deck sub-status):
		*(volatile uint32_t*)0x8DA1F0 = 3; // deck sub-object OK

		// AF530 then checks obj[1] >= 2 to advance
		obj[1] = 3; // deck reader ready
		step = 2;
		QOD_LOG("Deck init step 1: serial protocol complete, deck ready");
		// FALL THROUGH to step 2 — same as IC step 5→6: original code
		// sets status=3 and immediately checks it in the same call.
	}
	// FALLTHROUGH
	case 2:
		// Step 2: Finalize — set deck-available bit
		if (obj[1] == 3) {
			*(volatile uint8_t*)0xADAF08 |= 0x04; // deck-available bit
			*(volatile uint32_t*)0xAD9EA4 = 3;     // deck ready
			step = 3;
			QOD_LOG("Deck init step 2: ADAF08=0x%02X AD9EA4=3 — deck reader ready",
				*(volatile uint8_t*)0xADAF08);
		} else {
			*(volatile uint32_t*)0xAD9EA4 = 4; // error
			step = 3;
		}
		break;
	default:
		break;
	}
}

// ── IC Card File I/O ──────────────────────────────────────────────
// Card files are stored as raw binary: 3 tracks × 69 bytes = 207 bytes.
// Same format as YACardEmu (SANWA CRD-4432 compatible).
//
// The IC card serial protocol layer (B1310/B1390/B13A0) communicates
// with the card reader via RS-232.  Commands complete when the serial
// interrupt handler sets ACDB68=0.  Since there's no real hardware,
// serial commands never complete.
//
// Our approach: hook B1AD0 (the card runtime poll) to intercept card
// operations.  When a card operation is requested (ACDFB8 != 0), we
// bypass the multi-step serial state machine and directly load/save
// card data from/to .bin files.
//
// Card runtime state machine (B1AD0):
//   ACDFB8=0: idle
//   ACDFB8=1: read (B1CA0, 7 sub-states at AD08B4)
//   ACDFB8=2: write (B2450, 6 sub-states)
//   ACDFB8=3: eject (B2A30, 5 sub-states)
//
// Result area layout (ACDFD0, 0x1B48 bytes):
//   +0x00: error code (0=success)
//   +0x04: status 1
//   +0x08: status 2
//   +0x0C: status 3
//   +0x18: card data blocks (ACDFE8), 0xD98 bytes each

// Path to card file (alongside the XBE)
static char g_cardFilePath[MAX_PATH] = {0};

static void QodCardPathInit() {
	if (g_cardFilePath[0]) return;
	// Build path from XBE directory
	const char* xbePath = "C:\\arcade\\cxbx\\Quest of D The Battle Kingdom (CDV-10035B)\\card.bin";
	strncpy(g_cardFilePath, xbePath, MAX_PATH - 1);
	QOD_LOG("Card file path: %s", g_cardFilePath);
}

static bool QodLoadCardFile(uint8_t* dest, size_t maxLen) {
	QodCardPathInit();
	FILE* f = fopen(g_cardFilePath, "rb");
	if (!f) {
		QOD_LOG("Card file not found: %s", g_cardFilePath);
		return false;
	}
	fseek(f, 0, SEEK_END);
	long fileSize = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (fileSize <= 0 || (fileSize % 69) != 0) {
		QOD_LOG("Card file invalid size: %ld (must be multiple of 69)", fileSize);
		fclose(f);
		return false;
	}
	size_t toRead = (size_t)fileSize;
	if (toRead > maxLen) toRead = maxLen;
	size_t bytesRead = fread(dest, 1, toRead, f);
	fclose(f);
	QOD_LOG("Card file loaded: %zu bytes from %s", bytesRead, g_cardFilePath);
	return bytesRead > 0;
}

static bool QodSaveCardFile(const uint8_t* src, size_t len) {
	QodCardPathInit();
	FILE* f = fopen(g_cardFilePath, "wb");
	if (!f) {
		QOD_LOG("Failed to open card file for writing: %s", g_cardFilePath);
		return false;
	}
	size_t written = fwrite(src, 1, len, f);
	fclose(f);
	QOD_LOG("Card file saved: %zu bytes to %s", written, g_cardFilePath);
	return written == len;
}

// ─── QoD Reboot Interceptor ────────────────────────────────────
// On real hardware, after reading a blank card the game registers
// it with the server and reboots the satellite. Since we don't
// have a server, the reboot just loops forever. Intercept it:
// save any card data from memory, then suppress the reboot.
static volatile bool g_QodGameFullyBooted = false;
static uintptr_t g_QodAdvanceGateAddr = 0; // Address of sub_BF4A0 (state 12 advance gate)
static uint8_t g_QodAdvanceGateOrig[3] = {}; // Original bytes before patch

// Global flag: set when the game XBE (not SEGABOOT) patches are active.
// MediaBoard.cpp checks this to skip SEGABOOT-specific memory manipulation
// (direct entry writes, channel count patch, boot struct, forced reboot).
bool g_QodGamePatchesActive = false;

// Declared in EmuKrnlHal.cpp
extern bool (*g_pfnQuickRebootInterceptor)();

static bool QodQuickRebootInterceptor() {
	if (!g_QodGameFullyBooted) return false; // allow boot reboots

	QOD_LOG("QuickReboot interceptor: saving card data and suppressing reboot");

	// Save any card data from ACDFE8
	uint8_t cardBuf[207];
	memcpy(cardBuf, (void*)0xACDFE8, sizeof(cardBuf));
	bool hasData = false;
	for (int i = 0; i < 207; i++) { if (cardBuf[i] != 0) { hasData = true; break; } }
	if (hasData) {
		QodCardPathInit();
		QodSaveCardFile(cardBuf, sizeof(cardBuf));
	}

	// Clear card state
	*(volatile uint32_t*)0xACDFB8 = 0;
	*(volatile uint32_t*)0xACDFBC = 0;
	*(volatile uint32_t*)0xACDFD0 = 0;

	return true; // suppress the reboot
}

// Hook for B1AD0 — IC card runtime poll
// Intercepts card read/write/eject operations and handles them
// immediately using file I/O instead of serial protocol.
static void QodIcCardRuntimeHook() {
	// Check for pending serial operation (AD08C4)
	// The original code at B1ADE handles this; we skip it.

	uint32_t cardState = *(volatile uint32_t*)0xACDFB8;
	if (cardState == 0) return; // idle

	switch (cardState) {
	case 1: {
		// ── Card READ ──
		// Load card.bin into card data area at ACDFE8
		QOD_LOG("IC Card READ requested");

		// Clear the result area (ACDFD0, 0x6D2 dwords = 0x1B48 bytes)
		memset((void*)0xACDFD0, 0, 0x1B48);

		// Try to load card file
		uint8_t cardBuf[207]; // 3 tracks × 69 bytes
		memset(cardBuf, 0, sizeof(cardBuf));
		bool loaded = QodLoadCardFile(cardBuf, sizeof(cardBuf));

		if (loaded) {
			// Store card data at ACDFE8 (first card slot, offset 0)
			memcpy((void*)0xACDFE8, cardBuf, sizeof(cardBuf));

			// Set success result
			*(volatile uint32_t*)0xACDFBC = 0; // error code = success
			*(volatile uint32_t*)0xACDFC0 = 0; // status 1
			*(volatile uint32_t*)0xACDFC4 = 0; // status 2
			*(volatile uint32_t*)0xACDFC8 = 0; // status 3

			// Copy results to ACDFD0-DC
			*(volatile uint32_t*)0xACDFD0 = 0; // error
			*(volatile uint32_t*)0xACDFD4 = 0; // status 1
			*(volatile uint32_t*)0xACDFD8 = 0; // status 2
			*(volatile uint32_t*)0xACDFDC = 0; // status 3
			*(volatile uint32_t*)0xACDFE0 = 1; // card count

			QOD_LOG("IC Card READ success: %zu bytes loaded", sizeof(cardBuf));
		} else {
			// No card file — report "no card"
			*(volatile uint32_t*)0xACDFBC = 3; // error = no card
			*(volatile uint32_t*)0xACDFD0 = 3;
			QOD_LOG("IC Card READ: no card file available");
		}

		// Reset card state machine
		*(volatile uint32_t*)0xACDFB8 = 0; // idle
		*(volatile uint32_t*)0xAD08B4 = 0; // clear sub-state
		*(volatile uint32_t*)0xAD08B8 = 0;
		*(volatile uint32_t*)0xAD08BC = 0;
		break;
	}
	case 2: {
		// ── Card WRITE ──
		// Save card data from ACDFE8 to card.bin
		QOD_LOG("IC Card WRITE requested");

		uint8_t cardBuf[207];
		memcpy(cardBuf, (void*)0xACDFE8, sizeof(cardBuf));
		bool saved = QodSaveCardFile(cardBuf, sizeof(cardBuf));

		// Set result
		*(volatile uint32_t*)0xACDFBC = saved ? 0 : 4; // 0=success, 4=error
		*(volatile uint32_t*)0xACDFC0 = 0;
		*(volatile uint32_t*)0xACDFC4 = 0;
		*(volatile uint32_t*)0xACDFC8 = 0;
		*(volatile uint32_t*)0xACDFD0 = *(volatile uint32_t*)0xACDFBC;
		*(volatile uint32_t*)0xACDFD4 = 0;
		*(volatile uint32_t*)0xACDFD8 = 0;
		*(volatile uint32_t*)0xACDFDC = 0;

		QOD_LOG("IC Card WRITE %s", saved ? "success" : "FAILED");

		// Reset
		*(volatile uint32_t*)0xACDFB8 = 0;
		*(volatile uint32_t*)0xAD08B4 = 0;
		*(volatile uint32_t*)0xAD08B8 = 0;
		*(volatile uint32_t*)0xAD08BC = 0;
		break;
	}
	case 3: {
		// ── Card EJECT ──
		QOD_LOG("IC Card EJECT requested");
		*(volatile uint32_t*)0xACDFBC = 0; // success
		*(volatile uint32_t*)0xACDFD0 = 0;

		// Reset
		*(volatile uint32_t*)0xACDFB8 = 0;
		*(volatile uint32_t*)0xAD08B4 = 0;
		*(volatile uint32_t*)0xAD08B8 = 0;
		*(volatile uint32_t*)0xAD08BC = 0;
		QOD_LOG("IC Card EJECT done");
		break;
	}
	default:
		QOD_LOG("IC Card unknown state: %d", cardState);
		*(volatile uint32_t*)0xACDFB8 = 0;
		break;
	}
}

// Peripheral guard thread — emulates hardware subsystem statuses
// that can't be set naturally without Chihiro network/DIMM hardware.
static DWORD WINAPI QodPeripheralGuardThread(LPVOID) {
	Sleep(500);

	// Render calls and BF550 are NOT patched — let the game render
	// naturally from the start. Push buffer issues are handled by
	// the emulator's D3D HLE layer.

	bool mbLogged = false;
	int diagCount = 0;
	uint32_t prevTask80 = 0;
	uint32_t prevCardOp = 0xFFFFFFFF;
	int stateTimer = 0;
	bool dumpedTaskMgr = false;
	bool dumpedThreadEip = false;
	int selectorStuckTimer = 0;

	while (true) {
		// ── Emulated hardware statuses ─────────────────────────
		volatile uint32_t* mbStatus = (volatile uint32_t*)0x8D1990;
		if (*mbStatus >= 1 && *mbStatus < 3) {
			*mbStatus = 3;
			if (!mbLogged) { QOD_LOG("Media Board status → OK"); mbLogged = true; }
		}
		*(volatile uint32_t*)0x8D1984 = 3; // IO board
		*(volatile uint32_t*)0x8D197C = 3; // Network
		*(volatile uint32_t*)0x8D1998 = 3; // Data Load
		*(volatile uint32_t*)0x8D199C = 3; // Network Connect

		// Gate7 — IC reader ready gate
		if ((*(volatile uint8_t*)0xADAF08 & 0x02) && *(volatile uint32_t*)0xAD9A48 == 3) {
			*(volatile uint8_t*)0x8D39FC = 1;
		}

		// State 13 condition flags — network/download subsystem checks
		// can't pass without real network hardware
		*(volatile uint32_t*)0x8D19A4 = 7;

		// Force transition flag clear and render readiness.
		// Transition flag — sub_6E490 checks bit 0 of A935C6.
		// When set, sub-counter stops incrementing.
		*(volatile uint8_t*)0xA935C6 &= ~1u;

		// Rendering ready bit — force bit 1 at *0x6842E0 so the
		// main loop's rendering path executes.
		{
			uint32_t renderPtr = *(volatile uint32_t*)0x6842E0;
			if (renderPtr >= 0x10000 && renderPtr < 0x1000000) {
				*(volatile uint8_t*)renderPtr |= 0x02;
			}
		}

		// ── Network init bypass (states 10-14) ─────────────────
		{
			volatile uint32_t* outerState = (volatile uint32_t*)0x8D1970;
			volatile uint32_t* outerSub   = (volatile uint32_t*)0x8D1974;
			uint32_t curState = *outerState;

			// Runtime CRI patch: once state >= 15, init is done and it's
			// safe to patch CRI functions. Can't do this at boot time
			// because init code uses them legitimately.
			// Patch sub_6D3A0 entry to RET (prevent future CRI wait loops)
			// AND sub_6D320 to return 1 (break existing CRI wait loops —
			// a thread might already be inside sub_6D3A0's loop).
			// Also restore the advance gate (sub_BF4A0) so funcID=4's
			// draw function can reach attract mode paths instead of
			// always jumping to funcID=6.
			{
				static bool criRunPatched = false;
				if (!criRunPatched && curState >= 15) {
					criRunPatched = true;
					DWORD oldProt;
					// Patch sub_6D3A0: replace spin-loop with single-pass
					// CALL sub_6D330; RET — processes one batch of CRI
					// commands per invocation instead of spinning forever.
					if (VirtualProtect((void*)0x6D3A0, 6, PAGE_EXECUTE_READWRITE, &oldProt)) {
						*(uint8_t*)0x6D3A0  = 0xE8; // CALL rel32
						*(uint32_t*)0x6D3A1 = 0x6D330 - (0x6D3A0 + 5); // -> sub_6D330
						*(uint8_t*)0x6D3A5  = 0xC3; // RET
						VirtualProtect((void*)0x6D3A0, 6, oldProt, &oldProt);
						QOD_LOG("Runtime: patched sub_6D3A0 to single-pass CRI process");
					}
					// Patch sub_6D3D0: same treatment (boot-time RET -> single-pass)
					if (VirtualProtect((void*)0x6D3D0, 6, PAGE_EXECUTE_READWRITE, &oldProt)) {
						*(uint8_t*)0x6D3D0  = 0xE8; // CALL rel32
						*(uint32_t*)0x6D3D1 = 0x6D330 - (0x6D3D0 + 5); // -> sub_6D330
						*(uint8_t*)0x6D3D5  = 0xC3; // RET
						VirtualProtect((void*)0x6D3D0, 6, oldProt, &oldProt);
						QOD_LOG("Runtime: patched sub_6D3D0 to single-pass CRI process");
					}
					// Write custom sub_73D70 stub: only calls sub_6D3D0
					// (CRI command processing) + sets cleanup-done flag.
					// Original also called sub_29070(1), sub_29070(2),
					// sub_6A2C0, sub_6CB30 — those corrupt CRI state
					// after repeated attract cycles, triggering an
					// in-game error dialog with garbled text.
					// Custom stub:
					//   73D70: E8 5B 96 FF FF        CALL sub_6D3D0
					//   73D75: C6 05 7A 36 A9 00 01  MOV BYTE [A9367A], 1
					//   73D7C: C3                    RET
					if (VirtualProtect((void*)0x73D70, 13, PAGE_EXECUTE_READWRITE, &oldProt)) {
						*(uint8_t*)0x73D70  = 0xE8; // restore CALL opcode
						// 73D71-73D74 already has correct rel32 to sub_6D3D0
						*(uint8_t*)0x73D75  = 0xC6; // MOV BYTE PTR [imm32], imm8
						*(uint8_t*)0x73D76  = 0x05;
						*(uint32_t*)0x73D77 = 0x00A9367A;
						*(uint8_t*)0x73D7B  = 0x01;
						*(uint8_t*)0x73D7C  = 0xC3; // RET
						VirtualProtect((void*)0x73D70, 13, oldProt, &oldProt);
						QOD_LOG("Runtime: wrote custom sub_73D70 stub (CALL sub_6D3D0 + flag + RET)");
					}
					if (VirtualProtect((void*)0x6D320, 3, PAGE_EXECUTE_READWRITE, &oldProt)) {
						*(uint8_t*)0x6D320 = 0xB0; // MOV AL, 1
						*(uint8_t*)0x6D321 = 0x01;
						*(uint8_t*)0x6D322 = 0xC3; // RET
						VirtualProtect((void*)0x6D320, 3, oldProt, &oldProt);
						QOD_LOG("Runtime: patched sub_6D320 to return 1");
					}
					// Restore advance gate (sub_BF4A0) — no longer needed
					// for boot, and funcID=4's draw function uses it to
					// decide the attract mode path
					if (g_QodAdvanceGateAddr) {
						if (VirtualProtect((void*)g_QodAdvanceGateAddr, 3, PAGE_EXECUTE_READWRITE, &oldProt)) {
							memcpy((void*)g_QodAdvanceGateAddr, g_QodAdvanceGateOrig, 3);
							VirtualProtect((void*)g_QodAdvanceGateAddr, 3, oldProt, &oldProt);
							QOD_LOG("Runtime: restored advance gate at 0x%08X (attract mode enabled)", (unsigned)g_QodAdvanceGateAddr);
						}
					}

				}
			}

			if (curState == 10) {
				volatile uint32_t* innerState = (volatile uint32_t*)0xB29AD0;
				if (*innerState < 10) *innerState = 10;
				*(volatile uint32_t*)0xB29ACC = 0;
				stateTimer++;
				if (stateTimer > 190) {
					QOD_LOG("State 10 timeout → forcing to 11");
					*outerState = 11; *outerSub = 0; stateTimer = 0;
				}
			} else if (curState == 11) {
				*outerState = 12; *outerSub = 0; stateTimer = 0;
			} else if (curState == 12 || curState == 13) {
				stateTimer++;
				if (stateTimer > 190) {
					QOD_LOG("State %d timeout → forcing to 14", curState);
					*outerState = 14; *outerSub = 0; stateTimer = 0;
				}
			} else if (curState == 14 && *outerSub < 3) {
				*outerSub = 3;
			} else if (curState == 14 && !dumpedTaskMgr) {
				dumpedTaskMgr = true;
				// Dump state 14 patch locations to verify they're intact
				QOD_LOG("STATE14 CODE DUMP:");
				for (uint32_t base = 0x73640; base < 0x73780; base += 32) {
					uint8_t buf[32];
					memcpy(buf, (void*)base, 32);
					QOD_LOG("  %05X: %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X "
						"%02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X",
						base,
						buf[0],buf[1],buf[2],buf[3],buf[4],buf[5],buf[6],buf[7],
						buf[8],buf[9],buf[10],buf[11],buf[12],buf[13],buf[14],buf[15],
						buf[16],buf[17],buf[18],buf[19],buf[20],buf[21],buf[22],buf[23],
						buf[24],buf[25],buf[26],buf[27],buf[28],buf[29],buf[30],buf[31]);
				}
			} else {
				stateTimer = 0;
			}
		}

		// ── Task change detector ──────────────────────────────
		{
			uint32_t task80 = *(volatile uint32_t*)0xA93680;

			if (task80 != prevTask80) {
				QOD_LOG("TASK80: 0x%08X → 0x%08X (state=%d sub=%d)",
					prevTask80, task80,
					*(volatile uint32_t*)0x8D1970,
					*(volatile uint32_t*)0x8D1974);
				// Dump the new task entry
				if (task80 >= 0x002FAA08 && task80 <= 0x002FAB30) {
					uint32_t* e = (uint32_t*)(uintptr_t)task80;
					QOD_LOG("  entry: flags=%08X timer=%.2f funcID=%d comp=%d altComp=%d name=%08X cb=%08X draw=%08X cleanup=%08X",
						e[0], *(float*)&e[1], e[2], e[3], e[4], e[5], e[6], e[7], e[8]);
				}
				prevTask80 = task80;
				if (task80 == 0x002FAAE0 && !g_QodGameFullyBooted) {
					g_QodGameFullyBooted = true;
					QOD_LOG("Game fully booted — reboot suppression enabled");
				}
			}

			// One-time dump of all task entries when game reaches state 15
			{
				static bool taskTableDumped = false;
				if (!taskTableDumped && *(volatile uint32_t*)0x8D1970 >= 15) {
					taskTableDumped = true;
					QOD_LOG("=== TASK TABLE DUMP (FAA08-FAB28) ===");
					for (uintptr_t addr = 0x002FAA08; addr <= 0x002FAB28; addr += 0x24) {
						uint32_t* e = (uint32_t*)addr;
						QOD_LOG("  [%08X] flags=%08X timer=%.2f funcID=%d comp=%d altComp=%d cb=%08X draw=%08X cleanup=%08X",
							(unsigned)addr, e[0], *(float*)&e[1], e[2], e[3], e[4], e[6], e[7], e[8]);
					}
				}
			}

			// CRI ADX tasks (a1=3,4,5) now run naturally — their callbacks,
			// draws, and cleanups execute the game's real CRI code.
			// sub_6D3D0 (RET) and sub_73D70 (RET) prevent CRI from
			// blocking during task transitions. sub_6D300 (return 1)
			// ensures the CRI queue gate always passes.
		}

		// ── Card I/O monitoring (after init) ──────────────────
		if (*(volatile uint32_t*)0x8D1970 >= 15) {
			uint32_t cardOp = *(volatile uint32_t*)0xACDFB8;
			if (cardOp != prevCardOp && cardOp != 0) {
				QOD_LOG("Card op: %d → %d", prevCardOp, cardOp);
			}
			if (cardOp == 2) {
				QOD_LOG("IC Card WRITE (guard)");
				uint8_t cardBuf[207];
				memcpy(cardBuf, (void*)0xACDFE8, sizeof(cardBuf));
				QodSaveCardFile(cardBuf, sizeof(cardBuf));
				*(volatile uint32_t*)0xACDFBC = 0;
				*(volatile uint32_t*)0xACDFD0 = 0;
				*(volatile uint32_t*)0xACDFB8 = 0;
				*(volatile uint32_t*)0xAD08B4 = 0;
			} else if (cardOp == 3) {
				QOD_LOG("IC Card EJECT (guard)");
				*(volatile uint32_t*)0xACDFBC = 0;
				*(volatile uint32_t*)0xACDFD0 = 0;
				*(volatile uint32_t*)0xACDFB8 = 0;
				*(volatile uint32_t*)0xAD08B4 = 0;
			}
			if (cardOp != 0) prevCardOp = cardOp;
		}

		// ── F3 card insert hotkey ─────────────────────────────
		{
			static bool f3WasDown = false;
			bool f3Down = (GetAsyncKeyState(VK_F3) & 0x8000) != 0;
			if (f3Down && !f3WasDown) {
				QodCardPathInit();
				FILE* chk = fopen(g_cardFilePath, "rb");
				if (!chk) {
					chk = fopen(g_cardFilePath, "wb");
					if (chk) {
						uint8_t blank[207];
						memset(blank, 0, sizeof(blank));
						fwrite(blank, 1, sizeof(blank), chk);
						fclose(chk);
						QOD_LOG("Created blank card file");
					}
				} else {
					fclose(chk);
				}
				volatile uint8_t* evtBase = *(volatile uint8_t**)0x6842E0;
				if (evtBase) *(volatile uint32_t*)evtBase |= 0x02;
				*(volatile uint32_t*)0xACDFB8 = 1;
				QOD_LOG("IC Card INSERT (F3)");
			}
			f3WasDown = f3Down;
		}

		// ── Periodic diagnostic ───────────────────────────────
		diagCount++;
		if ((diagCount % 125) == 0) {
			uint32_t t80 = *(volatile uint32_t*)0xA93680;
			QOD_LOG("DIAG state=%d sub=%d | MB=%d IO=%d NET=%d DL=%d NC=%d | task80=%08X A93684=%08X e+12=%d timer=%.2f gate7=%d swap=%u",
				*(volatile uint32_t*)0x8D1970, *(volatile uint32_t*)0x8D1974,
				*(volatile uint32_t*)0x8D1990, *(volatile uint32_t*)0x8D1984,
				*(volatile uint32_t*)0x8D197C, *(volatile uint32_t*)0x8D1998,
				*(volatile uint32_t*)0x8D199C,
				t80,
				*(volatile uint32_t*)0xA93684,
				(t80 >= 0x002FAA08 && t80 <= 0x002FAAE0) ? *(volatile int32_t*)(t80 + 12) : -1,
				*(volatile float*)0xA93670,
				*(volatile uint8_t*)0x8D39FC,
				g_D3DSwapCounter);
		}

		// ── Thread EIP dump — find where game thread is blocked ──
		// Fires: once at state >= 14, and again when task > FAAE0
		{
			bool shouldDump = false;
			if (!dumpedThreadEip && *(volatile uint32_t*)0x8D1970 >= 14) {
				selectorStuckTimer++;
				if (selectorStuckTimer > 62) shouldDump = true;
			}
			static bool dumpedAtFAB04 = false;
			uint32_t curTask = *(volatile uint32_t*)0xA93680;
			if (!dumpedAtFAB04 && curTask > 0x002FAAE0 && curTask <= 0x002FAB30) {
				static int fab04Timer = 0;
				fab04Timer++;
				if (fab04Timer > 125) { // ~2 seconds to let it settle
					dumpedAtFAB04 = true;
					shouldDump = true;
					// Dump code at 0x29A80-0x29B20 (push buffer spin loop area)
					QOD_LOG("PUSH BUFFER CODE DUMP (0x29A80-0x29B20):");
					for (uint32_t base = 0x29A80; base < 0x29B20; base += 16) {
						uint8_t buf[16];
						memcpy(buf, (void*)base, 16);
						QOD_LOG("  %05X: %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X",
							base, buf[0],buf[1],buf[2],buf[3],buf[4],buf[5],buf[6],buf[7],
							buf[8],buf[9],buf[10],buf[11],buf[12],buf[13],buf[14],buf[15]);
					}
					// Also dump around 0x56E90 (main thread wait location)
					QOD_LOG("MAIN LOOP CODE DUMP (0x56E80-0x56F00):");
					for (uint32_t base = 0x56E80; base < 0x56F00; base += 16) {
						uint8_t buf[16];
						memcpy(buf, (void*)base, 16);
						QOD_LOG("  %05X: %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X",
							base, buf[0],buf[1],buf[2],buf[3],buf[4],buf[5],buf[6],buf[7],
							buf[8],buf[9],buf[10],buf[11],buf[12],buf[13],buf[14],buf[15]);
					}
				}
			}
			if (shouldDump) {
				if (!dumpedThreadEip) dumpedThreadEip = true;
				DWORD myTid = GetCurrentThreadId();
				DWORD pid = GetCurrentProcessId();
				HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
				if (snap != INVALID_HANDLE_VALUE) {
					THREADENTRY32 te;
					te.dwSize = sizeof(te);
					if (Thread32First(snap, &te)) {
						do {
							if (te.th32OwnerProcessID == pid && te.th32ThreadID != myTid) {
								HANDLE ht = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
								if (ht) {
									SuspendThread(ht);
									CONTEXT ctx = {};
									ctx.ContextFlags = CONTEXT_CONTROL;
									if (GetThreadContext(ht, &ctx)) {
										// Log all threads — check stack for game code refs
										bool hasGameCode = (ctx.Eip >= 0x10000 && ctx.Eip < 0x2000000);
										// Scan stack for any game code return addresses
										uint32_t gameRets[8];
										int gameRetCount = 0;
										uint32_t* stack = (uint32_t*)(uintptr_t)ctx.Esp;
										for (int i = 0; i < 32 && gameRetCount < 8; i++) {
											uint32_t val = 0;
											__try { val = stack[i]; } __except(1) { break; }
											if (val >= 0x10000 && val < 0x2000000) {
												gameRets[gameRetCount++] = val;
											}
										}
										if (hasGameCode || gameRetCount > 0) {
											QOD_LOG("THREAD tid=%d EIP=%08X ESP=%08X EBP=%08X",
												te.th32ThreadID, ctx.Eip, ctx.Esp, ctx.Ebp);
											for (int i = 0; i < gameRetCount; i++) {
												QOD_LOG("  ret[%d]=%08X", i, gameRets[i]);
											}
										}
									}
									ResumeThread(ht);
									CloseHandle(ht);
								}
							}
						} while (Thread32Next(snap, &te));
					}
					CloseHandle(snap);
				}
			}
		}

		Sleep(16);
	}
	return 0;
}

// Touch panel emulation thread – maps mouse cursor to touch coordinates
// Writes directly to the game's final touch output addresses that the
// gameplay code reads:
//   AD9520: calibrated X float (read by 0x9B909 → AC918C)
//   AD9524: calibrated Y float (read by 0x9B920 → AC9190)
//   AD9530: pen status byte (read by 0xA88EB, bit 0 = pen down)
// Also populates the intermediate parsed data at AD94FC+0x10..0x34
// and the pen status at AD9530 for the touch serial parse layer.
static DWORD WINAPI QodTouchThread(LPVOID) {
	Sleep(2000); // wait for D3D window
	QOD_LOG("Touch emulation thread started");
	while (true) {
		HWND hWnd = g_hEmuWindow;
		if (hWnd) {
			POINT pt;
			GetCursorPos(&pt);
			ScreenToClient(hWnd, &pt);
			RECT rc;
			GetClientRect(hWnd, &rc);
			float fx = (rc.right  > 0) ? (float)pt.x * 4095.0f / (float)rc.right  : 0.0f;
			float fy = (rc.bottom > 0) ? (float)pt.y * 4095.0f / (float)rc.bottom : 0.0f;
			if (fx < 0.0f) fx = 0.0f; if (fx > 4095.0f) fx = 4095.0f;
			if (fy < 0.0f) fy = 0.0f; if (fy > 4095.0f) fy = 4095.0f;
			bool mouseDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;

			uint16_t rawX = (uint16_t)fx;
			uint16_t rawY = (uint16_t)fy;
			// BC7F0 inverts Y: invertedY = 0xFFF - rawY
			float invertedY = 4095.0f - fy;

			// ── Game's final touch coordinate outputs ──
			// These are read by the gameplay code at 0x9B909/0x9B920.
			*(volatile float*)0xAD9520 = fx;          // calibrated X
			*(volatile float*)0xAD9524 = invertedY;   // calibrated Y (inverted)
			// Pen status byte: bit 0 = touching (read by 0xA88EB)
			*(volatile uint8_t*)0xAD9530 = mouseDown ? 1 : 0;

			// ── Intermediate parsed data at AD94FC ──
			// These mirror what BC7F0 would write from serial packets.
			*(volatile uint16_t*)0xAD950C = rawX;     // AD94FC+0x10: X raw
			*(volatile uint16_t*)0xAD950E = rawY;     // AD94FC+0x12: Y raw
			*(volatile uint16_t*)0xAD9510 = mouseDown ? 1 : 0; // +0x14: Z
			*(volatile float*)0xAD9514 = fx;          // AD94FC+0x18: X float
			*(volatile float*)0xAD9518 = invertedY;   // AD94FC+0x1C: Y float
			*(volatile float*)0xAD951C = mouseDown ? 1.0f : 0.0f; // +0x20: Z

			// Keep data-processing flag idle so BD020 returns true
			*(volatile uint32_t*)0xAD93FC = 0;
		}
		Sleep(16);
	}
	return 0;
}

// ── XBE hash constants ───────────────────────────────────────────

// Quest of D (CDV-10005C)
static const uint64_t kQoD1Hashes[] = {
	0x61E15DD1B354D6CBULL, // raw
	0x49F5486F29994E90ULL, // runtime (no SEGABOOT)
};
// Quest of D: Oukoku no Syugosya Ver. 3.02
static const uint64_t kQoD302Hashes[] = {
	0x15961E60C2CE0EDCULL, // raw
	0xE7C222A212B1E24FULL, // runtime (no SEGABOOT)
};
// Quest of D: The Battle Kingdom (CDV-10035B)
static const uint64_t kQoDTBKHashes[] = {
	0xDECEC3A6A90CBE76ULL, // raw
	0xBB91D51AD16D5AF0ULL, // runtime (no SEGABOOT)
	0xE9EE166CCCBD7847ULL, // runtime (Chihiro Type-3 boot)
};

enum QodGame { QOD_NONE, QOD_1, QOD_302, QOD_TBK };

static QodGame IdentifyQodGame(uint64_t xbeHash)
{
	for (auto h : kQoD1Hashes)   if (xbeHash == h) return QOD_1;
	for (auto h : kQoD302Hashes) if (xbeHash == h) return QOD_302;
	for (auto h : kQoDTBKHashes) if (xbeHash == h) return QOD_TBK;
	return QOD_NONE;
}

bool IsQuestOfDXbe(uint64_t xbeHash)
{
	return IdentifyQodGame(xbeHash) != QOD_NONE;
}

// ── Main patch entry point ───────────────────────────────────────

void ApplyQuestOfDPatches(uint64_t xbeHash, uint32_t imageSize)
{
	AddVectoredExceptionHandler(1, QodCrashHandler);
#if defined(_DEBUG)
	atexit(QodAtExit);
	remove("C:\\temp\\qod_crash.log");
#endif

	QodGame game = IdentifyQodGame(xbeHash);
	const char* gameName = "???";
	if (game == QOD_1)   gameName = "Quest of D (CDV-10005C)";
	if (game == QOD_302) gameName = "Quest of D v3.02";
	if (game == QOD_TBK) gameName = "Quest of D: Battle Kingdom";
	QOD_LOG("Applying patches for %s (imageSize=0x%X)", gameName, imageSize);

	// Type-3 boot hash: scan-based patterns match SEGABOOT library functions
	// embedded in the XBE (sub_3E910, sub_3F510, sub_3FC60, etc.).
	// Patching them causes "Error 02 - Main board malfunctioning."
	// Network progress relies on correct FPGA mailbox responses instead.
	if (xbeHash == 0xE9EE166CCCBD7847ULL) {
		QOD_LOG("Type-3 boot hash detected — skipping patches (SEGABOOT library)");
		return;
	}

	g_QodGamePatchesActive = true;

	// ═══════════════════════════════════════════════════════════════
	// Chihiro board detection — force return 1
	// 0x6E660: CALL sub; TEST EAX,EAX; JZ+9; CMP BYTE[EAX+4],1;
	//          JNZ+3; MOV AL,1; RET; XOR AL,AL; RET
	// ═══════════════════════════════════════════════════════════════
	{
		static const uint8_t kBoardDetectPat[] = {
			0xE8, 0xFF,0xFF,0xFF,0xFF,
			0x85, 0xC0,
			0x74, 0x09,
			0x80, 0x78, 0x04, 0x01,
			0x75, 0x03,
			0xB0, 0x01,
			0xC3,
			0x32, 0xC0,
			0xC3
		};
		uintptr_t va = ScanXbe(kBoardDetectPat, sizeof(kBoardDetectPat), imageSize);
		if (va) {
			static const uint8_t kAlways1[] = { 0xB0,0x01, 0xC3 };
			PatchXbeBytes(va, kAlways1, sizeof(kAlways1));
			QOD_LOG("Board detection forced at 0x%08X", (unsigned)va);
		} else {
			QOD_LOG("Board detection pattern not found!");
		}
	}

	// ═══════════════════════════════════════════════════════════════
	// Network board serial check — force return 0
	// 0x6E5E0: MOV EAX,[ptr]; MOV CL,[EAX+4]; TEST CL,CL; SETNE AL; RET
	// Patch to: XOR AL,AL; RET  (return 0 = "board finished/idle")
	// Returning 1 causes: (a) state 14 handler to loop forever,
	//                      (b) init display to show "GAME TEST MODE"
	// ═══════════════════════════════════════════════════════════════
	{
		static const uint8_t kNetBoardCheckPat[] = {
			0xA1, 0xFF,0xFF,0xFF,0xFF,
			0x8A, 0x48, 0x04,
			0x84, 0xC9,
			0x0F, 0x95, 0xC0,
			0xC3
		};
		uintptr_t va = ScanXbe(kNetBoardCheckPat, sizeof(kNetBoardCheckPat), imageSize);
		if (va) {
			static const uint8_t kAlways0[] = { 0x30,0xC0, 0xC3, 0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90 };
			PatchXbeBytes(va, kAlways0, sizeof(kAlways0));
			QOD_LOG("Network board check forced to 0 at 0x%08X", (unsigned)va);
		} else {
			QOD_LOG("Network board check pattern not found!");
		}
	}

	// ═══════════════════════════════════════════════════════════════
	// Baseboard init error bypass (JZ → JMP)
	// Pattern: CALL sub; TEST EAX,EAX; JZ +17; PUSH str; MOV [dword],val
	// QoD1 uses -2 (0xFE), QoD302/TBK uses -101 (0x9B)
	// ═══════════════════════════════════════════════════════════════
	{
		static const uint8_t kBaseboardInitPat[] = {
			0xE8, 0xFF,0xFF,0xFF,0xFF,
			0x85, 0xC0,
			0x74, 0x17,
			0x68, 0xFF,0xFF,0xFF,0xFF,
			0xC7, 0x05, 0xFF,0xFF,0xFF,0xFF,
			0xFF
		};
		uintptr_t va = ScanXbe(kBaseboardInitPat, sizeof(kBaseboardInitPat), imageSize);
		if (va) {
			uint8_t jmp = 0xEB;
			PatchXbeBytes(va + 7, &jmp, 1);
			QOD_LOG("Baseboard init error bypassed at 0x%08X", (unsigned)va);
		} else {
			QOD_LOG("Baseboard init error pattern not found!");
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
				PatchWithJmp(va - 5, (const void*)&QodLinkOkHook);
				QOD_LOG("LinkOK patched at 0x%08X", (unsigned)(va - 5));
			}
		}
		if (hits.empty()) QOD_LOG("LinkOK pattern not found");
	}

	// ═══════════════════════════════════════════════════════════════
	// State 12 queue-empty gate — return true for TBK
	// 0x6D300 checks a work queue at [0xF32954]/[0xF32958]. The queue
	// is never initialized under our network bypass path, so state 12
	// never advances. Returning true lets the later setup states run.
	// ═══════════════════════════════════════════════════════════════
	if (game == QOD_TBK) {
		static const uint8_t kState12QueuePat[] = {
			0x8B, 0x0D, 0x54, 0x29, 0xF3, 0x00,
			0x85, 0xC9,
			0x74, 0x0E,
			0xA1, 0x58, 0x29, 0xF3, 0x00,
			0x2B, 0xC1,
			0xC1, 0xF8, 0x02,
			0x85, 0xC0,
			0x75, 0x03,
			0xB0, 0x01,
			0xC3,
			0x32, 0xC0,
			0xC3
		};
		uintptr_t va = ScanXbe(kState12QueuePat, sizeof(kState12QueuePat), imageSize);
		if (va) {
			static const uint8_t kRet1[] = { 0xB0, 0x01, 0xC3 };
			PatchXbeBytes(va, kRet1, sizeof(kRet1));
			QOD_LOG("State 12 queue gate forced true at 0x%08X", (unsigned)va);
		} else {
			QOD_LOG("State 12 queue gate pattern not found");
		}
	}

	// ═══════════════════════════════════════════════════════════════
	// State 12 post-cleanup condition — return true for TBK
	// After the queue gate passes, state 12 calls 0xBF4A0 before it can
	// advance to state 13. Force success so the startup path can keep
	// progressing under the network/data-load bypass.
	// ═══════════════════════════════════════════════════════════════
	if (game == QOD_TBK) {
		static const uint8_t kState12AdvancePat[] = {
			0xE8, 0xEB, 0xF1, 0xFF, 0xFF,
			0x8B, 0x00,
			0x24, 0x01,
			0xFE, 0xC8,
			0xF6, 0xD8,
			0x1B, 0xC0,
			0x40,
			0xC3
		};
		uintptr_t va = ScanXbe(kState12AdvancePat, sizeof(kState12AdvancePat), imageSize);
		if (va) {
			// Save original bytes so we can restore after boot
			memcpy(g_QodAdvanceGateOrig, (void*)va, 3);
			g_QodAdvanceGateAddr = va;
			static const uint8_t kRet1[] = { 0xB0, 0x01, 0xC3 };
			PatchXbeBytes(va, kRet1, sizeof(kRet1));
			QOD_LOG("State 12 advance gate forced true at 0x%08X (will restore at state 15)", (unsigned)va);
		} else {
			QOD_LOG("State 12 advance gate pattern not found");
		}
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
			QOD_LOG("DIMM-ready patched at 0x%08X", (unsigned)va);
		} else {
			QOD_LOG("DIMM-ready pattern not found");
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
			// Don't stub — let the NV2A emulation handle it properly.
			// Stubbing breaks D3D device initialization (backbuffer never created).
			QOD_LOG("KickOffAndWaitForIdle found at 0x%08X (NOT stubbed)", (unsigned)va);
		} else {
			QOD_LOG("KickOffAndWaitForIdle pattern not found");
		}
	}

	// ═══════════════════════════════════════════════════════════════
	// State 14 sub-wait bypass + validation skip
	// 1) NOP the JL that waits for sub to reach a time threshold
	// 2) After calling 0x72FC0(2) to set MODE-WARNING, CALL 0xAEFE0
	//    checks hardware. Change the following JZ to JMP to skip
	//    all validation (JVS node check, DIMM version, etc.) and
	//    go directly to the state 15 transition.
	// Pattern 1: CVTTSS2SI; CMP ECX,EAX; JL rel32
	// Pattern 2: CALL; TEST AL,AL; JZ rel32 (after 0x72FC0(2))
	// ═══════════════════════════════════════════════════════════════
	{
		// Sub-wait bypass: NOP the JL
		static const uint8_t kState14WaitPat[] = {
			0xF3, 0x0F, 0x2C, 0xC0,         // CVTTSS2SI EAX, XMM0
			0x3B, 0xC8,                       // CMP ECX, EAX
			0x0F, 0x8C                         // JL rel32 (first 2 bytes)
		};
		uintptr_t va = ScanXbe(kState14WaitPat, sizeof(kState14WaitPat), imageSize);
		if (va) {
			static const uint8_t kNop6[] = { 0x90,0x90,0x90,0x90,0x90,0x90 };
			PatchXbeBytes(va + 6, kNop6, sizeof(kNop6));
			QOD_LOG("State 14 sub-wait bypassed at 0x%08X", (unsigned)va);
		} else {
			QOD_LOG("State 14 sub-wait pattern not found!");
		}

		// Validation skip: change JZ to JMP after CALL 0xAEFE0
		// At 0x73672: E8 69 B9 03 00 (CALL 0xAEFE0)
		// At 0x73677: 84 C0 (TEST AL,AL)
		// At 0x73679: 0F 84 E5 00 00 00 (JZ near → success path)
		// When 0xAEFE0 returns 0, JZ goes to state 15 (success).
		// When it returns non-zero, validation continues and fails.
		// Patch JZ to JMP to always take the success path.
		// Use the specific CALL offset (69 B9 03 00) for unique match.
		static const uint8_t kValSkipPat[] = {
			0xE8, 0xFF,0xFF,0xFF,0xFF,  // CALL 0x72FC0 (wildcards)
			0xE8, 0x69, 0xB9, 0x03, 0x00, // CALL 0xAEFE0 (specific offset)
			0x84, 0xC0,                  // TEST AL, AL
			0x0F, 0x84                   // JZ near (first 2 bytes)
		};
		uintptr_t va2 = ScanXbe(kValSkipPat, sizeof(kValSkipPat), imageSize);
		if (va2) {
			// Patch JZ (at va2+12) to JMP
			// JZ: 0F 84 E5 00 00 00 → JMP: E9 E6 00 00 00 90
			uint8_t* jzAddr = (uint8_t*)(uintptr_t)(va2 + 12);
			uint32_t jzOffset = *(uint32_t*)(jzAddr + 2); // read JZ offset
			uint32_t jmpOffset = jzOffset + 1; // JMP is 1 byte shorter
			static uint8_t kJmp[6];
			kJmp[0] = 0xE9; // JMP near
			memcpy(&kJmp[1], &jmpOffset, 4);
			kJmp[5] = 0x90; // NOP
			PatchXbeBytes(va2 + 12, kJmp, sizeof(kJmp));
			QOD_LOG("State 14 validation skipped at 0x%08X (JZ→JMP)", (unsigned)(va2 + 12));
		} else {
			QOD_LOG("State 14 validation skip pattern not found!");
		}

		// Fix the dispatch loop re-entry: the state 14 success path does:
		//   MOV EDX, 15; MOV [0x8D1970], EDX; JMP loop_body
		// But EDX was loaded at function start (stale=14), and the JMP
		// goes back to the dispatch loop which re-enters state 14 handler.
		// Patch the JMP to go to the POP+RET exit instead.
		// Pattern: BA 0F 00 00 00 89 15 70 19 8D 00 E9
		static const uint8_t kLoopFixPat[] = {
			0xBA, 0x0F, 0x00, 0x00, 0x00,       // MOV EDX, 15
			0x89, 0x15, 0x70, 0x19, 0x8D, 0x00,  // MOV [0x8D1970], EDX
			0xE9                                   // JMP (start byte)
		};
		uintptr_t va3 = ScanXbe(kLoopFixPat, sizeof(kLoopFixPat), imageSize);
		if (va3) {
			// The JMP is at va3+11, 5 bytes total. Find the POP+RET exit
			// by scanning forward for 5F 5E 5D 59 C3.
			uint8_t* jmpInstr = (uint8_t*)(uintptr_t)(va3 + 11);
			int32_t oldOffset = *(int32_t*)(jmpInstr + 1);
			// Scan forward from jmpInstr for POP EDI;ESI;EBP;ECX;RET
			bool found = false;
			for (int i = 5; i < 0x200; i++) {
				uint8_t* candidate = jmpInstr + i;
				if (candidate[0]==0x5F && candidate[1]==0x5E && candidate[2]==0x5D &&
				    candidate[3]==0x59 && candidate[4]==0xC3) {
					int32_t newOffset = (int32_t)(candidate - (jmpInstr + 5));
					uint8_t patch[5] = { 0xE9 };
					memcpy(patch + 1, &newOffset, 4);
					PatchXbeBytes(va3 + 11, patch, 5);
					QOD_LOG("State 14 loop-back fixed at 0x%08X (JMP offset %d→%d)",
					        (unsigned)(va3+11), oldOffset, newOffset);
					found = true;
					break;
				}
			}
			if (!found) QOD_LOG("POP+RET exit not found near 0x%08X!", (unsigned)(va3+11));
		} else {
			QOD_LOG("State 14 loop-fix pattern not found!");
		}
	}

	// ═══════════════════════════════════════════════════════════════
	// CRI ADXF_GetStat hook (QoD1 only — QoD302/TBK have different CRI SDK)
	// ═══════════════════════════════════════════════════════════════

		// State 12 success path: set state=13, update Network Connect,
		// then JMP back into the dispatch body with stale state still in
		// a register. Redirect the JMP to the common POP+RET exit.
		static const uint8_t kState12LoopFixPat[] = {
			0xBA, 0x0D, 0x00, 0x00, 0x00,
			0x89, 0x15, 0x70, 0x19, 0x8D, 0x00,
			0x89, 0x3D, 0x9C, 0x19, 0x8D, 0x00,
			0xE9
		};
		uintptr_t va4 = ScanXbe(kState12LoopFixPat, sizeof(kState12LoopFixPat), imageSize);
		if (va4) {
			uint8_t* jmpInstr = (uint8_t*)(uintptr_t)(va4 + 17);
			int32_t oldOffset = *(int32_t*)(jmpInstr + 1);
			bool found = false;
			for (int i = 5; i < 0x200; i++) {
				uint8_t* candidate = jmpInstr + i;
				if (candidate[0]==0x5F && candidate[1]==0x5E && candidate[2]==0x5D &&
				    candidate[3]==0x59 && candidate[4]==0xC3) {
					int32_t newOffset = (int32_t)(candidate - (jmpInstr + 5));
					uint8_t patch[5] = { 0xE9 };
					memcpy(patch + 1, &newOffset, 4);
					PatchXbeBytes(va4 + 17, patch, 5);
					QOD_LOG("State 12 loop-back fixed at 0x%08X (JMP offset %d→%d)",
					        (unsigned)(va4 + 17), oldOffset, newOffset);
					found = true;
					break;
				}
			}
			if (!found) QOD_LOG("State 12 POP+RET exit not found near 0x%08X!", (unsigned)(va4 + 17));
		} else {
			QOD_LOG("State 12 loop-fix pattern not found!");
		}

		// State 13 success path has the same stale-register loop-back.
		static const uint8_t kState13LoopFixPat[] = {
			0xBA, 0x0E, 0x00, 0x00, 0x00,
			0x89, 0x15, 0x70, 0x19, 0x8D, 0x00,
			0xE9
		};
		uintptr_t va5 = ScanXbe(kState13LoopFixPat, sizeof(kState13LoopFixPat), imageSize);
		if (va5) {
			uint8_t* jmpInstr = (uint8_t*)(uintptr_t)(va5 + 11);
			int32_t oldOffset = *(int32_t*)(jmpInstr + 1);
			bool found = false;
			for (int i = 5; i < 0x200; i++) {
				uint8_t* candidate = jmpInstr + i;
				if (candidate[0]==0x5F && candidate[1]==0x5E && candidate[2]==0x5D &&
				    candidate[3]==0x59 && candidate[4]==0xC3) {
					int32_t newOffset = (int32_t)(candidate - (jmpInstr + 5));
					uint8_t patch[5] = { 0xE9 };
					memcpy(patch + 1, &newOffset, 4);
					PatchXbeBytes(va5 + 11, patch, 5);
					QOD_LOG("State 13 loop-back fixed at 0x%08X (JMP offset %d→%d)",
					        (unsigned)(va5 + 11), oldOffset, newOffset);
					found = true;
					break;
				}
			}
			if (!found) QOD_LOG("State 13 POP+RET exit not found near 0x%08X!", (unsigned)(va5 + 11));
		} else {
			QOD_LOG("State 13 loop-fix pattern not found!");
		}

	// ═══════════════════════════════════════════════════════════════
	// ADX Logo skip — multiple patches needed:
	// 1. sub_6D3D0: spin-loop that waits for process completion
	//    (calls sub_6D300 in a loop). Without CRI ADX, sub_6D300
	//    never returns true, so this loops forever. Called from
	//    sub_73D70 (prev task cleanup) during sub_72FE0 execution.
	//    This blocks sub_74C30, preventing the A93684 check at
	//    0x72F11 from running. Fix: patch to RET.
	// 2. Callback (0x73E50) and draw (0x73E90): call CRI ADX
	//    functions that may also block. Fix: patch to RET.
	// ═══════════════════════════════════════════════════════════════
	// CRI ADX task callbacks, draws, and cleanups run naturally.
	// sub_6D3D0 (CRI spin-loop) and sub_73D70 (shared CRI cleanup) are
	// already patched to RET below, so cleanup functions won't block on
	// CRI wait loops. Letting cleanups run allows them to properly
	// initialize the next task entry (e.g. funcID=5 ADV_MOVIE / attract).
	QOD_LOG("CRI ADX task cleanups left intact (sub_6D3D0/sub_73D70 prevent CRI blocking)");

	// ═══════════════════════════════════════════════════════════════
	// sub_6D3D0 — CRI spin-loop that waits for process completion.
	// Called from task cleanup functions (sub_73D70) during transitions.
	// Without CRI middleware, sub_6D300 never returns true, so this
	// spins forever. MUST be patched before game code executes
	// (not in guard thread) — the game thread enters this function
	// during early task transitions and gets stuck if not patched.
	// ═══════════════════════════════════════════════════════════════
	{
		DWORD oldProt;
		if (VirtualProtect((void*)0x6D3D0, 1, PAGE_EXECUTE_READWRITE, &oldProt)) {
			*(uint8_t*)0x6D3D0 = 0xC3; // RET
			VirtualProtect((void*)0x6D3D0, 1, oldProt, &oldProt);
			QOD_LOG("Patched sub_6D3D0 (CRI spin-loop) to RET");
		}
	}

	// sub_73D70 — prev task cleanup / shared CRI cleanup.
	// Called from sub_72FE0 during task transitions and from state 14 init.
	// Calls CRI wait functions (sub_6D3A0, sub_6D3D0) that block.
	// Must be RET'd to prevent state 14 hang.
	{
		DWORD oldProt;
		if (VirtualProtect((void*)0x73D70, 1, PAGE_EXECUTE_READWRITE, &oldProt)) {
			*(uint8_t*)0x73D70 = 0xC3; // RET
			VirtualProtect((void*)0x73D70, 1, oldProt, &oldProt);
			QOD_LOG("Patched sub_73D70 (shared CRI cleanup) to RET");
		}
	}

	// ═══════════════════════════════════════════════════════════════
	// sub_6D3A0 and sub_6D320 — CRI wait functions used legitimately
	// during init. Patched at RUNTIME (state >= 15) by the guard thread
	// to avoid breaking the init sequence.

	// ═══════════════════════════════════════════════════════════════
	// sub_AEFE0 — main loop exit check. Returns non-zero when
	// hardware/network conditions want the loop to exit. In emulation
	// those conditions fire spuriously. Patch to return 0 (keep alive).
	// ═══════════════════════════════════════════════════════════════
	{
		DWORD oldProt;
		if (VirtualProtect((void*)0xAEFE0, 3, PAGE_EXECUTE_READWRITE, &oldProt)) {
			*(uint8_t*)0xAEFE0 = 0x33; // XOR EAX, EAX
			*(uint8_t*)0xAEFE1 = 0xC0;
			*(uint8_t*)0xAEFE2 = 0xC3; // RET
			VirtualProtect((void*)0xAEFE0, 3, oldProt, &oldProt);
			QOD_LOG("Patched sub_AEFE0 (main loop exit) to return 0");
		}
	}

	if (game == QOD_1) {
		static const uint8_t kGetStatPat[] = {
			0x8B, 0x44, 0x24, 0x04, 0x85, 0xC0, 0x75, 0x13, 0x68
		};
		uintptr_t va = ScanXbe(kGetStatPat, sizeof(kGetStatPat), imageSize);
		if (va) {
			PatchWithJmp(va, (const void*)&QodGetStatHook);
			QOD_LOG("CRI GetStat hooked at 0x%08X", (unsigned)va);
		} else {
			QOD_LOG("CRI GetStat pattern not found");
		}
	}

	// ═══════════════════════════════════════════════════════════════
	// Card reader state machine — hook or stub
	// For TBK: Hook B1AD0 with our IC card file I/O handler.
	// For older games: stub the card reader (return 0).
	// B1AD0 pattern: CMP [43593C],1; JNE; CALL; MOV EAX,[AD08C4]
	// ═══════════════════════════════════════════════════════════════
	if (game == QOD_TBK) {
		// Pattern match B1AD0: cmp dword [0x43593c], 1
		static const uint8_t kCardRuntimePat[] = {
			0x83, 0x3D, 0x3C, 0x59, 0x43, 0x00, 0x01,  // CMP [43593C], 1
			0x75, 0x05,                                   // JNE +5
			0xE8                                           // CALL (start)
		};
		uintptr_t va = ScanXbe(kCardRuntimePat, sizeof(kCardRuntimePat), imageSize);
		if (va) {
			PatchWithJmp(va, (const void*)&QodIcCardRuntimeHook);
			QOD_LOG("IC Card runtime hooked at 0x%08X (B1AD0)", (unsigned)va);
		} else {
			QOD_LOG("IC Card runtime pattern not found — card operations will hang!");
		}
	} else {
		static const uint8_t kCardReaderPat[] = {
			0xA1, 0xFF,0xFF,0xFF,0xFF, 0x83,0xEC,0x08, 0x48, 0x83,0xF8,0x05, 0x0F,0x87
		};
		uintptr_t va = ScanXbe(kCardReaderPat, sizeof(kCardReaderPat), imageSize);
		if (va) {
			static const uint8_t kRet0[] = { 0x33,0xC0, 0xC3 };
			PatchXbeBytes(va, kRet0, sizeof(kRet0));
			QOD_LOG("Card reader stubbed at 0x%08X", (unsigned)va);
		} else {
			QOD_LOG("Card reader pattern not found");
		}
	}

	// ═══════════════════════════════════════════════════════════════
	// JvsNodeSendPacket / JvsNodeReceivePacket hooks
	// QoD statically links the Sega JVS SDK (in XPP section), so the
	// standard symbol-based HLE patches can't find JvsNodeSend/Recv.
	// We pattern-scan the XPP section for the "Status error <JvsNode*>"
	// strings, trace back to find the function prologues, and hook them
	// to route through g_pJvsIo — the emulated JVS I/O board.
	// This replaces all the old JVS stubs and enables real JVS input
	// (buttons, coins, TEST/SERVICE via TeknoParrot or F1/F2 keys).
	// ═══════════════════════════════════════════════════════════════
	{
		// Find "Status error <JvsNodeReceivePacket>" string in XBE
		static const uint8_t kRecvErrStr[] = "Status error <JvsNodeReceivePacket>";
		static const uint8_t kSendErrStr[] = "Status error <JvsNodeSendPacket>";
		uintptr_t recvFuncVA = 0, sendFuncVA = 0;

		// Helper lambda: scan for a string, find the PUSH xref in code,
		// walk back to find NOP padding after the previous function's RET,
		// skip the NOPs to reach the function prologue.
		auto findJvsNodeFunc = [&](const uint8_t* errStr, size_t errStrLen) -> uintptr_t {
			uint8_t* imageBase = (uint8_t*)0x10000;
			for (uintptr_t off = 0; off < imageSize - errStrLen; off++) {
				if (memcmp(imageBase + off, errStr, errStrLen) == 0) {
					uintptr_t strVA = 0x10000 + off;
					for (uintptr_t co = 0; co < imageSize - 5; co++) {
						if (imageBase[co] == 0x68 && *(uint32_t*)(imageBase + co + 1) == strVA) {
							uintptr_t pushVA = 0x10000 + co;
							// Walk back to find NOP padding after previous function's RET
							for (uintptr_t back = pushVA - 1; back > pushVA - 0x200; back--) {
								uint8_t* p = (uint8_t*)back;
								// Accept any RET variant (C3, C2 xx 00) followed by NOP(s)
								if (p[0] == 0x90 && (p[-1] == 0xC3 ||
									(p[-1] == 0x00 && p[-3] == 0xC2))) {
									uintptr_t funcStart = back;
									while (*(uint8_t*)funcStart == 0x90) funcStart++;
									return funcStart;
								}
							}
							break;
						}
					}
					break;
				}
			}
			return 0;
		};
		recvFuncVA = findJvsNodeFunc(kRecvErrStr, sizeof(kRecvErrStr) - 1);
		sendFuncVA = findJvsNodeFunc(kSendErrStr, sizeof(kSendErrStr) - 1);

		if (recvFuncVA && sendFuncVA) {
			PatchWithJmp(recvFuncVA, (const void*)&QodJvsNodeRecvPacketHook);
			PatchWithJmp(sendFuncVA, (const void*)&QodJvsNodeSendPacketHook);
			QOD_LOG("JvsNodeRecvPacket hooked at 0x%08X", (unsigned)recvFuncVA);
			QOD_LOG("JvsNodeSendPacket hooked at 0x%08X", (unsigned)sendFuncVA);
		} else {
			QOD_LOG("JvsNode functions NOT FOUND (recv=0x%08X send=0x%08X)!", (unsigned)recvFuncVA, (unsigned)sendFuncVA);
		}
	}

	// Populate DIMM board info at D76CB4
	// sub_1B14E0 returns &unk_D76CB4. State 11 checks:
	// - byte[20] (D76CC8) must be nonzero
	// - byte[21] (D76CC9) must be nonzero
	// - byte[20] > 0x12 OR (byte[20]==0x12 AND byte[21]>=9)
	// Only for known builds (hardcoded addresses)
	if (game == QOD_TBK && xbeHash != 0xE9EE166CCCBD7847ULL) {
		uint8_t* dimmInfo = (uint8_t*)(uintptr_t)0xD76CB4;
		dimmInfo[20] = 0x13; // firmware major version (>0x12 passes check)
		dimmInfo[21] = 0x01; // firmware minor version (nonzero)
		QOD_LOG("DIMM board info populated at D76CB4: ver %d.%d", dimmInfo[20], dimmInfo[21]);
	}

	// Populate JVS node data so state 11 node count check passes
	// Only for known builds (hardcoded addresses)
	if (game == QOD_TBK && xbeHash != 0xE9EE166CCCBD7847ULL) {
		*(uint8_t*)(uintptr_t)0xD9C761 = 2;  // 2 JVS nodes
		*(uint8_t*)(uintptr_t)0xD9CD10 = 2;  // node 1: 2 players
		*(uint8_t*)(uintptr_t)0xD9CD11 = 16; // node 1: 16 bits per player (2*16=32 >= 8)
		QOD_LOG("JVS node data populated: count=2, players=2, bits=16");
	}

	// ═══════════════════════════════════════════════════════════════
	// Task-7 gate — NOT patched. The gate byte [0x8D39FC] is set
	// naturally by the IC card init hook when the card reader is ready.
	// Forcing it to 1 caused the game to skip the IC card flow and
	// jump straight to game-results.
	// ═══════════════════════════════════════════════════════════════

	// ═══════════════════════════════════════════════════════════════
	// Touch Panel emulation
	// Hook the __thiscall update function that polls serial hardware.
	// Pattern: PUSH ESI; MOV ESI,ECX; MOV EAX,[ESI+4]; MOV EDX,1;
	//          CMP EAX,EDX; JNE xx; MOV EAX,[ESI+0C]; CMP EAX,8
	// ═══════════════════════════════════════════════════════════════
	{
		static const uint8_t kTouchPat[] = {
			0x56, 0x8B, 0xF1, 0x8B, 0x46, 0x04,
			0xBA, 0x01, 0x00, 0x00, 0x00,
			0x3B, 0xC2,
			0x0F, 0x85, 0xFF, 0xFF, 0x00, 0x00,
			0x8B, 0x46, 0x0C,
			0x83, 0xF8, 0x08,
		};
		uintptr_t va = ScanXbe(kTouchPat, sizeof(kTouchPat), imageSize);
		if (va) {
			PatchWithJmp(va, (const void*)&QodTouchUpdateHook);
			QOD_LOG("Touch panel update hooked at 0x%08X", (unsigned)va);
		} else {
			QOD_LOG("Touch panel update pattern not found!");
		}
	}

	// ═══════════════════════════════════════════════════════════════
	// IC Card Reader emulation
	// Same prologue as Touch but step count is 7 instead of 8.
	// ═══════════════════════════════════════════════════════════════
	{
		static const uint8_t kIcPat[] = {
			0x56, 0x8B, 0xF1, 0x8B, 0x46, 0x04,
			0xBA, 0x01, 0x00, 0x00, 0x00,
			0x3B, 0xC2,
			0x0F, 0x85, 0xFF, 0xFF, 0x00, 0x00,
			0x8B, 0x46, 0x0C,
			0x83, 0xF8, 0x07,
		};
		uintptr_t va = ScanXbe(kIcPat, sizeof(kIcPat), imageSize);
		if (va) {
			PatchWithJmp(va, (const void*)&QodIcUpdateHook);
			QOD_LOG("IC reader update hooked at 0x%08X", (unsigned)va);
		} else {
			QOD_LOG("IC reader update pattern not found!");
		}
	}

	// ═══════════════════════════════════════════════════════════════
	// Deck Reader emulation
	// Prologue: PUSH EBX; PUSH ESI; MOV ESI,ECX; ... CMP EAX,3
	// ═══════════════════════════════════════════════════════════════
	{
		static const uint8_t kDeckPat[] = {
			0x53, 0x56, 0x8B, 0xF1, 0x8B, 0x46, 0x04,
			0xBB, 0x01, 0x00, 0x00, 0x00,
			0x3B, 0xC3,
			0x0F, 0x85, 0xFF, 0x00, 0x00, 0x00,
			0x8B, 0x46, 0x0C,
			0x83, 0xF8, 0x03,
		};
		uintptr_t va = ScanXbe(kDeckPat, sizeof(kDeckPat), imageSize);
		if (va) {
			PatchWithJmp(va, (const void*)&QodDeckUpdateHook);
			QOD_LOG("Deck reader update hooked at 0x%08X", (unsigned)va);
		} else {
			QOD_LOG("Deck reader update pattern not found!");
		}
	}

	// ═══════════════════════════════════════════════════════════════
	// Ensure card.bin exists before game code runs — state 12 depends
	// on it being present during IC reader init.
	// ═══════════════════════════════════════════════════════════════
	if (game == QOD_TBK && xbeHash != 0xE9EE166CCCBD7847ULL) {
		QodCardPathInit();
		FILE* chk = fopen(g_cardFilePath, "rb");
		if (!chk) {
			chk = fopen(g_cardFilePath, "wb");
			if (chk) {
				uint8_t blank[207];
				memset(blank, 0, sizeof(blank));
				fwrite(blank, 1, sizeof(blank), chk);
				fclose(chk);
				QOD_LOG("Created blank card.bin: %s", g_cardFilePath);
			}
		} else {
			fclose(chk);
		}
	}

	// ═══════════════════════════════════════════════════════════════
	// Hook HalReturnToFirmware to intercept QuickReboot
	// After reading a blank card, the game reboots to "register"
	// with the server. Without a server, this loops forever.
	// We suppress the reboot and save card data.
	// ═══════════════════════════════════════════════════════════════
	if (game == QOD_TBK && xbeHash != 0xE9EE166CCCBD7847ULL) {
		g_pfnQuickRebootInterceptor = QodQuickRebootInterceptor;
		QOD_LOG("QuickReboot interceptor installed (callback in HalReturnToFirmware)");
	}

	// ═══════════════════════════════════════════════════════════════
	// Start peripheral guard and touch emulation threads
	// ═══════════════════════════════════════════════════════════════
	if (game == QOD_TBK && xbeHash != 0xE9EE166CCCBD7847ULL) {
		CreateThread(nullptr, 0, QodPeripheralGuardThread, nullptr, 0, nullptr);
		CreateThread(nullptr, 0, QodTouchThread, nullptr, 0, nullptr);
		QOD_LOG("Peripheral guard + touch emulation threads started");
	}

	QOD_LOG("All patches applied for %s", gameName);
}
