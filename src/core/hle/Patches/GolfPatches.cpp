// ******************************************************************
// *  Cxbx Virtua Golf / Sega Golf Club patches
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
#include <cstdio>
#include <Windows.h>
#include <intrin.h>

#define GOLF_LOG(fmt, ...) do { \
	FILE* _f = fopen("C:\\temp\\golf_patches.log", "a"); \
	if (_f) { fprintf(_f, fmt "\n", ##__VA_ARGS__); fclose(_f); } \
	printf(fmt "\n", ##__VA_ARGS__); \
} while(0)

// ── Card file I/O ────────────────────────────────────────────────

static const char g_golfCardPath[] = "C:\\arcade\\cxbx\\Sega Golf Club Network Pro Tour 2005 (Rev C)\\card.bin";
static const char g_golfICCDCardPath[] = "C:\\arcade\\cxbx\\Sega Golf Club Network Pro Tour 2005 (Rev C)\\card_iccd.bin";
static volatile bool g_golfCardOpActive = false;  // set during card operations to suppress guardian writes

static bool GolfLoadCard(uint8_t* dest, size_t maxLen) {
	FILE* f = fopen(g_golfCardPath, "rb");
	if (!f) return false;
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (sz <= 0 || (size_t)sz > maxLen) { fclose(f); return false; }
	size_t r = fread(dest, 1, sz, f);
	fclose(f);
	GOLF_LOG("[CARD] Loaded %zu bytes from card.bin", r);
	return r > 0;
}

static bool GolfSaveCard(const uint8_t* src, size_t len) {
	FILE* f = fopen(g_golfCardPath, "wb");
	if (!f) { GOLF_LOG("[CARD] Failed to open card.bin for writing"); return false; }
	size_t w = fwrite(src, 1, len, f);
	fclose(f);
	GOLF_LOG("[CARD] Saved %zu bytes to card.bin", w);
	return w == len;
}

// ── ICCD card reader emulation ──────────────────────────────────
// The ICCD (IC Card Driver) handles physical card reader communication via
// serial port. Since we stub the serial port, the ICCD state machine
// (sub_82870) never completes. We hook it to provide file-based card I/O.
//
// ICCD buffer format (byte_676618, 0x814 bytes):
//   [0]          track count (byte, usually 2)
//   [1-3]        padding
//   [4..1035]    track 0 (1032 bytes) — data track (pre-swapped for slot 0)
//   [1036..2067] track 1 (1032 bytes) — empty track
//
// Each 1032-byte track:
//   [0-3]        track type (DWORD)
//   [4-5]        data length (WORD, 960)
//   [6-477]      card record copy 1 (472 bytes, first 2 = CRC-16-CCITT)
//   [478-949]    card record copy 2 (duplicate)
//   [950-983]    padding
//   [984-986]    magic sentinel (0xBA 0x82 0xE8)
//   [987-999]    padding
//   [1000-1003]  end marker (0xFFFFFFFF)
//   [1004-1007]  padding
//   [1008-1010]  magic sentinel (0x4F 0x82 0xCD)
//   [1011]       protocol version (0x01)
//   [1012-1031]  padding (zeros → passes sub_24BF0 range check)
//
// Card record offsets (within each 472-byte copy, relative to copy start):
//   [0-1]        CRC-16-CCITT (covers bytes [2..471])
//   [2]          version (must be 2)
//   [3]          game code (must match sub_34A40())
//   [10]         flags (bit 3 = expired, bit 4 = something)
//   [12]         remaining uses (0 = renewal needed)
//
// Card file (card_iccd.bin): 2064 bytes = 2 tracks × 1032 bytes (with markers)

static constexpr size_t ICCD_TRACK_SIZE = 1032;     // 0x408
static constexpr size_t ICCD_TRACK_COUNT = 2;
static constexpr size_t ICCD_FILE_SIZE = ICCD_TRACK_COUNT * ICCD_TRACK_SIZE; // 2064
static constexpr size_t ICCD_SEARCH_BUF_SIZE = 0x814; // 2068
static constexpr size_t ICCD_RECORD_SIZE = 472;      // 0x1D8
static constexpr size_t ICCD_RECORD_USES_OFFSET = 12; // remaining uses byte
static constexpr uint8_t ICCD_MAX_USES = 250;         // unlimited = always reset to max

// CRC-16-CCITT (polynomial 0x1021) — matches the game's sub_32DD0/sub_32D20.
// Used to checksum each 472-byte card record copy (bytes [2..471]).
// IMPORTANT: The game uses a 32-bit accumulator internally (int, not uint16_t).
// We must match this exactly — using uint16_t gives wrong results because
// intermediate values exceed 16 bits and the truncation changes subsequent iterations.
static int s_crc16Table[257];
static bool s_crc16TableInit = false;

// Table generation — exact match of game's sub_32D20.
// Each entry is CRC-CCITT for a single byte value, using polynomial 0x1021.
static void GolfICCDInitCRC16Table() {
	if (s_crc16TableInit) return;
	for (int i = 0; i <= 256; i++) {
		// Game: v1 = (_WORD)i << 9; if (i & 0x80) v1 ^= 0x1021;
		// This is equivalent to: crc = (i << 8), then first shift with XOR.
		int16_t crc = (int16_t)((uint16_t)i << 8);
		// 8 rounds of: if MSB set, shift left and XOR with polynomial
		for (int j = 0; j < 8; j++) {
			if (crc < 0)  // test bit 15 (sign bit of int16_t)
				crc = (int16_t)((crc << 1) ^ 0x1021);
			else
				crc = (int16_t)(crc << 1);
		}
		s_crc16Table[i] = (uint16_t)crc;
	}
	s_crc16TableInit = true;
}

// Compute CRC-16-CCITT — exact match of game's sub_32DD0(data, len).
// Uses 32-bit accumulator (int) like the game. Returns low 16 bits.
static uint16_t GolfICCDComputeCRC16(const uint8_t* data, int len) {
	GolfICCDInitCRC16Table();
	int result = 0;
	for (int i = 0; i < len; i++) {
		// BYTE1(result) = bits 8-15 = (result >> 8) & 0xFF
		result = s_crc16Table[data[i] ^ ((result >> 8) & 0xFF)] ^ (result << 8);
	}
	return (uint16_t)result;
}

// Fix up record fields in a 1032-byte track buffer for emulation.
// Patches both record copies (at track+6 and track+478):
//   - record[2] = 2 (version/status byte — required by sub_24D20 for type 2)
//   - record[12] = 250 (remaining uses — unlimited saves in emulation)
// Recalculates CRC-16-CCITT after modifications.
static void GolfICCDFixupRecord(uint8_t* track) {
	for (int copy = 0; copy < 2; copy++) {
		uint8_t* record = track + 6 + copy * ICCD_RECORD_SIZE;
		// Check if this copy has any data (non-zero past CRC)
		bool hasData = false;
		for (int i = 2; i < (int)ICCD_RECORD_SIZE; i++) {
			if (record[i]) { hasData = true; break; }
		}
		if (!hasData) continue;
		// Log original values
		uint16_t oldCRC = *(uint16_t*)record;
		uint16_t verifyCRC = GolfICCDComputeCRC16(record + 2, ICCD_RECORD_SIZE - 2);
		GOLF_LOG("[ICCD] Copy %d: oldCRC=%04X computed=%04X uses=%u ver=%u gamecode=%u flags=%02X",
			copy, oldCRC, verifyCRC, record[ICCD_RECORD_USES_OFFSET], record[2], record[3],
			record[10]);
		// Ensure version/status byte is 2 (required by sub_24D20 for valid type)
		record[2] = 2;
		// Set remaining uses to max
		record[ICCD_RECORD_USES_OFFSET] = ICCD_MAX_USES;
		// Clear expired flag (bit 3 of flags at record[10])
		record[10] &= ~0x08;
		// Recalculate CRC-16 over bytes [2..471] (470 bytes)
		uint16_t newCRC = GolfICCDComputeCRC16(record + 2, ICCD_RECORD_SIZE - 2);
		*(uint16_t*)record = newCRC;
		GOLF_LOG("[ICCD] Fixup copy %d: ver=2 uses=%u flags=%02X new CRC=%04X",
			copy, ICCD_MAX_USES, record[10], newCRC);
	}
}

// Add magic markers to a 1032-byte track buffer so it passes validation
// (sub_24070 and sub_24BF0 in the game's card middleware)
static void GolfICCDAddTrackMarkers(uint8_t* track) {
	// Magic sentinel 1 at offset 984 (from off_2380D4)
	track[984] = 0xBA;
	track[985] = 0x82;
	track[986] = 0xE8;
	// End marker at offset 1000 (-1 / 0xFFFFFFFF)
	track[1000] = 0xFF;
	track[1001] = 0xFF;
	track[1002] = 0xFF;
	track[1003] = 0xFF;
	// Magic sentinel 2 at offset 1008 (from off_2380D0)
	track[1008] = 0x4F;
	track[1009] = 0x82;
	track[1010] = 0xCD;
	// Protocol version byte at offset 1011
	track[1011] = 0x01;
	// Bytes 1012-1015 left as zero → sub_24A60 constructs 0x00000000
	// which is < 0x80BA, so sub_24BF0 returns 0 (valid)
}

// Construct a valid empty track — all zeros.
// Real ICCD empty tracks are all-zeros. The original sub_24D20/sub_24C40
// recognizes this as empty (type 1) without needing markers or headers.
static void GolfICCDMakeEmptyTrack(uint8_t* track) {
	memset(track, 0, ICCD_TRACK_SIZE);
}

// Load ICCD card file into the search buffer (byte_676618)
static bool GolfICCDLoadCard(uint8_t* searchBuf) {
	memset(searchBuf, 0, ICCD_SEARCH_BUF_SIZE);

	uint8_t fileData[ICCD_FILE_SIZE];
	FILE* f = fopen(g_golfICCDCardPath, "rb");
	if (f) {
		fseek(f, 0, SEEK_END);
		long sz = ftell(f);
		fseek(f, 0, SEEK_SET);
		if (sz == ICCD_FILE_SIZE) {
			fread(fileData, 1, ICCD_FILE_SIZE, f);
			fclose(f);
			// Reset remaining uses to max (unlimited saves in emulation).
			// Must recalculate CRC-16-CCITT after modifying the record.
			GolfICCDFixupRecord(fileData);
			// Return 2 tracks: track 0 = data, track 1 = empty.
			// On real hardware sub_24E50 swaps tracks (data from track 1 → slot 0).
			// Since we patched sub_24DB0 to skip the swap, we pre-swap here:
			// put data directly at track 0 so the game finds it in slot 0.
			searchBuf[0] = 2;
			memcpy(searchBuf + 4, fileData, ICCD_TRACK_SIZE); // track 0 = data
			GolfICCDMakeEmptyTrack(searchBuf + 4 + ICCD_TRACK_SIZE); // track 1 = empty
			GOLF_LOG("[ICCD] Loaded card_iccd.bin (%ld bytes), track0 byte6=%02X",
				sz, fileData[6]);
			return true;
		}
		fclose(f);
		GOLF_LOG("[ICCD] card_iccd.bin wrong size (%ld, expected %zu)", sz, ICCD_FILE_SIZE);
	}

	// No valid file — return 2 empty tracks.
	GOLF_LOG("[ICCD] No card file — constructing blank card (2 empty tracks)");
	searchBuf[0] = 2;
	GolfICCDMakeEmptyTrack(searchBuf + 4);
	GolfICCDMakeEmptyTrack(searchBuf + 4 + ICCD_TRACK_SIZE);
	return true;
}

// Save card data to ICCD card file
// writeData points to byte_677148 (write buffer: [0]=count, [4..]=track data)
static bool GolfICCDSaveCard(const uint8_t* writeData) {
	uint8_t fileData[ICCD_FILE_SIZE];

	// Load existing file or create blank
	FILE* f = fopen(g_golfICCDCardPath, "rb");
	if (f) {
		fseek(f, 0, SEEK_END);
		long sz = ftell(f);
		fseek(f, 0, SEEK_SET);
		if (sz == ICCD_FILE_SIZE) {
			fread(fileData, 1, ICCD_FILE_SIZE, f);
		} else {
			// Wrong size — start fresh
			GolfICCDMakeEmptyTrack(fileData);
			GolfICCDMakeEmptyTrack(fileData + ICCD_TRACK_SIZE);
		}
		fclose(f);
	} else {
		// No file — create blank
		GolfICCDMakeEmptyTrack(fileData);
		GolfICCDMakeEmptyTrack(fileData + ICCD_TRACK_SIZE);
	}

	// Write buffer: [0] = track count (usually 1), [4..1035] = track data
	uint8_t trackCount = writeData[0];
	GOLF_LOG("[ICCD] WRITE: trackCount=%u", trackCount);

	// Store the game's data track at track 0 in the file.
	// sub_24DB0 expects data at track 0 for single-track reads.
	if (trackCount >= 1) {
		memcpy(fileData, writeData + 4, ICCD_TRACK_SIZE);
		// Add magic markers (write data from sub_240E0 doesn't include them)
		GolfICCDAddTrackMarkers(fileData);
	}

	// Track 1 = empty backup
	GolfICCDMakeEmptyTrack(fileData + ICCD_TRACK_SIZE);

	// Save
	f = fopen(g_golfICCDCardPath, "wb");
	if (!f) {
		GOLF_LOG("[ICCD] Failed to open card_iccd.bin for writing");
		return false;
	}
	size_t w = fwrite(fileData, 1, ICCD_FILE_SIZE, f);
	fclose(f);
	GOLF_LOG("[ICCD] Saved card_iccd.bin (%zu bytes)", w);
	return w == ICCD_FILE_SIZE;
}

// Track validator hook — replaces sub_24D20
// The original sub_24D20 (__thiscall, ECX = track pointer) calls sub_24C40
// which uses __usercall(ECX, EBX). For unknown reasons, sub_24C40 fails to
// recognize empty tracks at runtime (possibly a register convention issue in the
// compiled code's interaction with our hooked ICCD buffer). This hook reimplements
// the validation logic entirely in C to avoid the issue.
//
// Returns: 1 = empty, 2 = valid data, 3 = data (no uses left), 4 = expired, 5 = invalid
static int __fastcall GolfTrackValidatorHook(uint8_t* track, void* /*edx*/) {
	GOLF_LOG("[CARD-VAL] called, track=%p", track);
	if (!track) return 5;

	// All-zero track = genuine empty track (no markers, no data).
	// Real ICCD empty cards are all zeros. Return type 1 (empty).
	bool allZero = true;
	for (int i = 0; i < (int)ICCD_TRACK_SIZE; i++) {
		if (track[i]) { allZero = false; break; }
	}
	if (allZero) {
		GOLF_LOG("[CARD-VAL] all-zero track → type 1 (empty)");
		return 1;
	}

	// Magic marker check (sub_24070) — hardcoded expected bytes
	static const uint8_t kMarker1[] = { 0xBA, 0x82, 0xE8 }; // track[984..986]
	static const uint8_t kMarker2[] = { 0x4F, 0x82, 0xCD }; // track[1008..1010]
	if (*(uint32_t*)(track + 1000) != 0xFFFFFFFF ||
		memcmp(track + 984, kMarker1, 3) != 0 ||
		memcmp(track + 1008, kMarker2, 3) != 0 ||
		track[1011] != 1) {
		GOLF_LOG("[CARD-VAL] marker check failed → type 5");
		return 5;
	}

	// Has markers — check if data area is populated
	bool isEmpty = true;
	for (int i = 6; i < 965; i++) {
		if (track[i]) { isEmpty = false; break; }
	}
	if (isEmpty) {
		GOLF_LOG("[CARD-VAL] markers but no data → type 1 (empty)");
		return 1;
	}

	// Non-empty track with valid markers → valid data track.
	GOLF_LOG("[CARD-VAL] data track (byte6-9: %02X %02X %02X %02X) → type 2",
		track[6], track[7], track[8], track[9]);
	return 2;
}

// Record parser hook — replaces sub_24C40
// Original: int __usercall sub_24C40@<eax>(int a1@<ecx>, _BYTE *a2@<ebx>)
// Bypasses the game's CRC verification which has a signed/unsigned comparison bug:
// the game stores CRC as int16 in a _WORD array, but compares with __int16 truncation
// of the int return from sub_24050. When the CRC has bit 15 set (>= 0x8000), the
// sign-extended int16 != zero-extended _WORD, causing validation to fail.
// Our implementation does a proper uint16_t comparison.
static int __cdecl GolfRecordParserImpl(uint8_t* track, uint8_t* output) {
	memset(output, 0, ICCD_RECORD_SIZE); // 472 bytes

	// sub_24BF0 check — validate range from bytes 1012-1015
	// sub_24A60 constructs DWORD from track[1012..1015], must be < 0x80BA
	uint32_t rangeVal = ((uint32_t)track[1012] << 24) | ((uint32_t)track[1013] << 16) |
	                    ((uint32_t)track[1014] << 8) | track[1015];
	if (rangeVal >= 0x80BA) {
		GOLF_LOG("[REC-PARSE] sub_24BF0 range check FAIL: val=0x%08X", rangeVal);
		return 6;
	}

	// sub_24070 marker check
	static const uint8_t kM1[] = { 0xBA, 0x82, 0xE8 };
	static const uint8_t kM2[] = { 0x4F, 0x82, 0xCD };
	if (*(uint32_t*)(track + 1000) != 0xFFFFFFFF ||
		memcmp(track + 984, kM1, 3) != 0 ||
		memcmp(track + 1008, kM2, 3) != 0 ||
		track[1011] != 1) {
		GOLF_LOG("[REC-PARSE] marker check FAIL");
		return 5;
	}

	// sub_24020 empty check — all bytes at track[6..964] zero and track[965]==0xFF?
	bool allZero = true;
	for (int i = 6; i < 965; i++) {
		if (track[i]) { allZero = false; break; }
	}
	if (allZero && track[965] == 0xFF) {
		GOLF_LOG("[REC-PARSE] empty track");
		return 1;
	}
	if (allZero) {
		GOLF_LOG("[REC-PARSE] empty (no 0xFF sentinel)");
		return 1;
	}

	// Copy both record copies (944 bytes) from track+6
	uint8_t recBuf[944];
	memcpy(recBuf, track + 6, 944);

	// CRC check — use uint16_t comparison (fixes the sign bug)
	GolfICCDInitCRC16Table();
	uint16_t storedCRC0 = *(uint16_t*)recBuf;
	uint16_t computedCRC0 = GolfICCDComputeCRC16(recBuf + 2, ICCD_RECORD_SIZE - 2);
	GOLF_LOG("[REC-PARSE] copy0: stored=%04X computed=%04X", storedCRC0, computedCRC0);

	uint8_t* src = nullptr;
	if (storedCRC0 == computedCRC0) {
		src = recBuf;
	} else {
		uint16_t storedCRC1 = *(uint16_t*)(recBuf + ICCD_RECORD_SIZE);
		uint16_t computedCRC1 = GolfICCDComputeCRC16(recBuf + ICCD_RECORD_SIZE + 2, ICCD_RECORD_SIZE - 2);
		GOLF_LOG("[REC-PARSE] copy1: stored=%04X computed=%04X", storedCRC1, computedCRC1);
		if (storedCRC1 == computedCRC1) {
			src = recBuf + ICCD_RECORD_SIZE;
		}
	}

	if (!src) {
		GOLF_LOG("[REC-PARSE] CRC FAIL for both copies");
		return 5;
	}

	memcpy(output, src, ICCD_RECORD_SIZE);
	GOLF_LOG("[REC-PARSE] OK: CRC=%04X ver=%u gamecode=0x%02X uses=%u flags=0x%02X",
		*(uint16_t*)output, output[2], output[3], output[12], output[10]);

	// Check flags — original returns 5 if (output[10] & 0x10)
	if (output[10] & 0x10)
		return 5;
	return 2;
}

// Naked thunk for sub_24C40 __usercall(ecx=track, ebx=output)
__declspec(naked) static void GolfRecordParserThunk() {
	__asm {
		push ebx       // arg2: output buffer (from ebx)
		push ecx       // arg1: track pointer (from ecx)
		call GolfRecordParserImpl
		add esp, 8
		ret
	}
}

// ICCD state machine hook — replaces sub_82870
// Handles RESET, SEARCH, and WRITE operations using file I/O
// instead of serial communication with the physical card reader.
static void __cdecl GolfICCDStateMachineHook() {
	volatile uint32_t* pState     = (volatile uint32_t*)0x00677970;
	volatile int32_t*  pResult    = (volatile int32_t*)0x00677974;

	// Periodically log the global slot array and menu state
	static int sMonitorCounter = 0;
	if (++sMonitorCounter % 300 == 1) {
		uint32_t* gs = (uint32_t*)0x004E4ED8;
		uint32_t menuState = *(volatile uint32_t*)0x005E99B8;
		uint32_t btnMask   = *(volatile uint32_t*)0x005E99B4;
		uint32_t baseMask  = *(volatile uint32_t*)0x005E99B0;
		// Also read parsed record data from slot 0
		// Slot layout: DWORD[0]=present, DWORD[1]=type, DWORD[2..3]=sub_24BC0 data,
		// then at byte offset 16 (DWORD[4]): 472-byte parsed record from sub_24C40
		uint8_t* slotRec = (uint8_t*)(gs + 4); // record at slot+16 bytes = DWORD[4]
		GOLF_LOG("[MENU-MON] menuState=%u baseMask=0x%X btnMask=0x%X slots: s0=[pres=%u type=%u] s1=[pres=%u type=%u]",
			menuState, baseMask, btnMask, gs[0], gs[1], gs[123], gs[124]);
		if (gs[0] > 0) {
			GOLF_LOG("[MENU-MON] slot0 record: CRC=%02X%02X ver=%u gamecode=0x%02X uses=%u flags=0x%02X  bytes8-13: %02X %02X %02X %02X %02X %02X",
				slotRec[1], slotRec[0], slotRec[2], slotRec[3], slotRec[12], slotRec[10],
				slotRec[8], slotRec[9], slotRec[10], slotRec[11], slotRec[12], slotRec[13]);
		}
	}

	volatile int32_t*  pAvailable = (volatile int32_t*)0x00677968;
	volatile int32_t*  pDisabled  = (volatile int32_t*)0x0067796C;

	// Respect the disabled flag (same as original)
	if (*pDisabled) return;

	uint32_t state = *pState;
	static uint32_t sLastLoggedState = 0xFFFFFFFF;
	if (state != sLastLoggedState) {
		GOLF_LOG("[ICCD] Hook called: state=%u result=%d avail=%d", state, *pResult, *pAvailable);
		sLastLoggedState = state;
	}

	switch (state) {
	case 0: // RESET INIT
	case 1: // RESET CTRL
		// Card reader is always "available" in emulation
		*pAvailable = 1;
		*pResult = 1;
		*pState = 6; // done
		GOLF_LOG("[ICCD] RESET complete (state was %u)", state);
		break;

	case 2: // SEARCH INIT
	case 3: // SEARCH CTRL
	{
		// Load card data into the search result buffer (byte_676618)
		uint8_t* searchBuf = (uint8_t*)0x00676618;
		bool loaded = GolfICCDLoadCard(searchBuf);
		*pResult = loaded ? 1 : -100;
		*pState = 6; // done
		GOLF_LOG("[ICCD] SEARCH complete: result=%d trackCount=%u", *pResult, searchBuf[0]);
		// Track 0 is the data track (pre-swapped), Track 1 is empty
		const uint8_t* t0 = searchBuf + 4;
		GOLF_LOG("[ICCD] Track0 hdr: %02X %02X %02X %02X %02X %02X",
			t0[0], t0[1], t0[2], t0[3], t0[4], t0[5]);
		GOLF_LOG("[ICCD] Track0 @984: %02X %02X %02X  @1000: %02X %02X %02X %02X  @1008: %02X %02X %02X  @1011: %02X",
			t0[984], t0[985], t0[986],
			t0[1000], t0[1001], t0[1002], t0[1003],
			t0[1008], t0[1009], t0[1010], t0[1011]);
		GOLF_LOG("[ICCD] Track0 byte6: %02X  rec[12](uses): %u",
			t0[6], t0[6 + ICCD_RECORD_USES_OFFSET]);
		break;
	}

	case 4: // WRITE INIT
	case 5: // WRITE CTRL
	{
		// Save card data from the write buffer (byte_677148)
		const uint8_t* writeData = (const uint8_t*)0x00677148;
		// Log what the game is writing
		const uint8_t* wRec = writeData + 4 + 6; // track data offset 4, record offset 6
		GOLF_LOG("[ICCD] WRITE: trackCount=%u rec: CRC=%02X%02X ver=%u gamecode=0x%02X uses=%u flags=0x%02X",
			writeData[0], wRec[1], wRec[0], wRec[2], wRec[3], wRec[12], wRec[10]);
		bool saved = GolfICCDSaveCard(writeData);
		*pResult = saved ? 1 : -100;
		*pState = 6; // done
		GOLF_LOG("[ICCD] WRITE complete: result=%d", *pResult);
		break;
	}

	default:
		// State 6+ = done/idle — do nothing
		// The original resets timeout here, but we don't need timeouts
		break;
	}
}

// ── Hook functions ────────────────────────────────────────────────

// JVS node function hooks — these forward JVS commands to the emulated I/O board
// instead of going through MbSendPacket/MbRecvPacket (which are stubbed).
// The game's library has JvsNodeSendPacket/JvsNodeReceivePacket statically linked,
// so the symbol-based HLE patches can't find them. We pattern-scan and hook directly.

static int __stdcall GolfJvsNodeSendPacketHook(uint8_t* Buffer, uint32_t Length, uint32_t a3) {
	if (!g_pJvsIo || !Buffer || Length < 3) return 0;
	unsigned packetCount = Buffer[1];
	GOLF_LOG("JvsNodeSend: len=%u pkts=%u buf=[%02X %02X %02X %02X %02X %02X %02X %02X]",
		Length, packetCount,
		Length > 0 ? Buffer[0] : 0, Length > 1 ? Buffer[1] : 0,
		Length > 2 ? Buffer[2] : 0, Length > 3 ? Buffer[3] : 0,
		Length > 4 ? Buffer[4] : 0, Length > 5 ? Buffer[5] : 0,
		Length > 6 ? Buffer[6] : 0, Length > 7 ? Buffer[7] : 0);
	uint8_t* packetPtr = &Buffer[2];
	for (unsigned i = 0; i < packetCount; i++) {
		packetPtr++; // skip separator byte (0x00)
		size_t bytes = g_pJvsIo->SendPacket(packetPtr);
		packetPtr += bytes;
	}
	return 0;
}

static int __stdcall GolfJvsNodeRecvPacketHook(uint8_t* Buffer, uint32_t* Length, uint32_t a3) {
	if (!g_pJvsIo || !Buffer || !Length) return 0;
	uint8_t DeviceId = g_pJvsIo->GetDeviceId();
	uint16_t payloadSize = (uint16_t)g_pJvsIo->ReceivePacket(&Buffer[6]);
	if (payloadSize > 0) {
		Buffer[0] = 0; // empty header
		Buffer[1] = 1; // number of packets
		Buffer[2] = DeviceId;
		Buffer[3] = 0; // unused
		*Length = payloadSize + 6;
		*((uint16_t*)&Buffer[4]) = payloadSize;
		GOLF_LOG("JvsNodeRecv: devId=%u payloadSize=%u", DeviceId, payloadSize);
	} else {
		GOLF_LOG("JvsNodeRecv: no payload (size=0)");
	}
	return 0;
}

// Fake device struct for state 0 handler — bytes at +0x14 and +0x15 must be non-zero
static uint8_t s_fakeDeviceStruct[0x20] = {};

static constexpr uintptr_t kSgc1JvsNodeRecvVA = 0x0022E700;
static constexpr uintptr_t kSgc1JvsNodeSendVA = 0x0022E7E0;
static constexpr uintptr_t kSgc1InputCacheBaseVA = 0x0098652C;
static constexpr uintptr_t kSgc1InputDeviceCountVA = 0x00986129;
static constexpr uintptr_t kGolfRawSystemByteVA = 0x006779D8;
static constexpr uintptr_t kGolfRawAuxButtonsVA = 0x006779F8;
static constexpr uintptr_t kGolfRawJvsRecordBaseVA = 0x006779DC;
static constexpr uintptr_t kGolfRawInputStruct0VA = 0x00677A6C;
static constexpr uintptr_t kGolfRawInputStruct0DmaVA = 0x006784C4;
static constexpr uintptr_t kGolfRawInputConsumer0VA = 0x00678D38;

struct GolfSharedInputState {
	uint32_t control = 0;
	uint32_t coinState = 0;
	uint8_t analogBytes[4] = {};
	uint8_t switchBytes[5] = {};
	uint16_t analogValues[4] = {};
};

static uint16_t GolfExpand8To16(uint8_t value)
{
	return (static_cast<uint16_t>(value) << 8) | value;
}

static bool GolfReadSharedInputState(GolfSharedInputState* state)
{
	extern void* g_jvs_view_ptr;
	if (!g_jvs_view_ptr || !state) {
		return false;
	}

	uint8_t* shared = static_cast<uint8_t*>(g_jvs_view_ptr);
	state->control = *reinterpret_cast<volatile uint32_t*>(shared + 8);
	state->coinState = *reinterpret_cast<volatile uint32_t*>(shared + 32);
	state->analogBytes[0] = *(reinterpret_cast<volatile uint8_t*>(shared + 12));
	state->analogBytes[1] = *(reinterpret_cast<volatile uint8_t*>(shared + 13));
	state->analogBytes[2] = *(reinterpret_cast<volatile uint8_t*>(shared + 14));
	state->analogBytes[3] = *(reinterpret_cast<volatile uint8_t*>(shared + 15));

	jvs_switch_system_inputs_t systemInputs = {};
	jvs_switch_player_inputs_t playerInputs[2] = {};
	systemInputs.test = (state->control & 0x01) != 0;

	playerInputs[0].start = (state->control & 0x02) != 0;
	playerInputs[0].service = (state->control & 0x40) != 0;
	playerInputs[0].up = (state->control & 0x800) != 0;
	playerInputs[0].down = (state->control & 0x2000) != 0;
	playerInputs[0].left = (state->control & 0x400) != 0;
	playerInputs[0].right = (state->control & 0x1000) != 0;
	playerInputs[0].button[0] = (state->control & 0x04) != 0;
	playerInputs[0].button[1] = (state->control & 0x20) != 0;
	playerInputs[0].button[2] = (state->control & 0x200) != 0;
	playerInputs[0].button[3] = (state->control & 0x80000) != 0;
	playerInputs[0].button[4] = (state->control & 0x100000) != 0;
	playerInputs[0].button[5] = (state->control & 0x200000) != 0;
	playerInputs[0].button[8] = (state->control & 0x400000) != 0;
	playerInputs[0].button[9] = (state->control & 0x8000000) != 0;

	playerInputs[1].start = (state->control & 0x08) != 0;
	playerInputs[1].service = (state->control & 0x100) != 0;
	playerInputs[1].up = (state->control & 0x10000) != 0;
	playerInputs[1].down = (state->control & 0x40000) != 0;
	playerInputs[1].left = (state->control & 0x8000) != 0;
	playerInputs[1].right = (state->control & 0x20000) != 0;
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

	state->analogValues[0] = GolfExpand8To16(state->analogBytes[0]);
	state->analogValues[1] = GolfExpand8To16(state->analogBytes[1]);
	state->analogValues[2] = GolfExpand8To16(state->analogBytes[2]);
	state->analogValues[3] = GolfExpand8To16(state->analogBytes[3]);
	return true;
}

static void GolfMirrorRawInputPathFromSharedMemory(const GolfSharedInputState& state)
{
	volatile uint8_t* rawRecord0 = reinterpret_cast<volatile uint8_t*>(kGolfRawJvsRecordBaseVA);
	volatile uint8_t* rawRecord1 = rawRecord0 + 36;
	volatile uint16_t* rawAnalogWords0 = reinterpret_cast<volatile uint16_t*>(kGolfRawJvsRecordBaseVA + 10);
	volatile uint16_t* rawAnalogWords1 = reinterpret_cast<volatile uint16_t*>(kGolfRawJvsRecordBaseVA + 46);
	uint8_t rawSystemByte = 0;

	if ((state.control & 0x01) != 0) {
		rawSystemByte |= 0x40;
	}
	if ((state.control & 0x40) != 0) {
		rawSystemByte |= 0x80;
	}
	*(volatile uint8_t*)kGolfRawSystemByteVA = rawSystemByte;
	*(volatile uint32_t*)kGolfRawAuxButtonsVA = state.coinState ? 0x00000002u : 0u;

	rawRecord0[4] = state.switchBytes[0];
	rawRecord0[5] = state.switchBytes[1];
	rawRecord0[6] = state.switchBytes[2];
	rawRecord0[7] = state.switchBytes[3];
	rawRecord0[8] = state.switchBytes[4];
	rawRecord1[4] = 0;
	rawRecord1[5] = 0;
	rawRecord1[6] = 0;
	rawRecord1[7] = 0;
	rawRecord1[8] = 0;

	// Write analog to raw record — this is the ONLY delivery mechanism.
	// The JVS protocol does NOT populate this record.
	// Swing goes to ch0, ch2, ch5 to cover all possible game read channels.
	rawAnalogWords0[0] = state.analogValues[0];
	rawAnalogWords0[1] = state.analogValues[1];
	rawAnalogWords0[2] = state.analogValues[0];
	rawAnalogWords0[3] = state.analogValues[2];
	rawAnalogWords0[4] = state.analogValues[3];
	rawAnalogWords0[5] = state.analogValues[0];
	rawAnalogWords0[6] = 0;
	rawAnalogWords0[7] = 0;

	// Debug: verify write-then-readback
	static uint16_t s_lastWritten = 0;
	if (state.analogValues[0] != s_lastWritten) {
		uint16_t readback = rawAnalogWords0[0];
		GOLF_LOG("GolfPatch: [RAW-WRITE] wrote=%04X readback=%04X addr=0x%08X",
			state.analogValues[0], readback,
			static_cast<unsigned>(kGolfRawJvsRecordBaseVA + 10));
		s_lastWritten = state.analogValues[0];
	}
	for (size_t index = 0; index < 8; ++index) {
		rawAnalogWords1[index] = 0;
	}

	// Raw record writes are sufficient — sub_82EE0 reads from raw records
	// every frame and propagates to source structs. sub_83DE0 copies source
	// to consumer. Direct struct writes are no longer needed (and were being
	// overwritten by the pipeline anyway).
}

static void GolfMirrorNativeInputCacheFromSharedMemory()
{
	GolfSharedInputState state = {};
	if (!GolfReadSharedInputState(&state)) {
		return;
	}

	GolfMirrorRawInputPathFromSharedMemory(state);

	volatile uint8_t* deviceCountPtr = reinterpret_cast<volatile uint8_t*>(kSgc1InputDeviceCountVA);
	uint8_t deviceCount = *deviceCountPtr;
	static uint8_t s_lastDeviceCount = 0xFF;
	static uint64_t s_lastSwitchState = ~0ull;
	static uint64_t s_lastAnalogState = ~0ull;
	static bool s_loggedLayout = false;

	if (deviceCount == 0) {
		if (s_lastDeviceCount != 0) {
			GOLF_LOG("GolfPatch: [NATIVE-CACHE] no devices configured yet");
			s_lastDeviceCount = 0;
		}
		return;
	}

	for (uint8_t deviceIndex = 1; deviceIndex <= deviceCount; ++deviceIndex) {
		volatile uint8_t* device = reinterpret_cast<volatile uint8_t*>(kSgc1InputCacheBaseVA + 224 * deviceIndex);
		uint32_t switchDataVA = *reinterpret_cast<volatile uint32_t*>(device + 172);
		uint32_t analogDataVA = *reinterpret_cast<volatile uint32_t*>(device + 180);
		volatile uint8_t* switchData = reinterpret_cast<volatile uint8_t*>(
			static_cast<uintptr_t>(switchDataVA));
		volatile uint8_t* analogData = reinterpret_cast<volatile uint8_t*>(
			static_cast<uintptr_t>(analogDataVA));

		if (!switchData) {
			switchData = device + 172;
		}
		if (!analogData) {
			analogData = device + 180;
		}

		if (device[204] == 0) {
			device[204] = 2;
		}
		if (device[205] == 0) {
			device[205] = 16;
		}
		if (device[207] == 0) {
			device[207] = 4;
		}
		if (device[208] == 0) {
			device[208] = 16;
		}

		// Write analog to native cache. Both switchDataVA and analogDataVA
		// resolve to 0x0098652C (device[0]) — this IS the game's analog read
		// location. Write swing to channel 2 onward (bytes 4-7) and keep
		// bytes 0-3 zeroed to minimize phantom switch presses.
		analogData[0] = 0;
		analogData[1] = 0;
		analogData[2] = 0;
		analogData[3] = 0;
		analogData[4] = static_cast<uint8_t>(state.analogValues[0] >> 8);
		analogData[5] = static_cast<uint8_t>(state.analogValues[0] & 0xFF);
		analogData[6] = static_cast<uint8_t>(state.analogValues[0] >> 8);
		analogData[7] = static_cast<uint8_t>(state.analogValues[0] & 0xFF);

		if (!s_loggedLayout && deviceIndex == 1) {
			GOLF_LOG("GolfPatch: [NATIVE-CACHE] dev1 switchPtr=0x%08X analogPtr=0x%08X switchCaps=%u/%u analogCaps=%u/%u",
				static_cast<unsigned>(reinterpret_cast<uintptr_t>(switchData)),
				static_cast<unsigned>(reinterpret_cast<uintptr_t>(analogData)),
				static_cast<unsigned>(device[204]),
				static_cast<unsigned>(device[205]),
				static_cast<unsigned>(device[207]),
				static_cast<unsigned>(device[208]));
			s_loggedLayout = true;
		}
	}

	uint64_t switchState = static_cast<uint64_t>(state.switchBytes[0])
		| (static_cast<uint64_t>(state.switchBytes[1]) << 8)
		| (static_cast<uint64_t>(state.switchBytes[2]) << 16)
		| (static_cast<uint64_t>(state.switchBytes[3]) << 24)
		| (static_cast<uint64_t>(state.switchBytes[4]) << 32);
	uint64_t analogState = static_cast<uint64_t>(state.analogValues[0])
		| (static_cast<uint64_t>(state.analogValues[1]) << 16)
		| (static_cast<uint64_t>(state.analogValues[2]) << 32)
		| (static_cast<uint64_t>(state.analogValues[3]) << 48);
	if (deviceCount != s_lastDeviceCount || switchState != s_lastSwitchState || analogState != s_lastAnalogState) {
		GOLF_LOG("GolfPatch: [NATIVE-CACHE] devices=%u sw=%02X %02X %02X %02X %02X a=%04X %04X %04X %04X",
			static_cast<unsigned>(deviceCount),
			state.switchBytes[0], state.switchBytes[1], state.switchBytes[2], state.switchBytes[3], state.switchBytes[4],
			state.analogValues[0], state.analogValues[1], state.analogValues[2], state.analogValues[3]);
		s_lastDeviceCount = deviceCount;
		s_lastSwitchState = switchState;
		s_lastAnalogState = analogState;
	}
}

// sub_F3910: hardware error check, called with arg=6 and arg=3 in state 0.
// Returns 1 if the indexed table entry is NULL (system not ready).
// Patch to always return 0 (system is ready).
static int __cdecl GolfErrorCheckHook(int arg) {
	return 0;
}

// === Touch panel emulation ===
// Instead of writing byte_718D64 externally, we hook the game's own
// read function (sub_C5F20) so we ARE the touch panel. sub_C6D20 is
// used as a once-per-frame tick to sample the mouse and compute state.
//
// Touch panel serial protocol bit definitions (from decompiled analysis):
//   bit 0 (0x01) = PRESS — first frame of a new touch
//   bit 1 (0x02) = HELD  — finger still down (subsequent frames)
//   bit 4 (0x10) = NEW_DATA — fresh data available this frame
//
// Dress room drag (sub_2F680) uses these bits:
//   State 5: checks (& 1) to start drag, then transitions to state 6
//   State 6: checks (& 2) every frame to continue drag; when (& 2)==0, drops
// Arrow buttons (sub_28CA0): check (& 0x10) for hit-test gating
// Camera drag: (& 1) saves start pos, (& 2) computes delta
static uint8_t g_golfTouchState = 0;

// Hook for sub_C6D20 — touch panel frame processor.
// Called once per frame from sub_C7350 → sub_83E10.
// Computes touch state from mouse; result is returned by GolfTouchStateHook.
static int __cdecl GolfTouchProcessHook(int /*a1*/) {
	static bool s_prevMouseDown = false;

	bool mouseDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;

	if (mouseDown && !s_prevMouseDown) {
		// Press transition: bit 0 (press) + bit 4 (new_data)
		g_golfTouchState = 0x11;
		// Reset the button guard so one action can fire.
		*(volatile uint8_t*)0x004E5CEA = 0;
		GOLF_LOG("GolfPatch: Touch PRESS  state=0x11");
	} else if (mouseDown) {
		// Held: bit 1 (held) + bit 4 (new_data) — NOT bit 0
		// Dress room drag (state 6) checks (& 2) to continue dragging.
		g_golfTouchState = 0x12;
	} else if (s_prevMouseDown) {
		// Release: no touch bits — game detects release by absence
		g_golfTouchState = 0x00;
		GOLF_LOG("GolfPatch: Touch RELEASE state=0x00");
	} else {
		g_golfTouchState = 0x00; // idle
	}

	// Log dress room drag state for debugging
	{
		static uint8_t s_lastDragState = 0xFF;
		uint8_t dragState = *(volatile uint8_t*)0x004E8138; // dword_4E8138 low byte
		if (dragState != s_lastDragState) {
			GOLF_LOG("GolfPatch: DressRoom drag state %u→%u  touch=0x%02X",
			         (unsigned)s_lastDragState, (unsigned)dragState,
			         (unsigned)g_golfTouchState);
			s_lastDragState = dragState;
		}
	}

	s_prevMouseDown = mouseDown;
	return 0;
}

// Hook for sub_C5F20 — touch state reader.
// The game reads touch state ONLY through this function.
// Returns the state computed once per frame by GolfTouchProcessHook,
// so all callers within the same frame see the same value.
static char __cdecl GolfTouchStateHook() {
	return (char)g_golfTouchState;
}

// F1 (TEST) hook for sub_831D0
// Returns bit 2 (0x04) on rising edge of TEST button.
// Also updates touch panel coordinates (position tracking for hit-testing).
// Touch STATE (byte_718D64) is handled by GolfTouchProcessHook (sub_C6D20).
static int __cdecl GolfTestButtonHook() {
	*(volatile uint32_t*)0x00718E6C = 2; // TP subsystem initialized

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
		*(volatile float*)0x00718D70 = fx;
		*(volatile float*)0x00718D74 = fy;
		*(volatile float*)0x00718D6C = 1.0f;
		*(volatile float*)0x00718D48 = fx;
		*(volatile float*)0x00718D4C = fy;
		*(volatile float*)0x00718D50 = 1.0f;
		// dword_718D5C is read by sub_C5E50 as the third output value (pressure/Z).
		// sub_BCDA0 stores it in dword_7186BC, and sub_BE100 uses it for camera
		// rotation speed: speed = value * (1/255). Must be 255.0 for full speed.
		*(volatile float*)0x00718D5C = 255.0f;
	}

	// Test button edge detection
	static bool s_testWasDown = false;
	GolfSharedInputState state = {};
	GolfReadSharedInputState(&state);
	bool testDown = (state.control & 0x01) != 0 || (GetAsyncKeyState(VK_F1) & 0x8000) != 0;
	int result = 0;
	if (testDown && !s_testWasDown) {
		result = 0x04;
		GOLF_LOG("GolfPatch: [TEST] pressed");
	}
	s_testWasDown = testDown;
	return result;
}

// F2 (SERVICE) hook for sub_83220
static int __cdecl GolfServiceButtonHook() {
	static bool s_serviceWasDown = false;
	GolfSharedInputState state = {};
	GolfReadSharedInputState(&state);
	bool serviceDown = (state.control & 0x40) != 0 || (GetAsyncKeyState(VK_F2) & 0x8000) != 0;
	int result = 0;
	if (serviceDown && !s_serviceWasDown) {
		result = 0x08; // rising edge of SERVICE
		GOLF_LOG("GolfPatch: SERVICE button pressed");
	}
	s_serviceWasDown = serviceDown;
	return result;
}

// '5' key / shared memory (COIN) hook for sub_831F0
static int __cdecl GolfCoinButtonHook() {
	static bool s_coinWasDown = false;
	GolfSharedInputState sharedState = {};
	bool coinDown = (GolfReadSharedInputState(&sharedState) && sharedState.coinState != 0)
		|| (GetAsyncKeyState('5') & 0x8000) != 0;
	int result = 0;
	if (coinDown && !s_coinWasDown) {
		result = 1;
		GOLF_LOG("GolfPatch: COIN inserted");
	}
	s_coinWasDown = coinDown;
	return result;
}

// Minimal guardian thread — keeps hardware state vars set, mirrors input,
// and guards the display loop exit flag.
static DWORD WINAPI GolfGuardianThread(LPVOID param) {
	volatile char* pDone = (volatile char*)0x0067D37C;
	int guardianTick = 0;

	for (;;) {
		// Guard display loop exit flag (baseboard reboot handler can set this)
		if (*pDone != 0) {
			*pDone = 0;
		}

		// Keep Chihiro hardware state vars set (game may reset during init)
		// Only force card-related state when no card operation is active.
		// During card ops, sub_1490C0 sets state=1 and the card reader hook
		// advances it through 1→3→0 (read) or 5→6→4 (write).
		if (!g_golfCardOpActive) {
			*(volatile int*)0x008EFAC8 = 1;    // baseboard init flag
			int cardState = *(volatile int*)0x008EFAD8;
			if (cardState == 0) {
				*(volatile int*)0x008EFAD8 = 4; // DIMM state = ready (only when idle)
			}
		}

		// Keep subsystem status set (no real hardware to provide these)
		*(volatile int*)0x007CAA78 = -3;   // touch panel: READY
		*(volatile int*)0x007CAA7C = -1;   // card system: OK
		*(volatile int*)0x007CAA80 = -1;   // network: OK
		*(volatile int*)0x007CAA84 = -2;   // database: OFF

		// Force card system available — override hardware check results
		// [0x986129] = active card service count (sub_14AAC0 returns this)
		//   sub_14B170 checks: arg1 >= 1 AND arg1 <= [0x986129]
		//   So [0x986129] must be >= 1 for any card service to be usable.
		// [0x986128] = pending service count (copied to [0x986129] on activation)
		// Service entry table at 0x98652C + index*0xE0:
		//   [entry+0xCE] = enabled flag (must be non-zero)
		//   [entry+0xD8] = max capacity
		// [0x8F0308] = sub_14CA20 completion flag (1=complete)
		{
			static bool s_cardInit = false;
			if (!s_cardInit && guardianTick > 100) {
				// Force card service count
				*(volatile uint8_t*)0x00986128 = 2;    // pending count
				*(volatile uint8_t*)0x00986129 = 2;    // active count (2 services)

				// Enable service entries in table (base=0x98652C, stride=0xE0)
				// Service 1: entry at 0x98652C + 1*0xE0 = 0x98660C
				*(volatile uint8_t*)0x009866DA = 1;    // [entry1+0xCE] enabled
				*(volatile uint8_t*)0x009866E4 = 4;    // [entry1+0xD8] capacity
				// Service 2: entry at 0x98652C + 2*0xE0 = 0x9866EC
				*(volatile uint8_t*)0x009867BA = 1;    // [entry2+0xCE] enabled
				*(volatile uint8_t*)0x009867C4 = 4;    // [entry2+0xD8] capacity

				// Force sub_14CA20 completion
				*(volatile uint32_t*)0x008F0308 = 1;

				// Clear any error codes
				*(volatile uint8_t*)0x002B39A4 = 0;

				s_cardInit = true;
				GOLF_LOG("GolfPatch: [GUARDIAN] forced card system available (services=2)");
			}
		}

		// Keep DHCP done (no network hardware)
		*(volatile int*)0x007CAAA4 = 6;
		*(volatile int*)0x007CAAB4 = 1;
		*(volatile int*)0x007CAAAC = 0;

		// Keep TP subsystem ready
		*(volatile uint32_t*)0x00718E6C = 2;

		// Free play — set coin library free play flag
		// byte_8F0007 is the free play flag in the coin management library.
		// When 1, sub_149A60 returns 2 (enough credits) for all modes,
		// and sub_149D50 returns NULL (hides credit display).
		*(volatile uint8_t*)0x008F0007 = 1;

		// Card reader availability:
		// dword_677968 is now set natively by the ICCD state machine hook (RESET).
		// dword_4E7FE8 = card quality status (sub_2B590 returns this; <3 for no CAUTION)
		// sub_70440 checks: sub_24690() [returns dword_677968] && sub_2B590(0) < 3
		// dword_4E46BC = card physically present in reader (sub_24910 returns this)
		//   On real hardware, set by the card reader's physical slot sensor.
		//   In emulation, always 1 = card always inserted (virtual card file).
		*(volatile int32_t*)0x004E7FE8 = 0;  // card quality = best
		*(volatile int32_t*)0x004E46BC = 1;  // card present in reader

		// Periodic log: card subsystem state
		{
			static int sCardLogCounter = 0;
			if (++sCardLogCounter >= 300) { // every ~5 seconds at 60fps
				sCardLogCounter = 0;
				GOLF_LOG("[CARD] presence=%d avail=%d iccdState=%u iccdResult=%d outerSM=%d entryReq=%d",
					*(volatile int32_t*)0x004E46BC,
					*(volatile int32_t*)0x00677968,
					*(volatile uint32_t*)0x00677970,
					*(volatile int32_t*)0x00677974,
					*(volatile int32_t*)0x004E4698,
					*(volatile int32_t*)0x004E46E8);
			}
		}

		// Keep game mode (not test mode)
		*(volatile uint8_t*)0x0067D324 = 0;

		// Mirror JVS input from TeknoParrot shared memory
		GolfMirrorNativeInputCacheFromSharedMemory();

		// Touch panel state is updated synchronously in GolfTestButtonHook.
		// Guardian thread only maintains TP init flag as backup.
		*(volatile uint32_t*)0x00718E6C = 2; // TP hardware ready

		// Periodic status log
		{
			static int s_lastMode = -9999;
			uint32_t modePtr = *(volatile uint32_t*)0x006BC8CC;
			int modeId = modePtr ? *(volatile int*)(modePtr + 8) : -999;
			char pDoneVal = *pDone;

			// Log every mode change immediately
			if (modeId != s_lastMode) {
				GOLF_LOG("GolfPatch: [MODE-CHG] tick=%d %d -> %d pDone=%d",
					guardianTick, s_lastMode, modeId, (int)pDoneVal);
				s_lastMode = modeId;
			}

			// Also log pDone if non-zero
			if (pDoneVal != 0) {
				GOLF_LOG("GolfPatch: [PDONE] tick=%d pDone=%d mode=%d",
					guardianTick, (int)pDoneVal, modeId);
			}

			if (guardianTick > 0 && (guardianTick % 500) == 0) {
				GOLF_LOG("GolfPatch: [GUARDIAN] tick=%d mode=%d", guardianTick, modeId);
			}

			// Dump raw record analog channels every 200 ticks to trace JVS delivery
			if (guardianTick > 0 && (guardianTick % 200) == 0) {
				volatile uint16_t* rAna = reinterpret_cast<volatile uint16_t*>(kGolfRawJvsRecordBaseVA + 10);
				GOLF_LOG("GolfPatch: [RAW-ANA] tick=%d ch0=%04X ch1=%04X ch2=%04X ch3=%04X ch4=%04X ch5=%04X ch6=%04X ch7=%04X",
					guardianTick, rAna[0], rAna[1], rAna[2], rAna[3], rAna[4], rAna[5], rAna[6], rAna[7]);
				// Dump source/consumer struct analog at +0xB4
				uint16_t srcAna = *(volatile uint16_t*)(kGolfRawInputStruct0VA + 0xB4);
				uint16_t dmaAna = *(volatile uint16_t*)(kGolfRawInputStruct0DmaVA + 0xB4);
				uint16_t conAna = *(volatile uint16_t*)(kGolfRawInputConsumer0VA + 0xB4);
				GOLF_LOG("GolfPatch: [STRUCT-ANA] src+B4=%04X dma+B4=%04X con+B4=%04X",
					srcAna, dmaAna, conAna);
			}
		}

		guardianTick++;
		Sleep(16);
	}
	return 0;
}

static int __stdcall GolfMbRecvHook(uint32_t a1, uint32_t a2, uint32_t a3) {
	static int callCount = 0;
	if (++callCount <= 20)
		GOLF_LOG("MbRecv: a1=0x%08X a2=0x%08X a3=0x%08X (#%d)", a1, a2, a3, callCount);
	return 0; // no data
}

static int __stdcall GolfMbSendHook(uint32_t a1, uint32_t a2, uint32_t a3) {
	static int callCount = 0;
	if (++callCount <= 20) {
		GOLF_LOG("MbSend: a1=0x%08X a2=0x%08X a3=0x%08X (#%d)", a1, a2, a3, callCount);
		if (a1 > 0x10000 && a1 < 0x10000000) {
			uint8_t* d = (uint8_t*)a1;
			GOLF_LOG("  data: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
				d[0],d[1],d[2],d[3],d[4],d[5],d[6],d[7],d[8],d[9],d[10],d[11],d[12],d[13],d[14],d[15]);
		}
	}
	return 0;
}

static int __stdcall GolfLinkOkHook(uint32_t a1, uint32_t a2, uint32_t a3) {
	return 1; // link is OK
}

// Credit calculation hook — replaces sub_10AAC0
// Returns our injected credit count from [6779CC].
// The original function computes credits from JAMMA coin data but returns 0 without JVS config.
static int __cdecl GolfCreditCalcHook() {
	int credits = *(volatile int32_t*)0x006779CC;
	static int logCount = 0;
	if (++logCount <= 10 || (logCount % 500) == 0) {
		void* retAddr = _ReturnAddress();
		GOLF_LOG("GolfPatch: [CREDIT-CALC] called from 0x%08X, returning %d", (unsigned)(uintptr_t)retAddr, credits);
	}
	return credits;
}

// Card reader state machine hook — replaces the 6-state switch function (sub_148DB0).
// The original function manages card dispense/read/write via serial I/O.
// We handle states directly with file-based card I/O:
//   State 1-3: READ path (load card.bin into track buffer at 0x8EFAE8)
//   State 5-6: WRITE path (save track buffer to card.bin)
//   State 0/4:  Idle/done
static int __cdecl GolfCardReaderHook() {
	static int callCount = 0;
	void* retAddr = _ReturnAddress();
	volatile int32_t* pState = (volatile int32_t*)0x008EFAD8;
	int state = *pState;

	if (++callCount <= 30 || (callCount % 1000) == 0) {
		GOLF_LOG("[CARD-RDR] call #%d from 0x%08X state=%d",
			callCount, (unsigned)(uintptr_t)retAddr, state);
	}

	switch (state) {
	case 1: {
		// Read start: load card.bin into the 207-byte track buffer
		GOLF_LOG("[CARD-RDR] State 1: READ — loading card.bin");
		uint8_t cardBuf[207];
		memset(cardBuf, 0, sizeof(cardBuf));
		bool loaded = GolfLoadCard(cardBuf, sizeof(cardBuf));
		if (loaded) {
			memcpy((void*)0x8EFAE8, cardBuf, sizeof(cardBuf));
			*(volatile uint8_t*)0x8EFADE = 0;  // read result = success
			GOLF_LOG("[CARD-RDR] Card data loaded (%zu bytes)", sizeof(cardBuf));
		} else {
			memset((void*)0x8EFAE8, 0, sizeof(cardBuf));
			*(volatile uint8_t*)0x8EFADE = 0;  // treat as empty card
			GOLF_LOG("[CARD-RDR] No card.bin — using empty card");
		}
		*pState = 3;  // skip to process-result step
		break;
	}
	case 2:
		// Read data (intermediate step) — advance
		*pState = 3;
		break;
	case 3:
		// Read done — set idle
		GOLF_LOG("[CARD-RDR] State 3: READ complete");
		g_golfCardOpActive = false;
		*pState = 0;
		break;
	case 5: {
		// Write start: save 207-byte track buffer to card.bin
		GOLF_LOG("[CARD-RDR] State 5: WRITE — saving card.bin");
		GolfSaveCard((const uint8_t*)0x8EFAE8, 207);
		*pState = 6;
		break;
	}
	case 6:
		// Write done
		GOLF_LOG("[CARD-RDR] State 6: WRITE complete");
		g_golfCardOpActive = false;
		*pState = 4;
		break;
	default:
		// Idle / done — reset to 0
		*pState = 0;
		break;
	}

	return 0;
}

// Card operation initiator hook — replaces sub_1490C0.
// __stdcall, 3 args: (int mode, int arg2, void* cardDataPtr)
// cardDataPtr == NULL → READ operation (load card.bin → 0x8EFAE8)
// cardDataPtr != NULL → WRITE operation (copy 48-byte struct to card buffer)
// Returns 0 = success, non-zero = error.
// Also sets [0x8F0308] = 1 to signal sub_14CA20 that the operation completed
// immediately, so the caller's polling loop exits without blocking.
static int __stdcall GolfCardInitHook(int mode, int arg2, void* cardDataPtr) {
	GOLF_LOG("[CARD-INIT] mode=%d arg2=%d cardData=%p", mode, arg2, cardDataPtr);

	// Clear card state structure (0x4B dwords = 300 bytes starting at 0x8EFAC8)
	memset((void*)0x8EFAC8, 0, 0x4B * 4);

	// Store mode (replicates original behavior)
	*(volatile int*)0x8EFACC = mode;

	// Clear result flags
	*(volatile uint8_t*)0x8EFADC = 0;  // result code
	*(volatile uint8_t*)0x8EFADD = 0;  // result byte
	*(volatile uint8_t*)0x8EFADE = 0;  // flags (original sets 8, we set 0 = success)

	g_golfCardOpActive = true;

	if (cardDataPtr == nullptr) {
		// READ operation: load card.bin into 207-byte track buffer
		GOLF_LOG("[CARD-INIT] READ operation");
		uint8_t cardBuf[207];
		memset(cardBuf, 0, sizeof(cardBuf));
		bool loaded = GolfLoadCard(cardBuf, sizeof(cardBuf));
		if (loaded) {
			memcpy((void*)0x8EFAE8, cardBuf, sizeof(cardBuf));
			GOLF_LOG("[CARD-INIT] READ: loaded card.bin");
		} else {
			memset((void*)0x8EFAE8, 0, sizeof(cardBuf));
			GOLF_LOG("[CARD-INIT] READ: no card.bin, empty card");
		}
	} else {
		// WRITE operation: save the 48-byte struct data (and track buffer if populated)
		GOLF_LOG("[CARD-INIT] WRITE operation (src=%p)", cardDataPtr);
		// The original function copies 48 bytes (12 dwords) from cardDataPtr
		// into a local buffer for formatting. We save the track buffer directly
		// since the formatted data ends up at 0x8EFAE8 anyway.
		// For now, just mark the operation as done — actual write happens in
		// the card reader hook when state=5 is reached.
	}

	// Card state → idle (operation processed instantly)
	*(volatile int*)0x8EFAD8 = 0;

	// Signal sub_14CA20 to return 0 (complete) — bypasses its 11-state machine
	*(volatile int*)0x008F0308 = 1;

	g_golfCardOpActive = false;

	GOLF_LOG("[CARD-INIT] complete: [8EFAD8]=0 [8F0308]=1");
	return 0;
}

// ── CRI ADXF_GetStat hook ────────────────────────────────────────
static int __cdecl GolfGetStatHook(int handle) {
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

// ── XBE hash constants ───────────────────────────────────────────

// Sega Golf Club: Network Pro Tour (GDX-0010C)
static const uint64_t kSGC1Hashes[] = {
	0x1824EAFC8DE7DD1DULL, // raw file hash
	0x18E6B0401CCC3553ULL, // runtime hash
};

static bool IsSGC1(uint64_t xbeHash)
{
	for (auto h : kSGC1Hashes) {
		if (xbeHash == h) return true;
	}
	return false;
}

bool IsGolfXbe(uint64_t xbeHash)
{
	return IsSGC1(xbeHash);
}

// ── Helper: write a 4-byte value at a guest VA ───────────────────

static void PatchDword(uintptr_t va, uint32_t value)
{
	PatchXbeBytes(va, reinterpret_cast<const uint8_t*>(&value), sizeof(value));
}

// ── Patch toggle flags ───────────────────────────────────────────
#define GOLF_LINKOK         1   // 1: LinkOK JMP hook
#define GOLF_MBRECVSEND     1   // 2: MbRecv/MbSend JMP hooks
#define GOLF_DIMM_READY     1   // 3: DIMM-board ready bypass
#define GOLF_KICKOFF_IDLE   1   // 4: D3D_KickOffAndWaitForIdle stub
#define GOLF_CRI_GETSTAT    1   // 5: CRI ADXF_GetStat hook
#define GOLF_CARD_READER    1   // 6: Card reader state machine stub
#define GOLF_DIMM_THREAD    0   // 7: DIMM update thread stubs (OFF - let threads run)
#define GOLF_DIMM_COMM      1   // 8: DIMM communication function stubs
#define GOLF_SKIP_INIT      1   // 9: Force-skip SYSTEM INITIALIZE state check

// ── Main patch entry point ───────────────────────────────────────

void ApplyGolfPatches(uint64_t xbeHash, uint32_t imageSize)
{
	g_jvs_game_type = JvsGameType::SegaGolfClub;
	GOLF_LOG("GolfPatch: applying patches for SGC Pro Tour (imageSize=0x%X)\n", imageSize);

	// Override Xbox backbuffer to 800x600 — Chihiro golf games render at higher
	// resolution on real hardware (VGA output). The Xbox default is 640x480 which
	// cuts off the right and bottom edges of the game's viewport.
	g_ChihiroBackbufferOverrideW = 800;
	g_ChihiroBackbufferOverrideH = 600;
	GOLF_LOG("GolfPatch: Backbuffer override set to 800x600");

	// Write marker file so we know patches are running
	{
		FILE* f = fopen("C:\\temp\\golf_patches.log", "w");
		if (f) { fprintf(f, "GolfPatch: start (imageSize=0x%X)\n", imageSize); fclose(f); }
	}
#if GOLF_LINKOK
	// === LinkOK — JMP hook to GolfLinkOkHook ===
	// Pattern: TEST EAX,EAX; JNZ +8; MOV EAX,0xFFFFFFFE; RET 0Ch
	// Same Sega media board library as Gundam.
	{
		static const uint8_t kLinkOkPat[] = {
			0x85,0xC0, 0x75,0x08, 0xB8,0xFE,0xFF,0xFF,0xFF, 0xC2,0x0C,0x00
		};
		auto linkOkHits = ScanXbeAll(kLinkOkPat, sizeof(kLinkOkPat), imageSize);
		for (auto linkOkVA : linkOkHits) {
			if (linkOkVA >= 5 && ((const uint8_t*)(linkOkVA - 5))[0] == 0xA1) {
				PatchWithJmp(linkOkVA - 5, (const void*)&GolfLinkOkHook);
				GOLF_LOG("GolfPatch: LinkOK patched at VA 0x%08X\n", (unsigned)(linkOkVA - 5));
			}
		}
		if (linkOkHits.empty()) {
			GOLF_LOG("GolfPatch: LinkOK pattern not found!\n");
		}
	}
#endif

#if GOLF_MBRECVSEND
	// === MbRecvPacket / MbSendPacket — JMP hooks ===
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
			PatchWithJmp(mbRecvVA, (const void*)&GolfMbRecvHook);
			PatchWithJmp(mbSendVA, (const void*)&GolfMbSendHook);
			GOLF_LOG("GolfPatch: MbRecv/MbSend hooked at 0x%08X / 0x%08X\n", (unsigned)mbRecvVA, (unsigned)mbSendVA);

			// === JvsNodeSendPacket / JvsNodeReceivePacket detection ===
			// Scan the Sega library region for CALL instructions targeting
			// MbSendPacket and MbRecvPacket. The functions containing these
			// CALLs are the JvsNode functions that we need to hook.
			uintptr_t jvsNodeSendVA = 0, jvsNodeRecvVA = 0;
			uintptr_t scanStart = (mbRecvVA > 0x2000) ? mbRecvVA - 0x2000 : 0x10000;
			uintptr_t scanEnd = mbSendVA + 0x2000;
			if (scanEnd > 0x10000 + imageSize) scanEnd = 0x10000 + imageSize;
			for (uintptr_t addr = scanStart; addr < scanEnd - 5; addr++) {
				uint8_t* p = (uint8_t*)addr;
				if (p[0] == 0xE8) {
					int32_t rel = *(int32_t*)(p + 1);
					uintptr_t target = addr + 5 + rel;
					if (target == mbSendVA) {
						// Found a CALL to MbSendPacket — walk back to find function start
						// Look for PUSH EBP (0x55) or SUB ESP (0x83 0xEC) as prologue
						for (uintptr_t back = addr - 1; back > addr - 0x100; back--) {
							uint8_t b = *(uint8_t*)back;
							// Check for common function prologues
							if (b == 0x55 && *(uint8_t*)(back+1) == 0x8B && *(uint8_t*)(back+2) == 0xEC) {
								jvsNodeSendVA = back;
								GOLF_LOG("GolfPatch: Found JvsNodeSendPacket candidate at 0x%08X (CALL MbSend at 0x%08X)\n",
									(unsigned)back, (unsigned)addr);
								break;
							}
							if (b == 0x83 && *(uint8_t*)(back+1) == 0xEC) {
								jvsNodeSendVA = back;
								GOLF_LOG("GolfPatch: Found JvsNodeSendPacket candidate at 0x%08X (CALL MbSend at 0x%08X)\n",
									(unsigned)back, (unsigned)addr);
								break;
							}
							// INT3 padding or RET = previous function end
							if (b == 0xCC || (b == 0xC3) || (b == 0xC2)) {
								jvsNodeSendVA = back + 1;
								GOLF_LOG("GolfPatch: Found JvsNodeSendPacket candidate at 0x%08X (after ret/int3 at 0x%08X)\n",
									(unsigned)(back + 1), (unsigned)back);
								break;
							}
						}
						break; // only need first CALL to MbSend
					}
					if (target == mbRecvVA) {
						for (uintptr_t back = addr - 1; back > addr - 0x100; back--) {
							uint8_t b = *(uint8_t*)back;
							if (b == 0x55 && *(uint8_t*)(back+1) == 0x8B && *(uint8_t*)(back+2) == 0xEC) {
								jvsNodeRecvVA = back;
								GOLF_LOG("GolfPatch: Found JvsNodeReceivePacket candidate at 0x%08X (CALL MbRecv at 0x%08X)\n",
									(unsigned)back, (unsigned)addr);
								break;
							}
							if (b == 0x83 && *(uint8_t*)(back+1) == 0xEC) {
								jvsNodeRecvVA = back;
								GOLF_LOG("GolfPatch: Found JvsNodeReceivePacket candidate at 0x%08X (CALL MbRecv at 0x%08X)\n",
									(unsigned)back, (unsigned)addr);
								break;
							}
							if (b == 0xCC || (b == 0xC3) || (b == 0xC2)) {
								jvsNodeRecvVA = back + 1;
								GOLF_LOG("GolfPatch: Found JvsNodeReceivePacket candidate at 0x%08X (after ret/int3 at 0x%08X)\n",
									(unsigned)(back + 1), (unsigned)back);
								break;
							}
						}
						break; // only need first CALL to MbRecv
					}
				}
			}
			// Hook JvsNode functions if found
			if (jvsNodeSendVA && jvsNodeRecvVA) {
				PatchWithJmp(jvsNodeSendVA, (const void*)&GolfJvsNodeSendPacketHook);
				PatchWithJmp(jvsNodeRecvVA, (const void*)&GolfJvsNodeRecvPacketHook);
				GOLF_LOG("GolfPatch: JvsNode hooked! Send=0x%08X Recv=0x%08X\n",
					(unsigned)jvsNodeSendVA, (unsigned)jvsNodeRecvVA);
			} else {
				// Pro Tour statically links the public JvsNode wrappers at fixed VAs.
				// The local heuristic above looks for nearby direct calls to MbSend/MbRecv,
				// but this build routes through deeper transport helpers instead, so the
				// scan misses the wrappers even though the functions are present.
				PatchWithJmp(kSgc1JvsNodeSendVA, (const void*)&GolfJvsNodeSendPacketHook);
				PatchWithJmp(kSgc1JvsNodeRecvVA, (const void*)&GolfJvsNodeRecvPacketHook);
				GOLF_LOG("GolfPatch: JvsNode hooked via SGC1 fallback! Send=0x%08X Recv=0x%08X\n",
					(unsigned)kSgc1JvsNodeSendVA, (unsigned)kSgc1JvsNodeRecvVA);
			}
		} else {
			GOLF_LOG("GolfPatch: MbRecvPacket/MbSendPacket not found! (%zu hits)\n", mbHits.size());
		}
	}
#endif

#if GOLF_DIMM_READY
	// === DIMM-board ready bypass (always return 1) ===
	// Pattern: CMP dword ptr [addr], 4; SBB EAX,EAX; INC EAX; RET
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
			GOLF_LOG("GolfPatch: DIMM-ready patched at 0x%08X\n", (unsigned)dimmReadyVA);
		} else {
			GOLF_LOG("GolfPatch: DIMM-ready pattern not found\n");
		}
	}
