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
#include "core\kernel\init\CxbxKrnl.h"
#include "devices\chihiro\JvsIo.h"

// D3D swap diagnostic — defined in Direct3D9.cpp
extern volatile uint32_t g_D3DSwapCounter;
#include <cstdio>
#include <windows.h>
#include <psapi.h>
#include <TlHelp32.h>

// Keep bring-up details in CXBXR's normal debug logger. Synchronous console
// and C:\temp writes make the Release path unnecessarily expensive under Wine.
#if defined(_DEBUG)
#define QOD_LOG(...) EmuLog(LOG_LEVEL::DEBUG, __VA_ARGS__)
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

	// Hardware watchpoint hit on byte_8D39FC write
	if (code == 0x80000004 /* STATUS_SINGLE_STEP */) {
		// Check if DR6 indicates DR0 triggered (bit 0)
		DWORD dr6 = ep->ContextRecord->Dr6;
		if (dr6 & 1) {
			uint8_t val = *(volatile uint8_t*)0x8D39FC;
			QOD_LOG("HW_WATCH: byte_8D39FC written! EIP=0x%08X val=%d EAX=0x%08X ECX=0x%08X",
				ep->ContextRecord->Eip, val,
				ep->ContextRecord->Eax, ep->ContextRecord->Ecx);
			// Clear DR6 and continue
			ep->ContextRecord->Dr6 = 0;
			return EXCEPTION_CONTINUE_EXECUTION;
		}
	}

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
	// Build path relative to XBE directory
	char xbeDir[MAX_PATH];
	strncpy(xbeDir, szFilePath_Xbe, MAX_PATH - 1);
	xbeDir[MAX_PATH - 1] = '\0';
	char* lastSlash = strrchr(xbeDir, '\\');
	if (!lastSlash) lastSlash = strrchr(xbeDir, '/');
	if (lastSlash) *(lastSlash + 1) = '\0';
	else strcat(xbeDir, "\\");
	snprintf(g_cardFilePath, MAX_PATH, "%scard.bin", xbeDir);
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
static volatile bool g_QodRebootPending = false; // Set when reboot was suppressed; guard thread resumes
static volatile int  g_QodAutoTouchCountdown = 0; // >0: touch thread simulates tap (countdown frames)
static volatile uint8_t* g_QodTransitionTrigger = nullptr; // Trigger byte in trampoline page
static uintptr_t g_QodAdvanceGateAddr = 0; // Address of sub_BF4A0 (state 12 advance gate)
static uint8_t g_QodAdvanceGateOrig[3] = {}; // Original bytes before patch

// Global flag: set when the game XBE (not SEGABOOT) patches are active.
// MediaBoard.cpp checks this to skip SEGABOOT-specific memory manipulation
// (direct entry writes, channel count patch, boot struct, forced reboot).
bool g_QodGamePatchesActive = false;

// Hook for sub_95020 (setter of byte_8D39FC game-exit flag)
// Original: MOV [8D39FC], AL; RET  (A2 FC 39 8D 00 C3)
static void __cdecl QodLogExitFlagSetter(uintptr_t retAddr, uint8_t value)
{
	if (value != 0) {
		QOD_LOG("EXIT_FLAG SET to %d — return addr=0x%08X", value, retAddr);
	}
}

static void __declspec(naked) QodExitFlagSetterHook()
{
	__asm {
		// AL = value being written
		pushad
		pushfd
		movzx eax, al
		push eax
		// Get return address from stack (past pushad/pushfd = 36 bytes)
		mov eax, [esp + 40]  // 4 (push eax) + 32 (pushad) + 4 (pushfd) = 40
		push eax
		call QodLogExitFlagSetter
		add esp, 8
		popfd
		popad
		// Execute original: MOV [8D39FC], AL; RET
		mov byte ptr ds:[0x008D39FC], al
		ret
	}
}

// Declared in EmuKrnlHal.cpp
extern bool (*g_pfnQuickRebootInterceptor)();

static bool QodQuickRebootInterceptor() {
	if (!g_QodGameFullyBooted) return false; // allow boot reboots

	// Save card data from memory before redirect — the game may have
	// put registration data into the IC card buffer during funcID=7.
	uint8_t cardBuf[207];
	memcpy(cardBuf, (void*)0xACDFE8, sizeof(cardBuf));
	bool allZero = true;
	for (int i = 0; i < 207; i++) { if (cardBuf[i] != 0) { allZero = false; break; } }
	if (!allZero) {
		QodSaveCardFile(cardBuf, sizeof(cardBuf));
		QOD_LOG("QuickReboot interceptor: saved card data (non-zero) to card.bin");
	} else {
		QOD_LOG("QuickReboot interceptor: card buffer still all zeros");
	}

	QOD_LOG("QuickReboot interceptor: reboot suppressed (boot/reset)");
	return true;
}

// Reboot redirect — called from game thread instead of sub_6E720 (reboot).
// Saves card data, then transitions to attract mode via sub_72F10.
typedef void (__cdecl *pfn_sub_72F10)();
static void __cdecl QodRebootRedirect() {
	// One-shot — original sub_6E720 never returned (it rebooted), so the
	// caller loops. We must only fire once.
	static volatile bool fired = false;
	if (fired) return;
	fired = true;
	// Save card buffer to card.bin (in case registration wrote data)
	uint8_t cardBuf[207];
	memcpy(cardBuf, (void*)0xACDFE8, sizeof(cardBuf));
	bool allZero = true;
	for (int i = 0; i < 207; i++) { if (cardBuf[i] != 0) { allZero = false; break; } }

	QOD_LOG("RebootRedirect: card buffer %s, first 16: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
		allZero ? "ALL ZERO" : "HAS DATA",
		cardBuf[0], cardBuf[1], cardBuf[2], cardBuf[3],
		cardBuf[4], cardBuf[5], cardBuf[6], cardBuf[7],
		cardBuf[8], cardBuf[9], cardBuf[10], cardBuf[11],
		cardBuf[12], cardBuf[13], cardBuf[14], cardBuf[15]);

	if (!allZero) {
		QodSaveCardFile(cardBuf, sizeof(cardBuf));
		QOD_LOG("RebootRedirect: saved card data to card.bin");
	}

	// Set A93684 to gameplay and call sub_72F10 to process transition
	*(volatile uint32_t*)0xA93684 = 0x002FAABC; // funcID=5
	QOD_LOG("RebootRedirect: set A93684=0x002FAABC (funcID=5), calling sub_72F10");
	((pfn_sub_72F10)0x72F10)();
	QOD_LOG("RebootRedirect: sub_72F10 returned, task80=0x%08X", *(volatile uint32_t*)0xA93680);
}

// Hook for B1AD0 — IC card runtime poll
// Intercepts card read/write/eject operations and handles them
// immediately using file I/O instead of serial protocol.
static volatile bool g_QodCardInserted = false; // true once card has been read

