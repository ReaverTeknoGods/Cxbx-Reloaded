// ******************************************************************
// *  Cxbx JVS watchdog suppression — shared across all Chihiro games
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

#include "PatchUtil.h"
#include "ChihiroPatches.h"
#include "devices\chihiro\JvsIo.h"

#include <string>
void* GetXboxSymbolPointer(std::string functionName);

// === JVS watchdog suppression — "Error 11" / "Error 12" ===
//
// Chihiro games contain a per-node timeout watchdog in the JVS receive loop.
// Two counters are incremented every tick inside a do-while:
//   v5[2]  secondary counter — fires Error 11 when >= 60 and a grace timer >= 600
//   v5[3]  primary  counter — fires Error 11 immediately when >= 10
//
// Our emulated JVS doesn't replicate the exact per-node packet accounting
// the game expects, so both counters climb until the error fires.
//
// Fix: NOP the two writeback instructions so neither counter can ever grow:
//   89 50 FC  —  MOV [EAX-4], EDX  (v5[2] secondary writeback, 3 bytes)
//   89 08     —  MOV [EAX],   ECX  (v5[3] primary  writeback, 2 bytes)
//
// Confirmed games:
//   Virtua Cop 3, House of the Dead 3, Ollie King, Ghost Squad,
//   Wangan MT 1/2, Crazy Taxi HR, OutRun 2/SP, Sega Golf Club

void ApplyJvsWatchdogPatch(uint32_t imageSize)
{
	if (GetXboxSymbolPointer("JvsNodeSendPacket") == nullptr)
		return;

	static const uint8_t kJvsWatchdogPattern[] = {
		0x8B, 0x48, 0xF8,              // MOV ECX, [EAX-8]
		0x85, 0xC9,                    // TEST ECX, ECX
		0x7E, 0x0E,                    // JLE +14
		0x8B, 0x50, 0xFC,              // MOV EDX, [EAX-4]   (load v5[2])
		0x8B, 0x08,                    // MOV ECX, [EAX]      (load v5[3])
		0x42,                          // INC EDX
		0x41,                          // INC ECX
		0x89, 0x50, 0xFC,              // MOV [EAX-4], EDX   (v5[2] writeback) ← patch +14
		0x89, 0x08                     // MOV [EAX],   ECX   (v5[3] writeback) ← patch +17
	};
	static const size_t kPatternLen = sizeof(kJvsWatchdogPattern);

	const uintptr_t base = XBE_IMAGE_BASE;
	const uintptr_t end  = base + imageSize - kPatternLen;

	for (uintptr_t addr = base; addr <= end; addr++) {
		if (memcmp((const void*)addr, kJvsWatchdogPattern, kPatternLen) == 0) {
			PatchNop(addr + 14, 3);  // v5[2] writeback
			PatchNop(addr + 17, 2);  // v5[3] writeback

			JvsLog("JVS watchdog patch applied: NOP'd v5[2] at 0x%08X and v5[3] at 0x%08X (pattern at 0x%08X)\n",
				(unsigned)(addr + 14), (unsigned)(addr + 17), (unsigned)addr);
			return;
		}
	}

	JvsLog("JVS watchdog patch: pattern not found in XBE image\n");
}