#endif

#if GOLF_KICKOFF_IDLE
	// === D3D_KickOffAndWaitForIdle NOP-stub ===
	// Polls NV2A GPU status registers — infinite busy-wait in HLE mode.
	{
		static const uint8_t kKickIdlePat[] = {
			0xA1, 0xFF,0xFF,0xFF,0xFF, 0x8B, 0x48, 0x2C
		};
		uintptr_t kickIdleVA = ScanXbe(kKickIdlePat, sizeof(kKickIdlePat), imageSize);
		if (kickIdleVA) {
			static const uint8_t kRet = 0xC3;
			PatchXbeBytes(kickIdleVA, &kRet, 1);
			GOLF_LOG("GolfPatch: D3D_KickOffAndWaitForIdle stubbed at 0x%08X\n", (unsigned)kickIdleVA);
		} else {
			GOLF_LOG("GolfPatch: D3D_KickOffAndWaitForIdle pattern NOT FOUND\n");
		}
	}
#endif

#if GOLF_CRI_GETSTAT
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
			PatchWithJmp(getStatVA, (const void*)&GolfGetStatHook);
			GOLF_LOG("GolfPatch: CRI GetStat hooked at 0x%08X\n", (unsigned)getStatVA);
		} else {
			GOLF_LOG("GolfPatch: CRI GetStat pattern not found\n");
		}
	}
