// ******************************************************************
// *  Cxbx Wangan Midnight Maximum Tune 1/2 patches
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
#include "devices\chihiro\JvsIo.h"
#include "core\kernel\support\Emu.h"

// Detect WMMT by its unique version-check byte pattern.
// Returns the VA of the pattern, or 0 if not found.
static uintptr_t FindWanganVersionCheck(uint32_t imageSize)
{
	static const uint8_t kVerChkPat[] = {
		0x8B,0x44,0x24,0x04, 0x50, 0x68,0x00,0x04,0x00,0x00, 0xE8
	};
	return ScanXbe(kVerChkPat, sizeof(kVerChkPat), imageSize);
}

bool IsWanganXbe(uint32_t imageSize)
{
	return FindWanganVersionCheck(imageSize) != 0;
}

JvsGameType ApplyWanganPatches(uint32_t imageSize)
{
	uintptr_t verChkVA = FindWanganVersionCheck(imageSize);
	if (!verChkVA) return JvsGameType::Generic;

	EmuLog(LOG_LEVEL::INFO, "WanganPatch: applying patches (version-check at 0x%08X)", (unsigned)verChkVA);

	// === Version-check bypass: XOR EAX,EAX; RETN 4 ===
	{
		static const uint8_t kRetSucc4[] = { 0x33,0xC0, 0xC2,0x04,0x00 };
		PatchXbeBytes(verChkVA, kRetSucc4, sizeof(kRetSucc4));
		EmuLog(LOG_LEVEL::INFO, "WanganPatch: version-check patched at VA 0x%08X", (unsigned)verChkVA);
	}

	// === LinkOK — always return 1 ===
	{
		static const uint8_t kLinkOkPat[] = {
			0x85,0xC0, 0x75,0x08, 0xB8,0xFE,0xFF,0xFF,0xFF, 0xC2,0x0C,0x00
		};
		auto linkOkHits = ScanXbeAll(kLinkOkPat, sizeof(kLinkOkPat), imageSize);
		for (auto linkOkVA : linkOkHits) {
			if (linkOkVA >= 5 && ((const uint8_t*)(linkOkVA - 5))[0] == 0xA1) {
				static const uint8_t kMovEax1Ret12[] = { 0xB8,0x01,0x00,0x00,0x00, 0xC2,0x0C,0x00 };
				PatchXbeBytes(linkOkVA - 5, kMovEax1Ret12, sizeof(kMovEax1Ret12));
				EmuLog(LOG_LEVEL::INFO, "WanganPatch: LinkOK patched at VA 0x%08X", (unsigned)(linkOkVA - 5));
			}
		}
		if (linkOkHits.empty()) {
			EmuLog(LOG_LEVEL::WARNING, "WanganPatch: LinkOK pattern not found!");
		}
	}

	// === Link-status inline patches (V322 only) ===
	JvsGameType gameType;
	{
		static const uint8_t kLinkStatPat[] = {
			0xBB,0x01,0x00,0x00,0x00, 0x3B,0xC3, 0x0F,0x85,0xFF,0xFF,0xFF,0xFF, 0xE8
		};
		uintptr_t linkStatVA = ScanXbe(kLinkStatPat, sizeof(kLinkStatPat), imageSize);
		if (linkStatVA) {
			static const uint8_t kMovEax1[] = { 0xB8,0x01,0x00,0x00,0x00 };
			static const uint8_t kMovEax0[] = { 0xB8,0x00,0x00,0x00,0x00 };
			PatchXbeBytes(linkStatVA - 5,  kMovEax1, sizeof(kMovEax1));
			PatchXbeBytes(linkStatVA + 13, kMovEax0, sizeof(kMovEax0));
			EmuLog(LOG_LEVEL::INFO, "WanganPatch: link-status patched at VA 0x%08X + 0x%08X",
				(unsigned)(linkStatVA - 5), (unsigned)(linkStatVA + 13));
		} else {
			EmuLog(LOG_LEVEL::DEBUG, "WanganPatch: link-status pattern not found (expected for V307)");
		}

		gameType = linkStatVA ? JvsGameType::WanganMT2 : JvsGameType::WanganMT1;
		EmuLog(LOG_LEVEL::INFO, "WanganPatch: detected %s",
			gameType == JvsGameType::WanganMT2 ? "Wangan MT2 (V322)" : "Wangan MT1 (V307)");
	}

	// === Type-3 check A: JZ → NOP×6 ===
	{
		static const uint8_t kType3aPat[] = {
			0x0F,0x84, 0xFF,0xFF,0xFF,0xFF,
			0xC7,0x05, 0xFF,0xFF,0xFF,0xFF, 0x05,0x00,0x00,0x00,
			0xEB,0x07
		};
		uintptr_t type3aVA = ScanXbe(kType3aPat, sizeof(kType3aPat), imageSize);
		if (type3aVA) {
			PatchNop(type3aVA, 6);
		}
	}

	// === Type-3 check B: JNZ → NOP×6 ===
	{
		static const uint8_t kType3bPat[] = {
			0x0F,0x85, 0xFF,0xFF,0xFF,0xFF,
			0xC7,0x05, 0xFF,0xFF,0xFF,0xFF, 0x06,0x00,0x00,0x00
		};
		uintptr_t type3bVA = ScanXbe(kType3bPat, sizeof(kType3bPat), imageSize);
		if (type3bVA) {
			PatchNop(type3bVA, 6);
		}
	}

	// === MbRecvPacket / MbSendPacket stub ===
	{
		static const uint8_t kMbFuncPat[] = {
			0x83,0xEC,0x08, 0x8D,0x44,0x24,0x04, 0x50, 0x6A,0x00,
			0xE8,0xFF,0xFF,0xFF,0xFF,
			0x8D,0x0C,0x24, 0x51, 0x6A,0x01, 0xE8
		};
		auto mbHits = ScanXbeAll(kMbFuncPat, sizeof(kMbFuncPat), imageSize);
		if (mbHits.size() >= 2) {
			static const uint8_t kRecvStub[] = { 0x33,0xC0, 0xC2,0x0C,0x00 };
			PatchXbeBytes(mbHits[0], kRecvStub, sizeof(kRecvStub));
			static const uint8_t kSendStub[] = { 0xC2,0x0C,0x00 };
			PatchXbeBytes(mbHits[1], kSendStub, sizeof(kSendStub));
		} else {
			EmuLog(LOG_LEVEL::WARNING, "WanganPatch: MbRecvPacket/MbSendPacket not found!");
		}
	}

	return gameType;
}