// Drive the card-reader status flags the SELECTOR queries to "read complete".
//   dword_ACDFAC is a packed status byte vector queried by:
//     sub_B1930: (BYTE0==1)  -> card present
//     sub_B1950: (BYTE1!=0)  -> reader busy
//     sub_B1970: (BYTE2==1)  -> read complete
//     sub_B19B0: (BYTE3==1)  -> read error
//   The monster-card read state machine (sub_B6310) only sets BYTE2 after a
//   real hardware parse via sub_B7490 succeeds; with no Chihiro card hardware
//   that never happens, so the SELECTOR keeps showing "insert IC card".
//   We populate the completed-read status directly. The card identity magic
//   lives at off_43590C+7148 (off_43590C = &unk_AD2590, so 0xAD2590+7148 =
//   0xAD417C, the field read as *((_DWORD*)off_43590C + 1787) in sub_B6310).
//   The parser sub_B7490 only treats a card as a full player card (copying the
//   profile + names via the qmemcpy/sub_B8A30 path) when the decoded type is
//   117637394 (0x7030112); the 0x60xxxxx family (0x6030111/0x6040111/0x6060111)
//   is recognized but yields an empty parse (return 0). So we must write
//   0x7030112 here, and to the CORRECT address (0xAD417C, not the previously
//   used 0xAD41BC which is index 1803 and is never read by the game).
static void QodSetCardReadComplete() {
	*(volatile uint8_t*)0xACDFAC = 1;          // BYTE0: card present (sub_B1930)
	*(volatile uint8_t*)0xACDFAD = 0;          // BYTE1: not busy   (sub_B1950)
	*(volatile uint8_t*)0xACDFAE = 1;          // BYTE2: read done  (sub_B1970)
	*(volatile uint8_t*)0xACDFAF = 0;          // BYTE3: no error   (sub_B19B0)
	*(volatile uint32_t*)0xAD417C = 0x7030112; // off_43590C+7148: full-player-card magic
	*(volatile uint32_t*)0xAD25B4 = 4;         // off_43590C idx9 = post-read-complete
}