#endif

#if GOLF_CARD_READER
	// === Card reader state machine hook (sub_148DB0) ===
	// 6-state switch on dword at 0x8EFAD8, with 0x3C (60) retry loops.
	// Pattern: MOV EAX,[state]; SUB ESP,8; DEC EAX; CMP EAX,5; JA ...
	{
		static const uint8_t kCardReaderPat[] = {
			0xA1, 0xFF,0xFF,0xFF,0xFF,
			0x83,0xEC,0x08,
			0x48,
			0x83,0xF8,0x05,
			0x0F,0x87
		};
		uintptr_t cardReaderVA = ScanXbe(kCardReaderPat, sizeof(kCardReaderPat), imageSize);
		if (cardReaderVA) {
			PatchWithJmp(cardReaderVA, (const void*)&GolfCardReaderHook);
			GOLF_LOG("GolfPatch: Card reader hooked at 0x%08X\n", (unsigned)cardReaderVA);
		} else {
			GOLF_LOG("GolfPatch: Card reader pattern not found\n");
		}
	}

	// === Card operation initiator (sub_1490C0) ===
	// NOT hooked — the original function does critical card system initialization
	// (structure setup, data formatting via sub_14830F, sub_148390, etc.) that
	// must run. The serial I/O is handled by the card reader hook above.
	// The original function sets state=1, then the DIMM thread polls sub_148DB0
	// (our hook) which advances states and does card file I/O.
	// sub_14CA20 (high-level card manager) polls until [0x8F0308] != 0;
	// state 9 of its switch sets that flag naturally when sub_148DB0 completes.
