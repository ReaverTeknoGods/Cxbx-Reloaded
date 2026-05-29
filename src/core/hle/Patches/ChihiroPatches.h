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
#ifndef CHIHIRO_PATCHES_H
#define CHIHIRO_PATCHES_H

#include <cstdint>

// Forward declaration for JVS game type (must match JvsIo.h)
enum class JvsGameType : uint8_t;

// Detection helpers
bool IsWanganXbe(uint32_t imageSize);
bool IsGundamXbe(uint64_t xbeHash);
bool IsCrazyTaxiXbe(uint64_t xbeHash);
bool IsGolfXbe(uint64_t xbeHash);
bool IsGolf2006Xbe(uint64_t xbeHash);
bool IsMahjongXbe(uint64_t xbeHash);

// Apply JVS watchdog suppression (Error 11/12) for all Chihiro games.
void ApplyJvsWatchdogPatch(uint32_t imageSize);

// Apply MediaBoard networking patches for Wangan Midnight Maximum Tune.
// Returns the detected JvsGameType (WanganMT1 or WanganMT2).
JvsGameType ApplyWanganPatches(uint32_t imageSize);

// Apply MediaBoard networking and init patches for Gundam Battle Operating Simulator.
void ApplyGundamPatches(uint64_t xbeHash, uint32_t imageSize);

// Apply CRI Sofdec spin loop fix for Crazy Taxi High Roller.
void ApplyCrazyTaxiPatches(uint64_t xbeHash, uint32_t imageSize);

// Apply MediaBoard/DIMM/D3D patches for Virtua Golf / Sega Golf Club.
void ApplyGolfPatches(uint64_t xbeHash, uint32_t imageSize);

// Apply MediaBoard/DIMM/D3D patches for Sega Golf Club 2006: Next Tours.
void ApplyGolf2006Patches(uint64_t xbeHash, uint32_t imageSize);

// Apply network patches for Sega Network Taisen Mahjong MJ2 / MJ3 / MJ3 Evolution.
void ApplyMahjongPatches(uint64_t xbeHash, uint32_t imageSize);

// Mahjong game type (set by ApplyMahjongPatches, read by D3D swap hook)
// 0=none, 1=MJ2, 2=MJ3, 3=MJ3Evo
extern int g_ChihiroMjGame;

// Per-game Xbox backbuffer override (set by game patches, read by Direct3D_CreateDevice).
// When non-zero, the Xbox presentation parameters are overridden to this size.
extern uint32_t g_ChihiroBackbufferOverrideW;
extern uint32_t g_ChihiroBackbufferOverrideH;

// Apply patches for OutRun 2 / OutRun 2 SP.
bool IsOutRun2Xbe(uint64_t xbeHash);
void ApplyOutRun2Patches(uint64_t xbeHash, uint32_t imageSize);

// Apply patches for Quest of D.
bool IsQuestOfDXbe(uint64_t xbeHash);
void ApplyQuestOfDPatches(uint64_t xbeHash, uint32_t imageSize);

#endif // CHIHIRO_PATCHES_H