static void QodIcCardRuntimeHook() {
	// Check for pending serial operation (AD08C4)
	// The original code at B1ADE handles this; we skip it.

	// Maintain card presence state on every poll
	if (g_QodCardInserted) {
		*(volatile uint32_t*)0xAD08D0 = 2;   // card reader state = present
		*(volatile uint32_t*)0xACDFC4 = 0x0B; // card ready
		*(volatile uint32_t*)0xACDFE0 = 2;    // card count
		*(volatile uint8_t*)0xC592BE = 1;     // high-level card present flag
		QodSetCardReadComplete();             // keep read-complete status latched
	}

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
			QOD_LOG("IC Card READ success: %zu bytes loaded", sizeof(cardBuf));
		} else {
			// No card file — provide a blank card (all zeros) like Golf.
			// Returning "no card" error causes the game to try to register
			// a new satellite card and reboot, which loops forever.
			memset((void*)0xACDFE8, 0, sizeof(cardBuf));
			QOD_LOG("IC Card READ: no card file — providing blank card");
		}

		// Always return success with card present
		*(volatile uint32_t*)0xACDFBC = 0; // error code = success
		*(volatile uint32_t*)0xACDFC0 = 0; // status 1
		*(volatile uint32_t*)0xACDFC4 = 0x0B; // card reader state = card ready
		*(volatile uint32_t*)0xACDFC8 = 0; // status 3
		*(volatile uint32_t*)0xACDFD0 = 0; // error
		*(volatile uint32_t*)0xACDFD4 = 0; // status 1
		*(volatile uint32_t*)0xACDFD8 = 0; // status 2
		*(volatile uint32_t*)0xACDFDC = 0; // status 3
		*(volatile uint32_t*)0xACDFE0 = 2; // card count (2 = recognized)

		// Set card reader state to "card present"
		*(volatile uint32_t*)0xAD08D0 = 2; // card reader state = card present
		QodSetCardReadComplete();          // mark the read as completed for the SELECTOR
		g_QodCardInserted = true;

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

		// ── Force FREE-PLAY + credit bank ─────────────────────
		// Now that the card pipeline works (card parses as a valid
		// 0x7030112 player card), the selector/next menus gate on
		// having credits. Force free-play (byte_90E7CF=1) so the
		// cost check (sub_269E20: free-play OR credits>=cost) always
		// passes, and keep a credit bank topped up as belt-and-braces
		// for any path that reads the credit counter directly.
		*(volatile uint8_t*)0x90E7CF = 1;       // free-play ON
		if (*(volatile uint32_t*)0x90E7E8 < 5)
			*(volatile uint32_t*)0x90E7E8 = 10; // credit bank

		// Gate7 — IC reader ready gate (AD9A48)
		// NOTE: previously wrote 0x8D39FC=1 here, but that's the
		// game-exit flag (byte_8D39FC) — NOT a gate register.
		// Writing it caused funcID=7 to exit immediately to results.

		// State 13 condition flags — network/download subsystem checks
		// can't pass without real network hardware
		*(volatile uint32_t*)0x8D19A4 = 7;

		// Force transition flag clear and render readiness.
		// Transition flag — sub_6E490 checks bit 0 of A935C6.
		// When set, sub-counter stops incrementing.
		*(volatile uint8_t*)0xA935C6 &= ~1u;

		// NOTE: previously forced bit 1 at *off_6842E0 here,
		// thinking it was a "rendering ready" bit. But bit 1
		// of *off_6842E0 is the SELECTOR advance flag — forcing
		// it caused SELECTOR to skip straight to funcID=7→8.

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
					// Restore original sub_73D70 — the full cleanup function
					// including sub_29070, sub_6A2C0 (DIALOG creation), and
					// sub_6CB30 (display cleanup). Previously we replaced this
					// with a minimal stub that hid the in-game error dialog.
					// Now that sub_6D3D0 is single-pass, the original code
					// won't block, and the DIALOG system can show errors.
					{
						static const uint8_t orig_73D70[] = {
							0xE8, 0x5B, 0x96, 0xFF, 0xFF,             // CALL sub_6D3D0
							0xB8, 0x01, 0x00, 0x00, 0x00,             // MOV EAX, 1
							0xE8, 0xF1, 0x52, 0xFB, 0xFF,             // CALL sub_29070
							0xB8, 0x02, 0x00, 0x00, 0x00,             // MOV EAX, 2
							0xE8, 0xE7, 0x52, 0xFB, 0xFF,             // CALL sub_29070
							0xC6, 0x05, 0x7A, 0x36, 0xA9, 0x00, 0x01, // MOV BYTE [A9367A], 1
							0xE8, 0x2B, 0x65, 0xFF, 0xFF,             // CALL sub_6A2C0
							0xA1, 0x8C, 0x36, 0xA9, 0x00,             // MOV EAX, [A9368C]
							0x83, 0xF8, 0xFF,                         // CMP EAX, -1
							0x74, 0x05,                               // JZ +5
							0xE9, 0x8C, 0x8D, 0xFF, 0xFF,             // JMP sub_6CB30
							0xC3                                      // RET
						};
						if (VirtualProtect((void*)0x73D70, sizeof(orig_73D70), PAGE_EXECUTE_READWRITE, &oldProt)) {
							memcpy((void*)0x73D70, orig_73D70, sizeof(orig_73D70));
							VirtualProtect((void*)0x73D70, sizeof(orig_73D70), oldProt, &oldProt);
							QOD_LOG("Runtime: restored original sub_73D70 (DIALOG system enabled)");
						}
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
					// Bypass resource 33 gate — CRI async file loading
					// for auth2d never completes in the emulator, so
					// sub_73F20 case 2 blocks forever. Patch sub_73F20
					// to return 1 immediately so funcID=4 draw can
					// transition to attract/selector modes.
					if (VirtualProtect((void*)0x73F20, 3, PAGE_EXECUTE_READWRITE, &oldProt)) {
						*(uint8_t*)0x73F20 = 0xB0; // MOV AL, 1
						*(uint8_t*)0x73F21 = 0x01;
						*(uint8_t*)0x73F22 = 0xC3; // RET
						VirtualProtect((void*)0x73F20, 3, oldProt, &oldProt);
						QOD_LOG("Runtime: patched sub_73F20 to return 1 (bypass resource 33 gate)");
					}
					// Fix touch detection — sub_195390 checks touch
					// (byte_AD9530 & 1) and calls sub_BD2E0(1) for game
					// setup, but the first check is sub_6E660() (board
					// type=Chihiro→1) which short-circuits to return 0.
					// NOP the JNZ at 0x195397 (75 36 → 90 90) so the
					// Chihiro board check doesn't block touch detection.
					if (VirtualProtect((void*)0x195397, 2, PAGE_EXECUTE_READWRITE, &oldProt)) {
						*(uint8_t*)0x195397 = 0x90; // NOP
						*(uint8_t*)0x195398 = 0x90; // NOP
						VirtualProtect((void*)0x195397, 2, oldProt, &oldProt);
						QOD_LOG("Runtime: patched sub_195390 (NOP'd board check JNZ) — touch detection enabled");
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
			} else if (curState == 14) {
				if (*outerSub < 3) *outerSub = 3;
				stateTimer++;
				// State 14 case handler calls sub_AEFE0() and only sets
				// state=15 when it returns true. But we patched sub_AEFE0
				// to return 0 (prevents main-loop exit). The JZ→JMP patch
				// at 073679 was supposed to bypass this, but the decompiled
				// code shows nested if/switch logic that can't be fully
				// fixed with a single jump patch. Force transition after
				// a short delay to let state 14 init run.
				if (stateTimer > 60) {
					QOD_LOG("State 14 timeout → forcing to 15");
					*outerState = 15; *outerSub = 0; stateTimer = 0;
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

			// ── Comprehensive edge-triggered state WATCH ──────────
			// Logs EVERY change of the key game-state values in one
			// unified stream so transitions are fully traceable (the
			// user asked for this so we don't get confused by sparse
			// polled snapshots). Each entry: {address, label}. Only
			// logs when a value actually changes.
			{
				struct WatchEntry { uintptr_t addr; const char* label; };
				static const WatchEntry kWatch[] = {
					{ 0xA93680, "task80(cur)" },
					{ 0xA93684, "A93684(pending)" },
					{ 0x8D1970, "initState" },
					{ 0x8D1974, "initSub" },
					{ 0xAD417C, "cardMagic" },
					{ 0xACDFAC, "cardStatus" },
					{ 0xACDFB8, "cardOp" },
					{ 0xAD08D0, "rdrState" },
					{ 0x90E7E8, "credits" },
					{ 0x8D39FC, "exitFlag" },
					{ 0xA93744, "selSM" },
					{ 0xA93748, "dlgId" },
				};
				static uint32_t prevWatch[sizeof(kWatch)/sizeof(kWatch[0])];
				static bool watchInit = false;
				// Selector object flags are pointer-indirected — track separately.
				static uint32_t prevObjFlags = 0xDEADBEEF;
				static uint8_t  prevFreePlay = 0xFF;
				if (!watchInit) {
					for (size_t i = 0; i < sizeof(kWatch)/sizeof(kWatch[0]); ++i)
						prevWatch[i] = *(volatile uint32_t*)kWatch[i].addr;
					watchInit = true;
				}
				for (size_t i = 0; i < sizeof(kWatch)/sizeof(kWatch[0]); ++i) {
					uint32_t v = *(volatile uint32_t*)kWatch[i].addr;
					if (v != prevWatch[i]) {
						QOD_LOG("WATCH %-16s %08X -> %08X (t80=%08X st=%d/%d)",
							kWatch[i].label, prevWatch[i], v,
							*(volatile uint32_t*)0xA93680,
							*(volatile uint32_t*)0x8D1970,
							*(volatile uint32_t*)0x8D1974);
						prevWatch[i] = v;
					}
				}
				// Selector object header flags (*(uint32*)*0x6842E0).
				uintptr_t selObj = *(volatile uintptr_t*)0x6842E0;
				uint32_t objFlags = selObj ? *(volatile uint32_t*)selObj : 0;
				if (objFlags != prevObjFlags) {
					QOD_LOG("WATCH %-16s %08X -> %08X (t80=%08X)",
						"selObjFlags", prevObjFlags, objFlags,
						*(volatile uint32_t*)0xA93680);
					prevObjFlags = objFlags;
				}
				uint8_t fp = *(volatile uint8_t*)0x90E7CF;
				if (fp != prevFreePlay) {
					QOD_LOG("WATCH %-16s %02X -> %02X", "freePlay", prevFreePlay, fp);
					prevFreePlay = fp;
				}
			}

			// Monitor game state flags continuously
			{
				static uint8_t prevGameObjByte = 0xFF;
				static uint8_t prevExitFlag = 0xFF;
				uint8_t* gameObj = *(uint8_t**)0x6842E0;
				uint8_t curGameByte = gameObj ? gameObj[0] : 0;
				uint8_t curExit = *(volatile uint8_t*)0x8D39FC;
				if (curGameByte != prevGameObjByte) {
					QOD_LOG("FLAG: *off_6842E0[0] changed 0x%02X → 0x%02X (task80=0x%08X)",
						prevGameObjByte, curGameByte, task80);
					prevGameObjByte = curGameByte;
				}
				if (curExit != prevExitFlag) {
					QOD_LOG("FLAG: byte_8D39FC changed %d → %d (task80=0x%08X)",
						prevExitFlag, curExit, task80);
					// Verify patch integrity when flag gets set
					if (curExit == 1) {
						uint8_t b0 = *(volatile uint8_t*)0x6E660;
						uint8_t b1 = *(volatile uint8_t*)0x6E661;
						uint8_t b2 = *(volatile uint8_t*)0x6E662;
						uint8_t bt = *(volatile uint8_t*)0xAF2638;
						QOD_LOG("  VERIFY: sub_6E660=[%02X %02X %02X] board_type=%d",
							b0, b1, b2, bt);
					}
					prevExitFlag = curExit;
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

		// ── Touch detection diagnostic ───────────────────────
		// sub_195390 is the touch→game path. Log state periodically.
		{
			static int advDiagCounter = 0;
			if (++advDiagCounter >= 120) { // ~2 sec
				uint32_t task80 = *(volatile uint32_t*)0xA93680;
				advDiagCounter = 0;
				QOD_LOG("TOUCH_STATE: AD9530=%d ACCD20=0x%08X A26C34=0x%08X task80=%08X",
					*(volatile uint8_t*)0xAD9530,
					*(volatile uint32_t*)0xACCD20,
					*(volatile uint32_t*)0xA26C34,
					task80);
			}
		}

		// ── Dialog text dump ──────────────────────────────────
		// Text entries: byte_A26D3C + index * 1336 + 96 (Shift-JIS)
		// Scan all 12 slots regardless of dword_A26D08
		{
			static bool dialogDumped = false;
			if (!dialogDumped && *(volatile uint32_t*)0x8D1970 >= 15) {
				// Check all 12 possible text entry slots
				bool anyText = false;
				for (int i = 0; i < 12; i++) {
					uint8_t* entry = (uint8_t*)(0xA26D3C + i * 1336);
					const char* textBase = (const char*)(entry + 96);
					if (textBase[0] != 0) { anyText = true; break; }
				}
				if (anyText) {
					dialogDumped = true;
					QOD_LOG("DIALOG TEXT DUMP (dword_A26D08=%d):", *(volatile int32_t*)0xA26D08);
					for (int i = 0; i < 12; i++) {
						uint8_t* entry = (uint8_t*)(0xA26D3C + i * 1336);
						uint8_t entState = entry[0];
						int32_t entState4 = *(int32_t*)(entry + 4);
						const char* textBase = (const char*)(entry + 96);
						int textLen = 0;
						for (int j = 0; j < 1024 && textBase[j]; j++) textLen++;
						if (textLen == 0) continue;
						// Dump raw hex (first 128 bytes)
						char hexBuf[400] = {};
						int hexPos = 0;
						int dumpLen = textLen > 128 ? 128 : textLen;
						for (int j = 0; j < dumpLen && hexPos < 390; j++) {
							hexPos += sprintf(hexBuf + hexPos, "%02X ", (uint8_t)textBase[j]);
						}
						QOD_LOG("  SLOT[%d] s0=%d s4=%d len=%d hex: %s", i, entState, entState4, textLen, hexBuf);
					}
					// Also dump pointer array at unk_A26D0C
					QOD_LOG("  PTR array: %08X %08X %08X %08X",
						*(volatile uint32_t*)0xA26D0C, *(volatile uint32_t*)0xA26D10,
						*(volatile uint32_t*)0xA26D14, *(volatile uint32_t*)0xA26D18);
				}
			}
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

				// Populate system time struct at B14AFC so sub_BF410
				// returns 1 (card registered). On real hardware this
				// is written by a vtable callback; in emulation it
				// stays zero (causing sub_BF410 to always return 0).
				// sub_BF410 also calls sub_BE500 which processes card data.
				{
					SYSTEMTIME st;
					GetLocalTime(&st);
					volatile int32_t* tm = (volatile int32_t*)0xB14AFC;
					tm[0] = st.wSecond;          // tm_sec
					tm[1] = st.wMinute;          // tm_min
					tm[2] = st.wHour;            // tm_hour
					tm[3] = st.wDay;             // tm_mday
					tm[4] = st.wMonth - 1;       // tm_mon (0-based)
					tm[5] = st.wYear - 1900;     // tm_year
					tm[6] = st.wDayOfWeek;       // tm_wday
					tm[7] = 0;                   // tm_yday
					tm[8] = -1;                  // tm_isdst (auto)
					QOD_LOG("Populated B14AFC with system time");
				}

				// Present the card as ALREADY REGISTERED: clear bit 1
				// (needs-registration).  Setting bit 1 sent the SELECTOR
				// into its registration sub-state and made the mode menu
				// inert.  sub_1923A0 reads bit 1; keeping it clear lets the
				// draw callback (0x74370) fall through to the mode-selected
				// check and keep the menu interactive.
				volatile uint8_t* evtBase = *(volatile uint8_t**)0x6842E0;
				if (evtBase) *(volatile uint32_t*)evtBase &= ~0x02u;
				*(volatile uint32_t*)0xACDFB8 = 1;
				QOD_LOG("IC Card INSERT (F3) — card presented as registered");
				QodIcCardRuntimeHook();
			}
			f3WasDown = f3Down;
		}

		// ── Auto touch when title/attract screen is stable ───
		// funcID=4 (task80=002FAA98) is the "press to start" screen.
		// After 120 frames (~2s), simulate a touch tap to advance to
		// the selector screen (funcID=6).
		{
			static int titleFrames = 0;
			static bool autoTouchDone = false;
			uint32_t curTask4 = *(volatile uint32_t*)0xA93680;
			if (curTask4 == 0x002FAA98 && !autoTouchDone) { // funcID=4
				titleFrames++;
				if (titleFrames == 120) {
					autoTouchDone = true;
					g_QodAutoTouchCountdown = 6; // 6 frames: 5 held + 1 release
					QOD_LOG("AUTO-TOUCH: Title screen stable for 120 frames, tapping center");
				}
			} else if (curTask4 != 0x002FAA98) {
				titleFrames = 0;
			}
		}

		// ── Auto card insert when selector screen is stable ───
		// After reaching funcID=6 (selector) for 180 frames (~3s),
		// automatically trigger card insert so tests can run unattended.
		{
			static int selectorFrames = 0;
			static bool autoInsertDone = false;
			uint32_t curTask = *(volatile uint32_t*)0xA93680;
			if (curTask == 0x002FAAE0 && !autoInsertDone) { // funcID=6
				selectorFrames++;
				if (selectorFrames == 20) {
					autoInsertDone = true;
					QOD_LOG("AUTO-F3: Selector reached, auto card insert");

					QodCardPathInit();
					FILE* chk = fopen(g_cardFilePath, "rb");
					if (!chk) {
						chk = fopen(g_cardFilePath, "wb");
						if (chk) {
							uint8_t blank[207];
							memset(blank, 0, sizeof(blank));
							fwrite(blank, 1, sizeof(blank), chk);
							fclose(chk);
							QOD_LOG("AUTO-F3: Created blank card file");
						}
					} else {
						fclose(chk);
					}

					SYSTEMTIME st;
					GetLocalTime(&st);
					volatile int32_t* tm = (volatile int32_t*)0xB14AFC;
					tm[0] = st.wSecond;
					tm[1] = st.wMinute;
					tm[2] = st.wHour;
					tm[3] = st.wDay;
					tm[4] = st.wMonth - 1;
					tm[5] = st.wYear - 1900;
					tm[6] = st.wDayOfWeek;
					tm[7] = 0;
					tm[8] = -1;
					QOD_LOG("AUTO-F3: Populated B14AFC");

					// Present as already-registered (clear needs-reg bit 1).
					volatile uint8_t* evtBase = *(volatile uint8_t**)0x6842E0;
					if (evtBase) *(volatile uint32_t*)evtBase &= ~0x02u;
					*(volatile uint32_t*)0xACDFB8 = 1;
					QOD_LOG("AUTO-F3: IC Card INSERT triggered — card presented as registered");
					QodIcCardRuntimeHook();
				}
			} else if (curTask != 0x002FAAE0) {
				selectorFrames = 0;
			}
		}

		// ── Maintain card presence every frame (B1AD0 hook is not polled) ──
		if (g_QodCardInserted) {
			*(volatile uint32_t*)0xAD08D0 = 2;   // card reader state = present
			*(volatile uint32_t*)0xACDFC4 = 0x0B; // card ready
			*(volatile uint32_t*)0xACDFE0 = 2;    // card count
			// NOTE: Do NOT set byte_C592BE = 1 here. C592BE is the
			// "satellite card dispenser" flag. When 1, the card-reg
			// state machine (sub_6E760) state 0 takes the satellite
			// path which calls sub_BF410 (save timer check) and
			// sub_6E720 (save + reboot). Without real DIMM hardware
			// this hangs the game loop. Leave at 0 so state 0 goes
			// to standalone path (state 1 or 5).
		}

		// ── Periodic diagnostic ───────────────────────────────
		// ── SELECTOR / card-object ground-truth logger ───────
		// Logs the card state object, IC reader struct (off_43590C@0xAD2590),
		// and status flags every ~30 frames while on the SELECTOR screen so
		// we can see exactly what the game's own card-read machine is doing.
		{
			static int selLogCtr = 0;
			uint32_t t80g = *(volatile uint32_t*)0xA93680;
			if (t80g == 0x002FAAE0 && (++selLogCtr % 30) == 0) {
				uintptr_t cardObj = *(volatile uintptr_t*)0x6842E0;
				uint32_t objFlags = cardObj ? *(volatile uint32_t*)cardObj : 0xFFFFFFFF;
				uint32_t objStep  = cardObj ? *(volatile uint32_t*)(cardObj + 4) : 0;
				uint8_t* rdr = (uint8_t*)0xAD2590; // off_43590C
				uint8_t* prof = (uint8_t*)0xAF2634; // sub_BE630() card profile
				QOD_LOG("SEL: objFlags=%08X step=%d | prof[4]=%d prof[5]=%d prof[8]=%d prof[85]=%d | magic=%08X ACDFAC=%08X ADAF08=%02X",
					objFlags, objStep,
					prof[4], prof[5], prof[8], prof[85],
					*(volatile uint32_t*)0xAD417C,
					*(volatile uint32_t*)0xACDFAC,
					*(volatile uint8_t*)0xADAF08);
			} else if (t80g != 0x002FAAE0) {
				selLogCtr = 0;
			}
		}

		// ── NETWORK FIRMWARE VERSION (passive data only) ──
		// Set firmware version at sub_268450()+20/21 = 0x90E180/81.
		// These are passively read by sub_730A0 (init SM vtable) and
		// init SM state 14. Setting them is harmless — the game only
		// reads these bytes, never writes them via network hardware.
		// DO NOT change B29ACC (network status) — setting it to 0
		// makes the game think it has a working network board, which
		// triggers active network operations on nonexistent hardware
		// and CRASHES (~47s into selector, access violation at host
		// address 0x527EB174). Leave B29ACC at its natural value (1 =
		// no network board detected).
		{
			static bool fwPatched = false;
			uint32_t initSt = *(volatile uint32_t*)0x8D1970;
			if (!fwPatched && initSt >= 11) {
				// Firmware version at sub_268450() + 20/21 = 0x90E180/81
				*(volatile uint8_t*)0x90E180 = 0x14; // major = 20
				*(volatile uint8_t*)0x90E181 = 0x05; // minor = 5
				fwPatched = true;
				uint32_t netSt = *(volatile uint32_t*)0xB29AD0;
				uint32_t netErr = *(volatile uint32_t*)0xB29ACC;
				QOD_LOG("NET-FW: set firmware 20.05 at initSt=%u | B29AD0=%u B29ACC=%u (NOT changed)", initSt, netSt, netErr);
			}
		}

		// ── SELECTOR OBSERVER (enhanced diagnostics) ──
		{
			static int gateLogCtr = 0;
			uint32_t t80a = *(volatile uint32_t*)0xA93680;
			if (t80a == 0x002FAAE0 && g_QodCardInserted) {
				if ((++gateLogCtr % 300) == 0) { // ~5s cadence
					uint32_t funcID = *(volatile uint32_t*)(t80a + 8);
					uint32_t sm     = *(volatile uint32_t*)0xA93744;
					uint32_t a93720 = *(volatile uint32_t*)0xA93720;
					uint32_t a93684 = *(volatile uint32_t*)0xA93684;
					uint8_t  prof4  = *(volatile uint8_t*)0xAF2638;
					// Network / firmware state
					uint32_t netSt  = *(volatile uint32_t*)0xB29AD0;
					uint32_t netErr = *(volatile uint32_t*)0xB29ACC;
					uint8_t  fwMaj  = *(volatile uint8_t*)0x90E180;
					uint8_t  fwMin  = *(volatile uint8_t*)0x90E181;
					// Init SM
					uint32_t initSt = *(volatile uint32_t*)0x8D1970;
					uint32_t initSub= *(volatile uint32_t*)0x8D1974;
					uint32_t initHw = *(volatile uint32_t*)0x8D197C;
					// Task entry callbacks
					uint32_t tcb28 = *(volatile uint32_t*)(t80a + 28);
					uint32_t tcb24 = *(volatile uint32_t*)(t80a + 24);
					uint32_t tcb20 = *(volatile uint32_t*)(t80a + 20);
					// Auth state variables
					uint8_t  authD8 = *(volatile uint8_t*)0xF78FD8; // sub_E1800: auth flag
					uint32_t authDC = *(volatile uint32_t*)0xF78FDC; // sub_E17E0: user_id
					uint8_t  authD6 = *(volatile uint8_t*)0xF78FD6; // sub_E1830: regist flag
					uint32_t authE4 = *(volatile uint32_t*)0xF78FE4; // network FSM state
					// Board struct dump (0xAF2634+)
					uint8_t* bs = (uint8_t*)0xAF2634;
					QOD_LOG("SEL-OBS: funcID=%u sm=%u err=%u pend=%08X | prof[4]=%u [5]=%u [8]=%u [85]=%u [86]=%u | net=%u/%u fw=%u.%02u | init=%u/%u hw=%u | auth: d8=%u dc=%u d6=%u fsm=%u | cb=%08X draw=%08X",
						funcID, sm, a93720, a93684,
						bs[4], bs[5], bs[8], bs[85], bs[86],
						netSt, netErr, fwMaj, fwMin,
						initSt, initSub, initHw,
						authD8, authDC, authD6, authE4,
						tcb28, tcb24);
				}
			}
		}

		// ── CARD AUTH BYPASS: force task 7 transition ──────────
		// The selector update callback at 0x74370 checks:
		//   if (*(byte*)*0x6842E0 & 0x02) → sub_72FC0(7)
		//   if (*(dword*)*0x6842E0 & 0x10) → sub_72FC0(4)
		// sub_72FC0(N) sets dword_A93684 = 36*N + 0x2FAA08.
		// For task 7: A93684 = 0x2FAB04.
		//
		// Setting the card object flag didn't work because the
		// selector callback may be a one-shot coroutine init, not a
		// per-frame tick. Instead, directly queue task 7 by writing
		// A93684 = 0x2FAB04, which the main dispatcher (sub_72EE0)
		// picks up on the next frame.
		{
			static int authBypassCtr = 0;
			static bool authBypassDone = false;
			uint32_t t80b = *(volatile uint32_t*)0xA93680;
			if (t80b == 0x002FAAE0 && g_QodCardInserted && !authBypassDone) {
				++authBypassCtr;
				// Wait ~3 seconds (180 frames) to let selector fully init
				if (authBypassCtr == 180) {
					// Restore prof[4] = 1 (overwritten by save data load)
					*(volatile uint8_t*)0xAF2638 = 1;

					// Set card object flags bit 1 (for any checks elsewhere)
					uintptr_t cardObj = *(volatile uintptr_t*)0x6842E0;
					if (cardObj) {
						*(volatile uint8_t*)cardObj |= 0x02;
					}

					// DIRECT task queue: set A93684 = task 7 entry
					// sub_72FC0(7): A93684 = 4 * (9 * 7) + 0x2FAA08 = 0x2FAB04
					*(volatile uint32_t*)0xA93684 = 0x002FAB04;

					authBypassDone = true;
					QOD_LOG("AUTH-BYPASS: queued task 7 via A93684=0x2FAB04 | prof4=1 | cardFlags=%08X",
						cardObj ? *(volatile uint32_t*)cardObj : 0);
				}
			} else if (t80b != 0x002FAAE0) {
				authBypassCtr = 0;
			}
		}

		// ── FUNC4 logger: watch funcID=4 (task80=002FAA98) ───
		{
			static int f4Ctr = 0;
			uint32_t t80f = *(volatile uint32_t*)0xA93680;
			if (t80f == 0x002FAA98 && (++f4Ctr % 15) == 0) {
				uintptr_t cardObj = *(volatile uintptr_t*)0x6842E0;
				uint32_t objFlags = cardObj ? *(volatile uint32_t*)cardObj : 0xFFFFFFFF;
				uint8_t* prof = (uint8_t*)0xAF2634;
				QOD_LOG("FUNC4: ctr=%d objFlags=%08X prof[4]=%d prof[5]=%d prof[8]=%d A93684=%08X timer=%.2f",
					f4Ctr, objFlags, prof[4], prof[5], prof[8],
					*(volatile uint32_t*)0xA93684, *(volatile float*)0xA93670);
			} else if (t80f != 0x002FAA98) {
				f4Ctr = 0;
			}
		}

		// ── EXPERIMENT: confirm funcID=4 intro dialog with a tap ─
		// (DISABLED) Center-tap on the funcID4 dialog cancels it back
		// to the selector. The no-tap path naturally times out and
		// proceeds to FAB4C, so do NOT auto-tap here.
		#if 0
		{
			static bool sawSelector = false;
			static int f4Confirm = 0;
			static int f4TapsLeft = 3;
			uint32_t t80c = *(volatile uint32_t*)0xA93680;
			if (t80c == 0x002FAAE0) sawSelector = true;
			if (t80c == 0x002FAA98 && sawSelector && f4TapsLeft > 0) {
				if (++f4Confirm >= 90) { // ~1.5s dwell, then tap
					f4Confirm = 0;
					f4TapsLeft--;
					g_QodAutoTouchCountdown = 6;
					int32_t dlgId = *(volatile int32_t*)0xA93748;
					QOD_LOG("F4-CONFIRM: tapping funcID4 dialog (tapsLeft=%d dlgId=%d)",
						f4TapsLeft, dlgId);
				}
			} else if (t80c != 0x002FAA98) {
				f4Confirm = 0;
			}
		}
		#endif

		diagCount++;
		if ((diagCount % 125) == 0) {
			uint32_t t80 = *(volatile uint32_t*)0xA93680;
			uint32_t regSt = *(volatile uint32_t*)0xC592C4;
			uint32_t regTm = *(volatile uint32_t*)0xC592C8;
			uint8_t cardPr = *(volatile uint8_t*)0xC592BE;
			QOD_LOG("DIAG state=%d sub=%d | MB=%d IO=%d NET=%d DL=%d NC=%d | task80=%08X A93684=%08X sm=%d timer=%.2f done=%d swap=%u | reg: s=%d t=%d card=%d",
				*(volatile uint32_t*)0x8D1970, *(volatile uint32_t*)0x8D1974,
				*(volatile uint32_t*)0x8D1990, *(volatile uint32_t*)0x8D1984,
				*(volatile uint32_t*)0x8D197C, *(volatile uint32_t*)0x8D1998,
				*(volatile uint32_t*)0x8D199C,
				t80,
				*(volatile uint32_t*)0xA93684,
				*(volatile uint32_t*)0xA93744,
				*(volatile float*)0xA93670,
				*(volatile uint8_t*)0xA93675,
				g_D3DSwapCounter,
				regSt, regTm, cardPr);
		}

		// ── Thread EIP dump — find where game thread is blocked ──
		// Fires: once at state >= 14, and again when task > FAAE0
		{
			bool shouldDump = false;
			if (!dumpedThreadEip && *(volatile uint32_t*)0x8D1970 >= 14) {
				selectorStuckTimer++;
				if (selectorStuckTimer > 62) shouldDump = true;
			}
			// ── Card reg: monitor reboot redirect progress ────
			// The reboot CALL at 0x6E94C is now redirected to set A93684
			// and call sub_72F10. Guard thread just monitors for logging.
			{
				uint32_t curTask80 = *(volatile uint32_t*)0xA93680;
				static int cardRegFrameCount = 0;
				if (curTask80 >= 0x002FAB04 && curTask80 <= 0x002FAB30) {
					cardRegFrameCount++;
					uint32_t regSt  = *(volatile uint32_t*)0xC592C4;
					uint32_t regTmr = *(volatile uint32_t*)0xC592C8;

					if (cardRegFrameCount <= 30 || (cardRegFrameCount % 60) == 0) {
						QOD_LOG("CARDREG: frame=%d C592C4=%d C592C8=%d A93684=%08X task80=0x%08X",
						        cardRegFrameCount, regSt, regTmr,
						        *(volatile uint32_t*)0xA93684,
						        curTask80);
					}
				} else {
					if (cardRegFrameCount > 0) {
						QOD_LOG("CARDREG: task left FAB04 after %d frames, C592C4=%d, new task80=0x%08X",
						        cardRegFrameCount, *(volatile uint32_t*)0xC592C4, curTask80);
					}
					cardRegFrameCount = 0;
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

// Touch panel emulation thread – maps mouse cursor to touch coordinates.
//
// The game's touch processing chain (sub_BC4D0, called from sub_BD0D0):
//   1. sub_BC9B0: copies AD9530→AD9531, zeros AD9532
//   2. sub_BC520→sub_BCA50: process serial packets (none in emulation)
//   3. State machine: reads AD9531 (prev state) and AD9532 (new data flag)
//
// Since no serial data arrives, AD9532 stays 0, the debounce counter
// never activates, and AD9530 stays at 0. Writing to AD9531/AD9532 from
// a thread races with sub_BC9B0 which clears them each frame.
//
// Solution: write directly to AD9530 (final state) AND keep the debounce
// counter (AD952C) high so the state machine's "debounce > 0" branch
// preserves our value instead of overriding it.
//
// Touch state values:  0=idle, 1=held, 2=pressed, 4=released
// sub_A88E0() checks (AD9530 & 1) → true for state 1 (held)
static DWORD WINAPI QodTouchThread(LPVOID) {
	Sleep(2000); // wait for D3D window
	QOD_LOG("Touch emulation thread started");

	bool prevMouseDown = false;

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
			float invertedY = 4095.0f - fy;

			// ── Synthetic auto-touch override ──
			// When g_QodAutoTouchCountdown > 0, simulate a tap at screen center.
			// Countdown: 6→2 = held, 1 = released, 0 = done.
			int autoTouch = g_QodAutoTouchCountdown;
			if (autoTouch > 0) {
				g_QodAutoTouchCountdown = autoTouch - 1;
				mouseDown = (autoTouch > 1); // held for frames 6..2, released at 1
				fx = 2048.0f;
				fy = 2048.0f;
				rawX = 2048;
				rawY = 2048;
				invertedY = 2048.0f;
			}

			// ── Final touch state (AD9530) ──
			// Write the state directly, matching the state machine's values.
			// Also set AD9531 (prev state copy) to match, and AD9532 (new
			// data flag) + AD952C (debounce counter) to prevent the state
			// machine from overriding our value.
			uint8_t touchState;
			if (mouseDown && !prevMouseDown) {
				touchState = 2; // pressed
			} else if (mouseDown) {
				touchState = 1; // held
			} else if (!mouseDown && prevMouseDown) {
				touchState = 4; // released
			} else {
				touchState = 0; // idle
			}

			*(volatile uint8_t*)0xAD9530 = touchState;
			*(volatile uint8_t*)0xAD9531 = touchState;
			*(volatile uint8_t*)0xAD9532 = (mouseDown || touchState == 4) ? 1 : 0;
			*(volatile int32_t*)0xAD952C = 10; // keep debounce counter alive

			{
				static uint8_t s_lastState = 0xFF;
				if (touchState != s_lastState) {
					QOD_LOG("TOUCH: state=%d ADAF08=0x%02X AD95E8=%d mouse=%d X=%.0f Y=%.0f",
						touchState, *(volatile uint8_t*)0xADAF08,
						*(volatile int32_t*)0xAD95E8,
						mouseDown ? 1 : 0, fx, invertedY);
					s_lastState = touchState;
				}
			}

			prevMouseDown = mouseDown;

			// ── Calibrated coordinate outputs ──
			*(volatile float*)0xAD9520 = fx;
			*(volatile float*)0xAD9524 = invertedY;

			// ── Intermediate parsed data at AD94FC ──
			*(volatile uint16_t*)0xAD950C = rawX;
			*(volatile uint16_t*)0xAD950E = rawY;
			*(volatile uint16_t*)0xAD9510 = mouseDown ? 1 : 0;
			*(volatile float*)0xAD9514 = fx;
			*(volatile float*)0xAD9518 = invertedY;
			*(volatile float*)0xAD951C = mouseDown ? 1.0f : 0.0f;

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
	// This is still the game process, so mark it active before returning:
	// MediaBoard uses this flag to select the game's IRQ10 response path and
	// to suppress SEGABOOT-only forced QuickReboot handling.
	if (xbeHash == 0xE9EE166CCCBD7847ULL) {
		g_QodGamePatchesActive = true;
		QOD_LOG("Type-3 game hash detected — using mailbox/IRQ path without scan patches");
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
			// Extract the CALL target to find sub_BE630
			int32_t callRel = *(int32_t*)(va + 1);
			uintptr_t subGetBoard = va + 5 + callRel;
			// sub_BE630 is: MOV EAX, imm32 (B8 xx xx xx xx); RET
			if (*(uint8_t*)subGetBoard == 0xB8) {
				uintptr_t boardStruct = *(uint32_t*)(subGetBoard + 1);
				// Set board type byte [+4] = 1 (Chihiro) so ALL
				// code that checks board type (not just sub_6E660)
				// sees the correct value from the very start.
				*(uint8_t*)(boardStruct + 4) = 1;
				QOD_LOG("Board type byte set to 1 at 0x%08X", (unsigned)(boardStruct + 4));
			}

			// NOTE: sub_6E660 is left NATURAL (returns 1 on IC cabinet).
			// Earlier experiments patched it to 0 and patched sub_AEF40 to
			// -7 to force the sub_72DF0 "start" for-loop. That was based on
			// the WRONG premise that phase 15 / task 0x2FAC24 is gameplay.
			// In fact sub_72DF0 is an I/O HEALTH MONITOR and phase 15 queues
			// the ERROR-display task (draw sub_74860) which renders
			// "Error 11 / JVS I/O board not connected" when dword_A93720==1.
			// Forcing that path only produced Error 11. Patches reverted so
			// the selector stays on the stable mode-select screen.
		} else {
			QOD_LOG("Board detection pattern not found!");
		}
	}

	// ═══════════════════════════════════════════════════════════════
	// Prevent sub_74B20 from setting game-exit flag (byte_8D39FC)
	// NOP the CALL sub_95020 at 0x74B2E (E8 ED 04 02 00)
	// On Chihiro this call should never execute (sub_6E660 check
	// skips it), but something triggers it during STARTUP.
	// ═══════════════════════════════════════════════════════════════
	{
		static const uint8_t kExitCallPat[] = {
			0xE8, 0xFF, 0xFF, 0xFF, 0xFF,  // CALL sub_95020
			0x8A, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF  // next instr context
		};
		// sub_74B20 starts with: CALL sub_6D260; MOV byte [A93675], 1; CALL sub_95020
		// Scan for the CALL followed by the specific next bytes
		// Simpler: directly patch at known address
		if (*(uint8_t*)0x74B2E == 0xE8) {
			int32_t callRel = *(int32_t*)0x74B2F;
			uintptr_t callTarget = 0x74B33 + callRel;
			QOD_LOG("Exit flag CALL at 0x74B2E targets 0x%08X", (unsigned)callTarget);
			// Verify it targets the exit flag setter (sub_95020)
			if (callTarget == 0x95020) {
				DWORD oldProt;
				if (VirtualProtect((void*)0x74B2E, 5, PAGE_EXECUTE_READWRITE, &oldProt)) {
					memset((void*)0x74B2E, 0x90, 5); // NOP * 5
					VirtualProtect((void*)0x74B2E, 5, oldProt, &oldProt);
					QOD_LOG("NOP'd exit flag CALL at 0x74B2E");
				}
			}
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
	// NOTE: sub_BF410 save timer patch REMOVED.
	// sub_BF410 returning 1 during init causes sub_6E720() to save
	// and reboot, hanging the game loop. The real fix is to NOT set
	// byte_C592BE=1 (satellite dispenser flag) so the card-reg SM
	// takes the standalone path instead.
	// ═══════════════════════════════════════════════════════════════

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
	// At boot time, sub_6D3D0 (which it calls first) is RET'd, so it won't
	// block. But sub_29070/sub_6A2C0/sub_6CB30 may not be safe before init
	// completes. RET'd at boot; original bytes restored at state >= 15 by
	// the guard thread so the DIALOG system and cleanup logic can run.
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
	// Card registration fix.
	//
	// Task table (0x24 bytes per entry):
	//   +0x08 = funcID, +0x18 = init, +0x1C = update, +0x20 = draw
	//   FAAE0 = funcID=6 (SELECTOR, "insert card" screen)
	//   FAB04 = funcID=7 (card registration task)
	//   FAB28 = funcID=8 (gameplay)
	//
	// On real Chihiro hardware the card registration flow is:
	//   1. F3 inserts card → funcID=6 (mode select) checks sub_1923A0
	//   2. sub_1923A0 returns true if card needs registration (bit 1)
	//   3. If true → transition to funcID=7 (card reg via server)
	//   4. Server registers card → reboot → come back to funcID=6
	//
	// In emulation there's no server.  Fix:
	//   1. Patch sub_1923A0 to always return 0 so funcID=7 is never
	//      entered.  The game stays at funcID=6 (mode select) and the
	//      card is treated as already registered, enabling the menus.
	//      The game stays at funcID=6 (mode select) with the card
	//      recognized, enabling the mode select buttons.
	//   2. Redirect reboot CALL at 0x6E94C → QodRebootRedirect as
	//      safety net (in case C592C4 reaches state 3 somehow).
	// ═══════════════════════════════════════════════════════════════
	if (game == QOD_TBK && xbeHash != 0xE9EE166CCCBD7847ULL) {
		// --- Skip card registration transition (patch mode select update) ---
		// funcID=6 update at 0x74370:
		//   074370  CALL sub_1923A0   (card needs registration?)
		//   074375  TEST AL, AL
		//   074377  JZ 0x74383        ← if 0, skip funcID=7
		//   074379  MOV EAX, 0x7      ← funcID=7 (card reg, FREEZES)
		//   07437E  JMP 0x72FC0       ← transition
		//   074383  CALL sub_195430   ← mode selected check
		//   ...
		// We patch the JZ at 0x74377 to unconditional JMP (EB 0A)
		// so funcID=7 is NEVER entered regardless of sub_1923A0's result.
		// This lets sub_1923A0 function normally (card state machine works)
		// while the game stays safe from the freezing funcID=7 path.
		{
			// At VA 0x74377: original is 74 0A (JZ +0x0A), change to EB 0A (JMP +0x0A)
			uint8_t origJZ[] = { 0x74, 0x0A };
			if (memcmp((void*)0x74377, origJZ, 2) == 0) {
				uint8_t newJMP[] = { 0xEB, 0x0A };
				PatchXbeBytes(0x74377, newJMP, 2);
				QOD_LOG("Patched mode select update: JZ->JMP at 0x74377 (skip funcID=7 transition)");
			} else {
				QOD_LOG("WARNING: JZ at 0x74377 not found (bytes=%02X %02X)",
					*(uint8_t*)0x74377, *(uint8_t*)0x74378);
			}
		}

		// --- Force sub_1923A0 (needs-registration check) to return 0 ---
		// sub_1923A0 reads bit 1 of the card object and returns it as the
		// "this card must be registered" flag.  With no Chihiro network
		// server, registration (funcID=7) can never complete and the game
		// reboots.  Presenting every card as already-registered keeps the
		// SELECTOR on its interactive mode-select screen.
		//   Original: A1 E0 42 68 00  mov eax,[6842E0]
		//             8A 00           mov al,[eax]
		//             D0 E8           shr al,1
		//             24 01           and al,1
		//             C3              ret
		//   Patched:  32 C0 C3        xor al,al ; ret
		{
			static const uint8_t origReg[] = { 0xA1, 0xE0, 0x42, 0x68, 0x00, 0x8A, 0x00 };
			if (memcmp((void*)0x1923A0, origReg, sizeof(origReg)) == 0) {
				static const uint8_t retZero[] = { 0x32, 0xC0, 0xC3 };
				PatchXbeBytes(0x1923A0, retZero, sizeof(retZero));
				QOD_LOG("Patched sub_1923A0 -> return 0 (card always treated as registered)");
			} else {
				QOD_LOG("WARNING: sub_1923A0 prologue not found (bytes=%02X %02X %02X)",
					*(uint8_t*)0x1923A0, *(uint8_t*)0x1923A1, *(uint8_t*)0x1923A2);
			}
		}

		// --- Reboot redirect (safety net) ---
		// If C592C4 state machine reaches state 3 before the card reg
		// update fires, it calls sub_6E720 (reboot).  Redirect to our
		// handler that transitions to gameplay instead.
		{
			static const uint8_t kRebootCall[] = { 0xE8, 0xCF, 0xFD, 0xFF, 0xFF };
			if (memcmp((void*)0x6E94C, kRebootCall, 5) == 0) {
				uintptr_t redirAddr = (uintptr_t)&QodRebootRedirect;
				int32_t rel32 = (int32_t)(redirAddr - (0x6E94C + 5));
				uint8_t newCall[5];
				newCall[0] = 0xE8;
				*(int32_t*)&newCall[1] = rel32;
				PatchXbeBytes(0x6E94C, newCall, 5);
				QOD_LOG("Patched reboot CALL at 0x6E94C → QodRebootRedirect (safety net)");
			} else {
				QOD_LOG("WARNING: CALL sub_6E720 at 0x6E94C not found");
			}
		}

		// Step 4: Bypass sub_94F60 — force early return (safety)
		{
			if (*(uint8_t*)0x94F70 == 0x74 && *(uint8_t*)0x94F71 == 0x7A) {
				static const uint8_t kJmp = 0xEB;
				PatchXbeBytes(0x94F70, &kJmp, 1);
				QOD_LOG("Bypassed sub_94F60 at 0x94F70 (JZ→JMP)");
			} else {
				QOD_LOG("WARNING: sub_94F60 JZ at 0x94F70 not found (bytes=%02X %02X)",
					*(uint8_t*)0x94F70, *(uint8_t*)0x94F71);
			}
		}

		g_pfnQuickRebootInterceptor = QodQuickRebootInterceptor;
		QOD_LOG("Card registration patches applied (trampoline approach)");
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