#endif

#if GOLF_DIMM_THREAD
	// === DIMM update thread stubs ===
	// Two thread entry points run DIMM I/O loops with many busy-waits.
	// 0x11320 and 0x18AE70 are thread entries (no CALL references).
	// Pattern for 0x11320: PUSH EBP; MOV EBP,ESP; AND ESP,-8; SUB ESP,28h; PUSH ESI; PUSH EDI; PUSH 002676B0h
	{
		static const uint8_t kDimmThread1Pat[] = {
			0x55, 0x8B,0xEC, 0x83,0xE4,0xF8, 0x83,0xEC,0x28,
			0x56, 0x57, 0x68, 0xB0,0x76,0x26,0x00, 0xE8
		};
		uintptr_t dimmThread1VA = ScanXbe(kDimmThread1Pat, sizeof(kDimmThread1Pat), imageSize);
		if (dimmThread1VA) {
			// Replace with: XOR EAX,EAX; RET 4 (thread entry takes one param)
			static const uint8_t kRetThread[] = { 0x33,0xC0, 0xC2,0x04,0x00 };
			PatchXbeBytes(dimmThread1VA, kRetThread, sizeof(kRetThread));
			GOLF_LOG("GolfPatch: DIMM thread 1 stubbed at 0x%08X\n", (unsigned)dimmThread1VA);
		} else {
			GOLF_LOG("GolfPatch: DIMM thread 1 pattern not found\n");
		}
	}
	// Pattern for 0x18AE70: PUSH EBP; MOV EBP,ESP; AND ESP,-8; SUB ESP,1Ch; PUSH ESI; PUSH 002676B0h
	{
		static const uint8_t kDimmThread2Pat[] = {
			0x55, 0x8B,0xEC, 0x83,0xE4,0xF8, 0x83,0xEC,0x1C,
			0x56, 0x68, 0xB0,0x76,0x26,0x00, 0xE8
		};
		uintptr_t dimmThread2VA = ScanXbe(kDimmThread2Pat, sizeof(kDimmThread2Pat), imageSize);
		if (dimmThread2VA) {
			static const uint8_t kRetThread[] = { 0x33,0xC0, 0xC2,0x04,0x00 };
			PatchXbeBytes(dimmThread2VA, kRetThread, sizeof(kRetThread));
			GOLF_LOG("GolfPatch: DIMM thread 2 stubbed at 0x%08X\n", (unsigned)dimmThread2VA);
		} else {
			GOLF_LOG("GolfPatch: DIMM thread 2 pattern not found\n");
		}
	}
