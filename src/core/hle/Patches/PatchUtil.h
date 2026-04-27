// ******************************************************************
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
#ifndef PATCH_UTIL_H
#define PATCH_UTIL_H

#include <cstdint>
#include <cstring>
#include <vector>
#include <Windows.h>

// XBE image base for Chihiro/Xbox executables
#ifndef XBE_IMAGE_BASE
#define XBE_IMAGE_BASE 0x00010000
#endif

// Write bytes at a guest VA with page-protection flip.
inline void PatchXbeBytes(uintptr_t va, const uint8_t* data, size_t len)
{
	DWORD old = 0;
	VirtualProtect((LPVOID)va, len, PAGE_EXECUTE_READWRITE, &old);
	memcpy((void*)va, data, len);
	VirtualProtect((LPVOID)va, len, old, &old);
	FlushInstructionCache(GetCurrentProcess(), (LPVOID)va, len);
}

// Write NOP bytes over a range of guest virtual addresses.
inline void PatchNop(uintptr_t addr, size_t len)
{
	DWORD oldProtect;
	if (VirtualProtect((void*)addr, len, PAGE_EXECUTE_READWRITE, &oldProtect)) {
		memset((void*)addr, 0x90, len);
		VirtualProtect((void*)addr, len, oldProtect, &oldProtect);
		FlushInstructionCache(GetCurrentProcess(), (LPVOID)addr, len);
	}
}

// Patch XBE address with a JMP rel32 to a host function.
inline void PatchWithJmp(uintptr_t va, const void* target)
{
	uint8_t jmp[5];
	jmp[0] = 0xE9;
	int32_t rel = (int32_t)((uintptr_t)target - (va + 5));
	memcpy(&jmp[1], &rel, 4);
	PatchXbeBytes(va, jmp, 5);
}

// Patch XBE address with a CALL rel32 to a host function, NOP the remaining bytes.
inline void PatchWithCall(uintptr_t va, const void* target, size_t totalLen)
{
	uint8_t buf[32] = {};
	buf[0] = 0xE8;
	int32_t rel = (int32_t)((uintptr_t)target - (va + 5));
	memcpy(&buf[1], &rel, 4);
	for (size_t i = 5; i < totalLen && i < sizeof(buf); i++) buf[i] = 0x90;
	PatchXbeBytes(va, buf, totalLen);
}

// Scan XBE image for first occurrence of pattern (0xFF = wildcard byte).
inline uintptr_t ScanXbe(const uint8_t* pat, size_t patLen, uint32_t imageSize)
{
	const uintptr_t base = XBE_IMAGE_BASE;
	const uintptr_t end  = base + imageSize - patLen;
	for (uintptr_t va = base; va <= end; va++) {
		bool ok = true;
		for (size_t j = 0; j < patLen; j++) {
			if (pat[j] != 0xFF && ((const uint8_t*)va)[j] != pat[j]) { ok = false; break; }
		}
		if (ok) return va;
	}
	return 0;
}

// Scan XBE image returning all hits (up to maxHits).
inline std::vector<uintptr_t> ScanXbeAll(const uint8_t* pat, size_t patLen, uint32_t imageSize, size_t maxHits = 4)
{
	std::vector<uintptr_t> hits;
	const uintptr_t base = XBE_IMAGE_BASE;
	const uintptr_t end  = base + imageSize - patLen;
	for (uintptr_t va = base; va <= end && hits.size() < maxHits; va++) {
		bool ok = true;
		for (size_t j = 0; j < patLen; j++) {
			if (pat[j] != 0xFF && ((const uint8_t*)va)[j] != pat[j]) { ok = false; break; }
		}
		if (ok) hits.push_back(va);
	}
	return hits;
}

#endif // PATCH_UTIL_H
