// ******************************************************************
// *  Cxbx Crazy Taxi High Roller patches
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

bool IsCrazyTaxiXbe(uint64_t xbeHash)
{
	return xbeHash == 0xF8CB941EC5A7B4B4ULL
		|| xbeHash == 0xf319c176ab55e589ULL
		|| xbeHash == 0xE9EE166CCCBD7847ULL;
}

// CRI Sofdec spin loop helper for Crazy Taxi.
// The game's init code spins on SFD_GetStat(0) waiting for status==3 (complete),
// but never calls CRI ExecServer, so async I/O completions are never processed.
// This helper drives ExecServer between iterations so the I/O actually completes.
static void __cdecl TaxiSfdSpinHelper() {
	typedef int (__cdecl *GetStatFn)(int);
	typedef void (__cdecl *ExecFn)(void);

	GetStatFn getStat = (GetStatFn)0xC7160;
	ExecFn exec2 = (ExecFn)0xCBC90;  // CRI ExecServer priority 2 (filesystem I/O)
	ExecFn exec5 = (ExecFn)0xCBCA0;  // CRI ExecServer priority 5 (file completion)

	for (;;) {
		int stat = getStat(0);
		if (stat == 3) break;
		exec2();
		exec5();
		Sleep(0);
	}
}

void ApplyCrazyTaxiPatches(uint64_t xbeHash, uint32_t imageSize)
{
	printf("CrazyTaxiPatch: xbeHash=0x%016llX, match=%d\n", (unsigned long long)xbeHash, (int)IsCrazyTaxiXbe(xbeHash));

	if (!IsCrazyTaxiXbe(xbeHash)) return;

	// CRI Sofdec spin loop fix: the 15-byte spin at VA 0x3CB74-0x3CB82
	// (PUSH 0; CALL SFD_GetStat; ADD ESP,4; CMP EAX,3; JNZ back)
	// is replaced with a CALL to TaxiSfdSpinHelper which drives CRI
	// ExecServer between iterations so async I/O actually completes.
	{
		DWORD oldProtect;
		uint8_t* p = (uint8_t*)0x3CB74;
		if (VirtualProtect(p, 15, PAGE_EXECUTE_READWRITE, &oldProtect)) {
			p[0] = 0xE8; // CALL rel32
			*(int32_t*)(p + 1) = (int32_t)((uintptr_t)&TaxiSfdSpinHelper - (uintptr_t)(p + 5));
			for (int i = 5; i < 15; i++) p[i] = 0x90; // NOP padding
			VirtualProtect(p, 15, oldProtect, &oldProtect);
			FlushInstructionCache(GetCurrentProcess(), p, 15);
			printf("CrazyTaxiPatch: Patched CRI Sofdec spin @0x3CB74\n");
		}
	}

	printf("CrazyTaxiPatch: done\n");
}