#endif

#if GOLF_DIMM_COMM
	// === DIMM communication function stubs ===
	// 0x17FD90: DIMM init/comm function with busy-wait loops
	// Pattern: PUSH ESI; PUSH 1; XOR ESI,ESI; CALL ...
	{
		static const uint8_t kDimmCommPat[] = {
			0x56, 0x6A,0x01, 0x33,0xF6, 0xE8
		};
		auto dimmCommHits = ScanXbeAll(kDimmCommPat, sizeof(kDimmCommPat), imageSize);
		for (auto va : dimmCommHits) {
			if (va >= 0x170000 && va <= 0x190000) {
				static const uint8_t kRet0[] = { 0x33,0xC0, 0xC3 };
				PatchXbeBytes(va, kRet0, sizeof(kRet0));
				GOLF_LOG("GolfPatch: DIMM comm stubbed at 0x%08X\n", (unsigned)va);
			}
		}
	}
	// 0x1849C0: Baseboard check function
	// Pattern: PUSH EBX; PUSH ESI; PUSH EDI; CALL ... (target in 0x1Cxxxx range)
	{
		static const uint8_t kBaseboardPat[] = {
			0x53, 0x56, 0x57, 0xE8, 0xFF,0xFF,0xFF,0xFF,
			0x8B,0xF8, 0xA1, 0xFF,0xFF,0xFF,0xFF,
			0x83,0xCB,0xFF, 0x85,0xC0
		};
		uintptr_t baseboardVA = ScanXbe(kBaseboardPat, sizeof(kBaseboardPat), imageSize);
		if (baseboardVA) {
			static const uint8_t kRet0[] = { 0x33,0xC0, 0xC3 };
			PatchXbeBytes(baseboardVA, kRet0, sizeof(kRet0));
			GOLF_LOG("GolfPatch: Baseboard check stubbed at 0x%08X\n", (unsigned)baseboardVA);
		} else {
			GOLF_LOG("GolfPatch: Baseboard check pattern not found\n");
		}
	}
#endif

#if GOLF_SKIP_INIT
	// === Help state machine progress past hardware-blocking checks ===
	// The update function dispatches state handlers 0-6. State 0 blocks on
	// hardware checks that never pass in emulation. Instead of disabling the
	// state machine entirely (which skips all init), we fix the blocking calls
	// so states 0→1→2→3→... progress naturally.
	{
		// --- Part A: Find state variable address ---
		static const uint8_t kInitCheckPat[] = {
			0x39, 0x1D, 0xFF, 0xFF, 0xFF, 0xFF,  // CMP [subsys1], EBX
			0xBF, 0x04, 0x00, 0x00, 0x00,         // MOV EDI, 4
			0x7D, 0x1E,                            // JGE +0x1E
			0x39, 0x1D, 0xFF, 0xFF, 0xFF, 0xFF,   // CMP [subsys2], EBX
			0x7D, 0x16,                            // JGE +0x16
			0x39, 0x1D, 0xFF, 0xFF, 0xFF, 0xFF,   // CMP [subsys3], EBX
			0x7D, 0x0E,                            // JGE +0x0E
			0x39, 0x1D, 0xFF, 0xFF, 0xFF, 0xFF,   // CMP [subsys4], EBX
			0x7D, 0x06,                            // JGE +0x06
			0x89, 0x3D, 0xFF, 0xFF, 0xFF, 0xFF    // MOV [stateVar], EDI
		};
		uintptr_t initCheckVA = ScanXbe(kInitCheckPat, sizeof(kInitCheckPat), imageSize);
		if (initCheckVA) {
			uint32_t stateAddr = *(uint32_t*)(initCheckVA + 39);
			GOLF_LOG("GolfPatch: Init state var at 0x%08X", stateAddr);

			// --- Part B: Patch sub_F3910 (hardware error check) ---
			// Pattern: MOV EAX,[ESP+4]; LEA EAX,[EAX+EAX*8]; MOV ECX,[EAX*4+table]; TEST ECX,ECX; SETE AL; RET
			// This is a tiny function: 8B 44 24 04 8D 04 C0 8B 0C 85 ?? ?? ?? ?? 85 C9 0F 94 C0 C3
			{
				static const uint8_t kErrCheckPat[] = {
					0x8B, 0x44, 0x24, 0x04,   // MOV EAX, [ESP+4]
					0x8D, 0x04, 0xC0,         // LEA EAX, [EAX+EAX*8]
					0x8B, 0x0C, 0x85,         // MOV ECX, [EAX*4+...
					0xFF, 0xFF, 0xFF, 0xFF,   //   table_addr]
					0x85, 0xC9,               // TEST ECX, ECX
					0x0F, 0x94, 0xC0,         // SETE AL
					0xC3                       // RET
				};
				uintptr_t errCheckVA = ScanXbe(kErrCheckPat, sizeof(kErrCheckPat), imageSize);
				if (errCheckVA) {
					// Replace with: XOR EAX,EAX; RET (always return 0 = "no error")
					static const uint8_t kRet0[] = { 0x33, 0xC0, 0xC3 };
					PatchXbeBytes(errCheckVA, kRet0, sizeof(kRet0));
					GOLF_LOG("GolfPatch: Error check (sub_F3910) stubbed at 0x%08X", (unsigned)errCheckVA);
				} else {
					GOLF_LOG("GolfPatch: Error check pattern NOT FOUND!");
				}
			}

			// --- Part B2: Patch sub_F3930 (firmware version check) ---
			// Pattern: MOV EAX,[ESP+4]; LEA EAX,[EAX+EAX*2]; CMP [EAX*4+table2],2; SETE AL; RET
			// Returns 1 if firmware version == 2 for the given subsystem.
			// Stub to always return 1 so Error 14 ("Network firmware version...") is never raised.
			{
				static const uint8_t kVerCheckPat[] = {
					0x8B, 0x44, 0x24, 0x04,   // MOV EAX, [ESP+4]
					0x8D, 0x04, 0xC0,         // LEA EAX, [EAX+EAX*2]
					0x83, 0x3C, 0x85,         // CMP [EAX*4+...
					0xFF, 0xFF, 0xFF, 0xFF,   //   table2_addr], ...
					0x02,                     // 2
					0x0F, 0x94, 0xC0,         // SETE AL
					0xC3                       // RET
				};
				uintptr_t verCheckVA = ScanXbe(kVerCheckPat, sizeof(kVerCheckPat), imageSize);
				if (verCheckVA) {
					// Replace with: MOV AL,1; RET (always return 1 = "version OK")
					static const uint8_t kRet1[] = { 0xB0, 0x01, 0xC3 };
					PatchXbeBytes(verCheckVA, kRet1, sizeof(kRet1));
					GOLF_LOG("GolfPatch: Version check (sub_F3930) stubbed at 0x%08X", (unsigned)verCheckVA);
				} else {
					GOLF_LOG("GolfPatch: Version check pattern NOT FOUND!");
				}
			}

			// --- Part B3: Stub sub_100EB0 (subsystem 4 monitor / Error 14 display) ---
			// This function checks [DA6B34] each frame and can display Error 14/15.
			// Pattern: MOV EAX,[DA6B34]; LEA ECX,[EAX+7]; CMP ECX,9; JA ...
			// Stub to just return so it never displays Error 14.
			{
				static const uint8_t kSubsys4Pat[] = {
					0xA1, 0x34, 0x6B, 0xDA, 0x00,  // MOV EAX, [DA6B34]
					0x8D, 0x48, 0x07,               // LEA ECX, [EAX+7]
					0x83, 0xF9, 0x09,               // CMP ECX, 9
					0x0F, 0x87                       // JA ...
				};
				uintptr_t subsys4VA = ScanXbe(kSubsys4Pat, sizeof(kSubsys4Pat), imageSize);
				if (subsys4VA) {
					// Replace with RET (just return, don't process subsystem 4 at all)
					static const uint8_t kRet = 0xC3;
					PatchXbeBytes(subsys4VA, &kRet, 1);
					GOLF_LOG("GolfPatch: Subsystem 4 monitor stubbed at 0x%08X", (unsigned)subsys4VA);
				} else {
					GOLF_LOG("GolfPatch: Subsystem 4 monitor pattern NOT FOUND!");
				}
			}

			// --- Part B4: Patch sub_0A0840 (IsTestMode check) ---
			// Returns 1 if test mode, 0 if game mode.
			// The init function uses this to select test mode vs game mode startup.
			// Pattern: MOV ECX,[31D324h]; MOV DL,[ECX+4]; XOR AL,AL; TEST DL,DL
			// Patch to always return 0 to boot into GAME MODE (not test menu).
			{
				static const uint8_t kErrGuardPat[] = {
					0x8B, 0x0D, 0x24, 0xD3, 0x31, 0x00,  // MOV ECX, [31D324h]
					0x8A, 0x51, 0x04,                      // MOV DL, [ECX+4]
					0x32, 0xC0,                            // XOR AL, AL
					0x84, 0xD2,                            // TEST DL, DL
					0x74, 0x02,                            // JZ +2
					0xB0, 0x01,                            // MOV AL, 1
					0xC3                                   // RET
				};
				uintptr_t errGuardVA = ScanXbe(kErrGuardPat, sizeof(kErrGuardPat), imageSize);
				if (errGuardVA) {
					// Replace with: XOR AL,AL; RET (always return 0 = "game mode")
					static const uint8_t kRet0[] = { 0x32, 0xC0, 0xC3 };
					PatchXbeBytes(errGuardVA, kRet0, sizeof(kRet0));
					GOLF_LOG("GolfPatch: IsTestMode (sub_0A0840) → always 0 (game mode) at 0x%08X", (unsigned)errGuardVA);
				} else {
					GOLF_LOG("GolfPatch: IsTestMode pattern NOT FOUND!");
				}
			}

			// --- Part C: Patch sub_A1A10 (device wait) to return fake struct ---
			// State 0 handler calls sub_A1A10(0), expects non-NULL result with
			// bytes at offset +0x14 and +0x15 being non-zero.
			// Find the pattern: PUSH EBX(=0); CALL sub; ADD ESP,4; CMP EAX,EBX; JE ...
			// which is: 53 E8 ?? ?? ?? ?? 83 C4 04 3B C3 0F 84
			{
				static const uint8_t kDevWaitPat[] = {
					0x53,                               // PUSH EBX (=0)
					0xE8, 0xFF, 0xFF, 0xFF, 0xFF,       // CALL sub_A1A10
					0x83, 0xC4, 0x04,                   // ADD ESP, 4
					0x3B, 0xC3,                         // CMP EAX, EBX
					0x0F, 0x84                          // JE ... (bail if NULL)
				};
				uintptr_t devWaitVA = ScanXbe(kDevWaitPat, sizeof(kDevWaitPat), imageSize);
				if (devWaitVA) {
					// Initialize fake device struct
					memset(s_fakeDeviceStruct, 0, sizeof(s_fakeDeviceStruct));
					s_fakeDeviceStruct[0x14] = 1;  // must be non-zero
					s_fakeDeviceStruct[0x15] = 1;  // must be non-zero

					// NOP out the PUSH+CALL+ADDSP (9 bytes) and replace with:
					// MOV EAX, &s_fakeDeviceStruct (5 bytes) + 4 NOPs
					uint8_t movEax[9] = { 0xB8, 0,0,0,0, 0x90,0x90,0x90,0x90 };
					uint32_t fakeAddr = (uint32_t)(uintptr_t)s_fakeDeviceStruct;
					memcpy(movEax + 1, &fakeAddr, 4);
					PatchXbeBytes(devWaitVA, movEax, sizeof(movEax));
					GOLF_LOG("GolfPatch: Device wait bypassed at 0x%08X (fake=0x%08X)",
						(unsigned)devWaitVA, fakeAddr);
				} else {
			GOLF_LOG("GolfPatch: Device wait pattern NOT FOUND!");
				}
			}

		} else {
			GOLF_LOG("GolfPatch: Init state check pattern NOT FOUND");
		}

		// --- Part E: Stub subsystem UPDATE functions ---
		// These 3 functions check hardware and update subsystem vars (DA6B28-DA6B30).
		// They're called from state 3 handler. If not stubbed, they reset subsystem
		// vars to 0 and run hardware checks that fail in emulation.
		// (sub_100EB0 for DA6B34 was already stubbed in Part B3 above.)
		// We make each set its subsystem var to -3 (done) and return.
		{
			// sub_100A00: updates DA6B28 (network check)
			// Pattern: MOV EAX,[DA6B28]; PUSH ESI; ADD EAX,3; XOR ESI,ESI; CMP EAX,3; JA
			static const uint8_t kUpd1Pat[] = {
				0xA1, 0x28, 0x6B, 0xDA, 0x00,  // MOV EAX, [DA6B28]
				0x56,                           // PUSH ESI
				0x83, 0xC0, 0x03,               // ADD EAX, 3
				0x33, 0xF6,                     // XOR ESI, ESI
				0x83, 0xF8, 0x03,               // CMP EAX, 3
				0x77                            // JA
			};
			uintptr_t upd1VA = ScanXbe(kUpd1Pat, sizeof(kUpd1Pat), imageSize);
			if (upd1VA) {
				// MOV dword [DA6B28], -3; XOR EAX,EAX; RET
				static const uint8_t kPatch[] = {
					0xC7, 0x05, 0x28, 0x6B, 0xDA, 0x00, 0xFD, 0xFF, 0xFF, 0xFF,
					0x33, 0xC0, 0xC3
				};
				PatchXbeBytes(upd1VA, kPatch, sizeof(kPatch));
				GOLF_LOG("GolfPatch: Subsys update 1 (DA6B28) patched at 0x%08X", (unsigned)upd1VA);
			}

			// sub_100A80: updates DA6B2C (database check)
			// Pattern: SUB ESP,10h; LEA EAX,[ESP+4]; PUSH EAX; LEA ECX,[ESP+0Ch]; PUSH ECX
			static const uint8_t kUpd2Pat[] = {
				0x83, 0xEC, 0x10,               // SUB ESP, 10h
				0x8D, 0x44, 0x24, 0x04,         // LEA EAX, [ESP+4]
				0x50,                           // PUSH EAX
				0x8D, 0x4C, 0x24, 0x0C,         // LEA ECX, [ESP+0Ch]
				0x51                            // PUSH ECX
			};
			uintptr_t upd2VA = 0; // SGC2006-specific pattern, skip for SGC1
			if (upd2VA) {
				// MOV dword [DA6B2C], -3; XOR EAX,EAX; RET
				static const uint8_t kPatch[] = {
					0xC7, 0x05, 0x2C, 0x6B, 0xDA, 0x00, 0xFD, 0xFF, 0xFF, 0xFF,
					0x33, 0xC0, 0xC3
				};
				PatchXbeBytes(upd2VA, kPatch, sizeof(kPatch));
				GOLF_LOG("GolfPatch: Subsys update 2 (DA6B2C) patched at 0x%08X", (unsigned)upd2VA);
			}

			// sub_100BA0: updates DA6B30 (touch panel / card system, big state machine)
			// Pattern: CALL IsTestMode; TEST AL; JNZ far; MOV EAX,[DA6B30]; ADD EAX,3; CMP EAX,0Bh; JA far
			static const uint8_t kUpd3Pat[] = {
				0xE8, 0xFF, 0xFF, 0xFF, 0xFF,       // CALL IsTestMode
				0x84, 0xC0,                          // TEST AL, AL
				0x0F, 0x85, 0xC9, 0x01, 0x00, 0x00, // JNZ +0x1C9
				0xA1, 0x30, 0x6B, 0xDA, 0x00         // MOV EAX, [DA6B30]
			};
			uintptr_t upd3VA = ScanXbe(kUpd3Pat, sizeof(kUpd3Pat), imageSize);
			if (upd3VA) {
				// MOV dword [DA6B30], -3; XOR EAX,EAX; RET
				static const uint8_t kPatch[] = {
					0xC7, 0x05, 0x30, 0x6B, 0xDA, 0x00, 0xFD, 0xFF, 0xFF, 0xFF,
					0x33, 0xC0, 0xC3
				};
				PatchXbeBytes(upd3VA, kPatch, sizeof(kPatch));
				GOLF_LOG("GolfPatch: Subsys update 3 (DA6B30) patched at 0x%08X", (unsigned)upd3VA);
			}

			// sub_100B60: updates DA6B30 supplementary (called later in state 3)
			// Pattern: CALL IsTestMode; TEST AL; JNZ +18h; MOV EAX,[DA6B30]; TEST EAX,EAX; JL +0Fh
			static const uint8_t kUpd4Pat[] = {
				0xE8, 0xFF, 0xFF, 0xFF, 0xFF,   // CALL IsTestMode
				0x84, 0xC0,                      // TEST AL, AL
				0x75, 0x18,                      // JNZ +18h
				0xA1, 0x30, 0x6B, 0xDA, 0x00    // MOV EAX, [DA6B30]
			};
			uintptr_t upd4VA = ScanXbe(kUpd4Pat, sizeof(kUpd4Pat), imageSize);
			if (upd4VA) {
				static const uint8_t kRet = 0xC3;
				PatchXbeBytes(upd4VA, &kRet, 1);
				GOLF_LOG("GolfPatch: Subsys update 4 (DA6B30 supp) stubbed at 0x%08X", (unsigned)upd4VA);
			}
		}
	}
#endif

	GOLF_LOG("GolfPatch: pattern-based patches applied");

	// ── SGC Pro Tour (GDX-0010C) hardcoded patches ───────────────
	{
		// === Chihiro hardware state (emulating ready baseboard/DIMM) ===
		*(volatile int*)0x008EFAC8 = 1;       // baseboard init flag
		*(volatile int*)0x008EFAD8 = 4;       // DIMM state = ready
		*(volatile uint8_t*)0x008EFB08 = 1;   // device struct byte +0x14
		*(volatile uint8_t*)0x008EFB09 = 1;   // device struct byte +0x15
		*(volatile uint8_t*)0x008EFB74 = 1;   // media board identity config byte

		// === Boot into game mode ===
		*(volatile uint8_t*)0x0067D324 = 0;   // 0=game mode, 1=test mode

		// === Subsystem init status (hardware not present) ===
		*(volatile int*)0x007CAA78 = -3;      // touch panel: READY
		*(volatile int*)0x007CAA7C = -1;      // card system: OK
		*(volatile int*)0x007CAA80 = -1;      // network: OK
		*(volatile int*)0x007CAA84 = -2;      // database: OFF

		// === DHCP (no network hardware) ===
		*(volatile int*)0x007CAAA4 = 6;       // DHCP state = complete
		*(volatile int*)0x007CAAB4 = 1;       // DHCP done flag

		// === Touch panel subsystem ===
		*(volatile uint32_t*)0x00718E6C = 2;  // TP hardware ready

		// === Operator config (equivalent to test menu settings) ===
		*(volatile uint8_t*)0x005E8F34 |= 0x40;  // free play enabled

		// === Communication error suppress ===
		PatchDword(0x00299AE8, 0x00000000);

		// === sub_D4F10 (SYSTEM STARTUP state machine) ===
		// Stub to return 1 immediately. The guardian thread maintains
		// all hardware state vars so subsystem checks pass.
		{
			static const uint8_t kRet1[] = { 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3 };
			PatchXbeBytes(0x000D4F10, kRet1, sizeof(kRet1));
			GOLF_LOG("GolfPatch: sub_D4F10 (STARTUP) stubbed");
		}

		// === sub_8FC90 (mode transition fade handler) ===
		// The fade-out task blocks indefinitely in HLE mode.
		// Stub to return 0 so mode transitions proceed immediately.
		{
			static const uint8_t kRet0[] = { 0x33, 0xC0, 0xC3 };
			PatchXbeBytes(0x0008FC90, kRet0, sizeof(kRet0));
			GOLF_LOG("GolfPatch: sub_8FC90 (fade handler) stubbed");
		}

		// === Input hooks (keyboard + TeknoParrot shared memory) ===
		// Touch panel: hook both the reader (sub_C5F20) and frame processor (sub_C6D20).
		// sub_C6D20 computes state once per frame; sub_C5F20 returns it to all callers.
		// This emulates touch from inside the game's own functions.
		PatchWithJmp(0x000C5F20, (const void*)&GolfTouchStateHook);
		PatchWithJmp(0x000C6D20, (const void*)&GolfTouchProcessHook);
		PatchWithJmp(0x000831D0, (const void*)&GolfTestButtonHook);
		PatchWithJmp(0x000831F0, (const void*)&GolfCoinButtonHook);
		PatchWithJmp(0x00083220, (const void*)&GolfServiceButtonHook);
		GOLF_LOG("GolfPatch: Input hooks installed (TOUCH/TEST/COIN/SERVICE)");

		// === Touch panel coordinate calibration ===
		// sub_D63E0 maps raw touch coords (0-4095) to screen coords (0-799, 0-599)
		// using calibration data at 0x7CAB38-0x7CAB5C. Each DWORD packs two int16:
		//   low word = raw coordinate, high word = screen coordinate.
		// Since we bypass the serial processing chain, calibration is never set.
		// Without this, all coordinates transform to 0 and no buttons are detected.
		*(volatile int32_t*)0x007CAB38 = 0x00000000;           // X min: raw=0, screen=0
		*(volatile int32_t*)0x007CAB3C = 0x00000000;           // Y min: raw=0, screen=0
		*(volatile int32_t*)0x007CAB58 = (799 << 16) | 4095;   // X max: raw=4095, screen=799
		*(volatile int32_t*)0x007CAB5C = (599 << 16) | 4095;   // Y max: raw=4095, screen=599
		GOLF_LOG("GolfPatch: TP calibration set (0-4095 -> 0-799/599)");

		// === Coin system init ===
		*(volatile int32_t*)0x00677988 = 1;   // coin mode active
		*(volatile int32_t*)0x006779D4 = 100; // JAMMA counter past input gate threshold

		// === Credit system ===
		// Set free play flag in coin management library (byte_8F0007).
		// When 1: sub_149A60 always returns 2 (enough credits),
		// sub_149D50 returns NULL (hides credit text),
		// sub_149B00 won't deduct coins.
		*(volatile uint8_t*)0x008F0007 = 1;
		GOLF_LOG("GolfPatch: Free play enabled (byte_8F0007=1)");

		// (bit-4 NOP no longer needed — sub_C6D20 is fully replaced by GolfTouchProcessHook)

		// === Button guard fix for sub_28CA0 (character/course selection arrows) ===
		// sub_28CA0 resets byte_4E5CEA = 0 every frame (via MOV [4E5CEA], BL at VA 0x28D3D).
		// Combined with stale stack coordinates passing sub_E1DD0 hit-tests, this causes
		// arrows to fire every frame while the mouse is held. NOP the per-frame reset
		// so the guard persists across frames; GolfTouchProcessHook resets it on press.
		{
			static const uint8_t kNop6[] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
			PatchXbeBytes(0x00028D3D, kNop6, sizeof(kNop6));  // NOP byte_4E5CEA = 0
			GOLF_LOG("GolfPatch: NOP'd byte_4E5CEA per-frame reset in sub_28CA0");
		}

		// === Error display suppression ===
		// Stub the error check/display functions (redundant safety over 299AE8=0)
		{
			static const uint8_t kRet0[] = { 0x33, 0xC0, 0xC3 };
			PatchXbeBytes(0x000878F0, kRet0, sizeof(kRet0));  // comm error check
			PatchXbeBytes(0x00088530, kRet0, sizeof(kRet0));  // ERR_DSP draw
			PatchXbeBytes(0x000884C0, kRet0, sizeof(kRet0));  // ERR_DSP update
		}

		// === Card reader serial port init stub ===
		// sub_18A810 tries to init serial port for card reader hardware.
		// Stub to return 0 (success) so card system init chain completes.
		// Actual card I/O is handled by the ICCD state machine hook.
		{
			static const uint8_t kRet0[] = { 0x33, 0xC0, 0xC3 };
			PatchXbeBytes(0x0018A810, kRet0, sizeof(kRet0));
			GOLF_LOG("GolfPatch: sub_18A810 (card serial init) stubbed");
		}

		// === ICCD state machine hook (sub_82870) ===
		// Replaces the card reader serial protocol state machine with file-based I/O.
		// This is the root cause fix: instead of forcing dword_677968 in the guardian,
		// the ICCD RESET now completes naturally and sets dword_677968=1.
		// SEARCH loads card data from card_iccd.bin, WRITE saves to it.
		PatchWithJmp(0x00082870, (const void*)&GolfICCDStateMachineHook);
		GOLF_LOG("GolfPatch: ICCD state machine (sub_82870) hooked");

		// === Track validator (sub_24D20) ===
		// Hooked to return 1 (empty) or 2 (valid data) based on track contents.
		// The original sub_24D20 crashes when called — possibly due to sub_24C40's
		// __usercall calling convention or other emulation issues.
		// sub_24C40 has no global side effects (only writes to local output buffer),
		// so our hook returning the correct type is sufficient.
		PatchWithJmp(0x00024D20, (const void*)&GolfTrackValidatorHook);
		GOLF_LOG("GolfPatch: sub_24D20 (track validator) hooked → GolfTrackValidatorHook");

		// === Record parser (sub_24C40) ===
		// Hooked to fix CRC verification bug: original compares __int16 with _WORD,
		// causing CRCs with bit 15 set to fail due to sign extension mismatch.
		// Our hook uses proper uint16_t comparison.
		PatchWithJmp(0x00024C40, (const void*)&GolfRecordParserThunk);
		GOLF_LOG("GolfPatch: sub_24C40 (record parser) hooked → GolfRecordParserThunk");

		// === Card combination validator (sub_24DB0) ===
		// Force sub_24DB0 to return 1 (valid). Our track validator hook
		// returns correct types (1=empty, 2=data), so sub_24DB0 would
		// normally accept them. But running it natively is unnecessary
		// risk — its __usercall convention and error handlers (sub_24200)
		// can cause side effects. Force-return-1 is safe.
		{
			static const uint8_t kRet1[] = { 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3 };
			PatchXbeBytes(0x00024DB0, kRet1, sizeof(kRet1));
			GOLF_LOG("GolfPatch: sub_24DB0 (card combination) forced to return 1");
		}

		// === Card entry menu: swap USE CARD / RENEW CARD masks ===
		// In sub_711C0 case 2, the game's condition checks slot types:
		//   if (data+empty) → dword_5E99B4 |= 4  (disables USE CARD)
		//   else            → dword_5E99B4 |= 8  (disables RENEW CARD)
		// The mask bits are DISABLE flags (bit set = panel grayed out,
		// touch handler blocked). The game intends data+empty → RENEW,
		// but we want data+empty → USE CARD. Swap the immediates:
		//   VA 0x712D3: imm8 04 → 08  (now disables RENEW instead)
		//   VA 0x712E6: imm8 08 → 04  (now disables USE CARD instead)
		{
			static const uint8_t kDisableRenew = 0x08;
			static const uint8_t kDisableUse   = 0x04;
			PatchXbeBytes(0x000712D3, &kDisableRenew, 1);
			PatchXbeBytes(0x000712E6, &kDisableUse, 1);
			GOLF_LOG("GolfPatch: Card entry masks swapped (USE CARD enabled for data+empty)");
		}

		// === Reboot prevention ===
		{
			static const uint8_t kRet0[] = { 0x33, 0xC0, 0xC3 };
			PatchXbeBytes(0x000DF570, kRet0, sizeof(kRet0));  // reboot request check
			PatchXbeBytes(0x00085A10, kRet0, sizeof(kRet0));  // test program launcher
		}

		// === POWERON task registration ===
		// NOP writes to dword_7CAAC8 and dword_7CAADC in sub_D4010 so
		// POWERON task can register (populates runtime mode table).
		{
			uint8_t* fn = (uint8_t*)0x000D4010;
			auto nopWrite = [&](uint32_t addr, const char* name) {
				uint8_t addrBytes[4];
				memcpy(addrBytes, &addr, 4);
				for (int off = 0; off < 0x80; off++) {
					if (memcmp(fn + off, addrBytes, 4) == 0) {
						int instrStart = off - 1;
						int instrLen = 5;
						if (fn[off - 1] != 0xA3) {
							instrStart = off - 2;
							instrLen = 6;
						}
						PatchNop((uintptr_t)fn + instrStart, instrLen);
						GOLF_LOG("GolfPatch: NOP'd %s write at 0x%08X", name, (unsigned)(0x000D4010 + instrStart));
						return;
					}
				}
			};
			nopWrite(0x007CAAC8, "dword_7CAAC8");
			nopWrite(0x007CAADC, "dword_7CAADC");
		}

		// === Board type ===
		*(volatile uint32_t*)0x004C150C = 0;  // main board (not satellite)

		// === Guardian thread ===
		CreateThread(NULL, 0, GolfGuardianThread, NULL, 0, NULL);

		GOLF_LOG("GolfPatch: SGC Pro Tour patches applied");
	}
}
