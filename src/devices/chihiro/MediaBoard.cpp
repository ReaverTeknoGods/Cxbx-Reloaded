// This is an open source non-commercial project. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
// ******************************************************************
// *   src->devices->chihiro->MediaBoard.cpp
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
// *  (c) 2019 Luke Usher
// *
// *  All rights reserved
// *
// ******************************************************************

#include "MediaBoard.h"
#include <cstdio>
#include <cstdarg>
#include <string>
#include <thread>
#include <algorithm>

// Set by QuestOfDPatches when the game XBE (not SEGABOOT) is loaded.
// Used to skip SEGABOOT-only logic (forced QuickReboot) in the game process.
extern bool g_QodGamePatchesActive;
#if defined(_DEBUG)
static FILE* g_mbLog = nullptr;
uint32_t s_type3TableBase = 0; // exported for state monitor
static void MbLog(const char* fmt, ...) {
    if (!g_mbLog) g_mbLog = fopen("C:\\temp\\mb_log.txt", "w");
    if (!g_mbLog) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(g_mbLog, fmt, ap);
    va_end(ap);
    fflush(g_mbLog);
}
#else
uint32_t s_type3TableBase = 0; // exported for state monitor
#define MbLog(...) do {} while (0)
#endif

#define _XBOXKRNL_DEFEXTRN_
#define LOG_PREFIX CXBXR_MODULE::JVS // TODO: XBAM


#include <core\kernel\exports\xboxkrnl.h>
#include "core\kernel\init\\CxbxKrnl.h"
#include "core\kernel\exports\EmuKrnl.h" // for HalSystemInterrupts

chihiro_bootid &MediaBoard::GetBootId()
{
    return BootID;
}

void MediaBoard::SetMountPath(std::string path)
{
    m_MountPath = path;

    // Load Boot.id from file
    FILE* bootidFile = fopen((path+"/boot.id").c_str(), "rb");
    if (bootidFile == nullptr) {
        CxbxrAbort("Could not open Chihiro boot.id");
    }
    fread(&BootID, 1, sizeof(chihiro_bootid), bootidFile);
    fclose(bootidFile);
}

uint32_t MediaBoard::LpcRead(uint32_t addr, int size)
{
    uint32_t result = 0;
    const char* desc = nullptr;

    switch (addr) {
    case 0x40E0: result = 1;          desc = "MB_ALIVE";           break;
    case 0x401E: result = 0x0317;     desc = "FW_VERSION";         break;
    case 0x4020: result = 0x00A0;     desc = "XBAM[0]";            break;
    case 0x4022: result = 0x4258;     desc = "XBAM[1] 'XB'";       break;
    case 0x4024: result = 0x4D41;     desc = "XBAM[2] 'MA'";       break;
    // Use the standard Type-1, 1 GB MediaBoard identity for supported games.
    // Per-install beta.ini overrides made otherwise compatible titles enter
    // the Type-3 network-boot path and wait indefinitely for a network image.
    case 0x40F4: result = 0x0003;     desc = "DIMM_SIZE(1GB)";
                break;
    case 0x4026: result = temp_0x4026;  desc = "HANDSHAKE(echo)";    break;
    case 0x40F0: result = 0x0000;     desc = "BOARD_TYPE(Type-1)";
                break;
    case 0x4084: result = 0;          desc = "UNK_4084";           break;
    case 0x4000:
        if (temp_0x4004 == 0xA0001E60) {
            result = fpga_aux_reg;
            desc = "FPGA_AUX";
        } else if (temp_0x4004 == 0xA0000000 && fpga_data_latch == 0x84000000 && fpga_response_read_index < fpga_response_word_count) {
            result = fpga_response_packet[fpga_response_read_index++];
            desc = "FPGA_RESPONSE";
            // After the last word is read, clear the response so subsequent
            // ISR drains (after reset) don't re-process stale data.
            if (fpga_response_read_index >= fpga_response_word_count) {
                memset(fpga_response_packet, 0, sizeof(fpga_response_packet));
                fpga_response_word_count = 0;
                fpga_response_read_index = 0;
            }
        } else if (temp_0x4004 == 0xA0000000 && fpga_data_latch == 0x84000020 && fpga_data_response_index < fpga_data_response_count) {
            result = fpga_data_response[fpga_data_response_index++];
            desc = "FPGA_DATA_RESPONSE";
            if (fpga_data_response_index >= fpga_data_response_count) {
                memset(fpga_data_response, 0, sizeof(fpga_data_response));
                fpga_data_response_count = 0;
                fpga_data_response_index = 0;
            }
        } else if (temp_0x4004 == 0xA0000000 && fpga_data_latch == 0x84000020 && fpga_mailbox_reply != 0) {
            result = fpga_mailbox_reply;
            fpga_mailbox_reply = 0;
            desc = "FPGA_MAILBOX_REPLY";
        } else if (temp_0x4004 == 0xA0000000 && (fpga_data_latch == 0x84000000 || fpga_data_latch == 0x84000020 || fpga_data_latch == 0x84000040)) {
            // No response data available — return 0 so HIWORD==0 terminates drain loops.
            // Without this, stale temp_0x4000 (e.g. 0x84000000) would have HIWORD!=0
            // causing the ROM's drain loop to spin forever.
            // 0x84000040 is the commit register — must also return 0 when idle.
            result = 0;
            desc = "FPGA_EMPTY";
        } else if ((temp_0x4004 == 0x90000000 || temp_0x4004 == 0x91000000) && sat_response_read_index < sat_response_count) {
            // Satellite channel response read (data already prepared)
            result = sat_response[sat_response_read_index++];
            desc = "SAT_RESPONSE";
            if (sat_response_read_index >= sat_response_count) {
                memset(sat_response, 0, sizeof(sat_response));
                sat_response_count = 0;
                sat_response_read_index = 0;
            }
        } else if ((temp_0x4004 == 0x90000000 || temp_0x4004 == 0x91000000) && sat_command_active && sat_command_word_count >= 8) {
            // Lazy response preparation: the drain reads 0 (no response),
            // then sub_3EB60 writes the command and reads the response.
            // We prepare the response NOW (on first response read after command).
            uint32_t ticket = sat_command[0] & 0xFFFF;
            MbLog("  SatResponse lazy prepare ticket=%u\n", ticket);
            memset(sat_response, 0, sizeof(sat_response));
            sat_response[0] = ticket | 0x00020000;
            sat_response[1] = 1; // success
            sat_response_count = 8;
            sat_response_read_index = 0;
            sat_command_active = false;
            result = sat_response[sat_response_read_index++];
            desc = "SAT_RESPONSE_LAZY";
        } else {
            result = temp_0x4000;
            desc = "FPGA_DATA";
        }
        break;
    case 0x4004: result = temp_0x4004; desc = "FPGA_CTRL";          break;
    default:
        MbLog("LpcRead UNKNOWN 0x%04X sz=%d\n", addr, size);
        EmuLog(LOG_LEVEL::WARNING, "LpcRead: Unknown addr 0x%04X size=%d", addr, size);
        return 0;
    }

    // Show ALL reads for debugging
    MbLog("LpcRead [0x%04X] -> 0x%04X (%s)\n", addr, result, desc);
    return result;
}

void MediaBoard::LpcWrite(uint32_t addr, uint32_t value, int size)
{
    static int write4026Count = 0;
    // Temporarily show ALL writes for debugging
    if (addr == 0x4026) {
        write4026Count++;
        if (write4026Count <= 5 || (write4026Count % 10000) == 0)
            MbLog("LpcWrite [0x4026] = 0x%08X (count=%d)\n", value, write4026Count);
    } else {
        MbLog("LpcWrite [0x%04X] = 0x%08X\n", addr, value);
    }
    EmuLog(LOG_LEVEL::DEBUG, "LpcWrite [0x%04X] = 0x%08X", addr, value);

    if (addr == 0x40E1) {
        EmuLog(LOG_LEVEL::DEBUG, "LpcWrite [0x40E1] deassert IRQ10");
        HalSystemInterrupts[10].Assert(false);
        return;
    }

    if (addr == 0x4026) {
        // Game writes here as part of baseboard handshake / "kick".
        // If we have a pending response that the game may have cleared
        // from buffer_900000, re-deliver it once.
        temp_0x4026 = value;
        if (m_responsePending) {
            memcpy(buffer_900000, m_shadowResponse, 512);
            m_responsePending = false;
            MbLog("LpcWrite [0x4026] re-delivered pending response, IRQ10\n");
            HalSystemInterrupts[10].Assert(true);
        }
        return;
    }

    if (addr == 0x408E) {
        // DMA configuration register — store value, no interrupt
        temp_0x408E = value;
        return;
    }

    if (addr == 0x4080) {
        // Command trigger register — the game writes here to initiate a media board
        // command/DMA transfer. On real hardware the media board would process the
        // command asynchronously and assert IRQ10 when done. We don't assert IRQ10
        // here because the ISR callback may write back to this port, creating an
        // infinite cascade. Instead, periodic IRQ10 in Timer.cpp drives the
        // interrupt→ISR→DPC→semaphore chain at a steady rate.
        temp_0x4080 = value;
        return;
    }

    if (addr == 0x4008) {
        // FPGA reset/clear register
        return;
    }

    if (addr == 0x4004) {
        // FPGA control register
        temp_0x4004 = value;
        if (value == 0xA0000040) {
            // Reset command — clear data, reset response read cursors.
            // Fully clear fpga_data_response so stale data can't trap drain loops.
            temp_0x4000 = 0;
            fpga_data_latch = 0;
            fpga_response_read_index = 0;
            fpga_data_response_index = 0;
            fpga_data_response_count = 0;
        } else if (value == 0xA0000020) {
            // Load data command — next write to 0x4000 sets data latch
        } else if (value == 0xA0000000) {
            // Execute/read command — several Type-3 BIOS probes only check bit 0.
            // 0x80000140 expects bit 0 set, while 0x80000160 expects bit 0 clear.
            uint32_t result = fpga_data_latch;
            if (fpga_data_latch == 0x80000140) {
                result = (result & ~1u) | (type3_status_ready ? 1u : 0u);
            } else if (fpga_data_latch == 0x80000160) {
                // Satellite link status: sub_3EFF0 case -87 checks bit 0.
                // bit 0 = 0 → sub_3EFF0 sets up standalone DMA and returns 0 (SUCCESS)
                // bit 0 = 1 → sub_3EFF0 returns 5 (FAILURE, satellite present but stuck)
                // We want standalone mode (no satellite), so bit 0 must be 0.
                result &= ~1u;
            } else if (fpga_data_latch == 0x84000020) {
                // For the execute-mode pre-load:
                // If there's a pending data response, serve it from the buffer.
                // Otherwise return 0 (idle, HIWORD=0).
                if (fpga_data_response_index < fpga_data_response_count) {
                    result = fpga_data_response[fpga_data_response_index];
                } else {
                    result = 0;
                }
            } else if (fpga_data_latch == 0x84000000 || fpga_data_latch == 0x84000040) {
                // The Type-3 BIOS polls this window until the returned high word clears.
                // Zero keeps the queue moving toward the media-board command path.
                // 0x84000040 is the commit trigger register — reading it should return 0 (idle).
                result = 0;
            }
            temp_0x4000 = result;
        } else if (value == 0xA0001E60) {
            // Preserve a small sideband register used by later Type-3 transfers.
            temp_0x4000 = fpga_aux_reg;
        }
        return;
    }

    if (addr == 0x4000) {
        // FPGA data register — store data
        temp_0x4000 = value;
        if (temp_0x4004 == 0xA0000020) {
            fpga_data_latch = value;
        } else if (temp_0x4004 == 0xA0001E60) {
            fpga_aux_reg = value;
        } else if ((temp_0x4004 & 0xFF000000) == 0x91000000) {
            // Satellite channel FPGA write (sideband mode, address 0x91000xxx)
            // Accumulate command words. First word with HIWORD!=0 starts a new command.
            if ((value & 0xFFFF0000u) != 0 && !sat_command_active) {
                memset(sat_command, 0, sizeof(sat_command));
                sat_command[0] = value;
                sat_command_word_count = 1;
                sat_command_active = true;
                MbLog("  SatCommand start id=0x%08X\n", value);
            } else if (sat_command_active && sat_command_word_count < 8) {
                sat_command[sat_command_word_count++] = value;
                if (sat_command_word_count >= 8) {
                    // Command complete — response will be prepared lazily on read
                    MbLog("  SatCommand complete ticket=%u (response deferred)\n",
                          sat_command[0] & 0xFFFF);
                }
            }
        } else if (temp_0x4004 == 0xA0000000) {
            if (fpga_data_latch == 0x80000140) {
                type3_status_ready = (value & 1u) != 0;
                MbLog("  Type3Ready set to %u via 0x80000140 write\n", type3_status_ready ? 1u : 0u);
            } else if (fpga_data_latch == 0x84000020) {
                if ((value & 0xFFFF0000u) != 0) {
                    // Non-zero HIWORD = event_type header (start of new mailbox packet).
                    // event_type 0x0001 = cmd, 0x0101 = version query, 0x0103 = status, etc.
                    fpga_mailbox_packet[0] = value;
                    fpga_mailbox_word_count = 1;
                    fpga_mailbox_reply = 0;
                    MbLog("  Type3Mailbox start id=0x%08X evtype=0x%04X\n", value, (value >> 16) & 0xFFFF);
                } else if (fpga_mailbox_word_count != 0 && fpga_mailbox_word_count < 8) {
                    fpga_mailbox_packet[fpga_mailbox_word_count++] = value;
                }
            } else if (fpga_data_latch == 0x84800000) {
                MbLog("  Type3Init write = 0x%08X\n", value);
            } else if (fpga_data_latch == 0x84000040) {
                if (value == 1 && fpga_mailbox_packet[0] != 0) {
                    const uint32_t channel = fpga_aux_reg == 0 ? 8 : fpga_aux_reg;
                    const uint32_t requestId = fpga_mailbox_packet[0] & 0xFF;
                    MbLog("  Type3Mailbox commit packet=0x%08X request=%u channel=%u words=%u\n",
                        fpga_mailbox_packet[0], requestId, channel, fpga_mailbox_word_count);
                    MbLog("  Type3Mailbox data:");
                    for (int pi = 0; pi < 8; pi++) MbLog(" %08X", fpga_mailbox_packet[pi]);
                    MbLog("\n");

                    // The ROM checks HIWORD(response[0]) == 0 for "completed".
                    // The 0x0001 prefix means "pending"; strip it so the response
                    // signals completion. Return the request ID in the low word only.
                    uint32_t completedId = fpga_mailbox_packet[0] & 0xFFFF;
                    fpga_mailbox_reply = completedId;

                    // Prepare the 8-DWORD response for reads from 0x84000000.
                    // sub_3F510 (ISR, called synchronously by sub_3F6B0) reads
                    // 8 DWORDs via sub_3EC40 into the entry at 698288 + 96*index.
                    // Then checks HIWORD of DWORD[0] (v7[1]):
                    //   - bit 15 set → calls sub_3E3C0 which CLEARS the entry!
                    //   - bit 15 clear → entry stays for game thread to read
                    // sub_40930 (game thread) checks:
                    //   - sub_3E5B0: entry WORD[1] != 0 (response present)
                    //   - WORD[1] == 0x8000 → error -3
                    //   - otherwise → success, reads DWORD[1] as result
                    // Use 0x0002: non-zero ✓, not 0x8000 ✓, bit 15 clear ✓
                    // This lets the entry survive the ISR so the game thread reads it.
                    memset(fpga_response_packet, 0, sizeof(fpga_response_packet));
                    fpga_response_packet[0] = (fpga_mailbox_packet[0] & 0xFFFF) | 0x00020000;
                    fpga_response_packet[1] = 1;  // completion status = success

                    // Evtype-specific response data in DWORD[2..7]
                    const uint16_t evtype = (uint16_t)(fpga_mailbox_packet[0] >> 16);
                    switch (evtype) {
                    case 0x0100: // NET_GetStatus — network status poll
                        fpga_response_packet[2] = 3; // state: online/ready
                        fpga_response_packet[3] = 0; // error: none
                        fpga_response_packet[4] = 0x0A000001; // IP: 10.0.0.1
                        fpga_response_packet[5] = 0xFFFFFF00; // subnet: 255.255.255.0
                        fpga_response_packet[6] = 0x0A000001; // gateway: 10.0.0.1
                        break;
                    case 0x0101: // NET_GetVersion — firmware version
                        fpga_response_packet[2] = 0x00020011; // version 2.17
                        fpga_response_packet[3] = 0x00000001; // build 1
                        break;
                    case 0x0102: // NET_SetConfig — returns success
                        fpga_response_packet[2] = 0; // success
                        break;
                    case 0x0103: // NET_GetConfig — config data
                        fpga_response_packet[2] = 1; // config flags (network enabled)
                        fpga_response_packet[3] = 1; // mode (DHCP)
                        break;
                    default:
                        break;
                    }

                    fpga_response_word_count = 8;
                    fpga_response_read_index = 0;

                    // Do NOT populate fpga_data_response — the drain loop
                    // (sub_3EEE0) will read fpga_mailbox_reply instead (set above).
                    // fpga_mailbox_reply has HIWORD=0 so the drain exits on first read.
                    // Populating fpga_data_response caused a race with the ISR thread
                    // where the 0xA0000040 reset kept resetting the index, serving stale
                    // data with HIWORD≠0 and trapping the drain in an infinite loop.
                    memset(fpga_data_response, 0, sizeof(fpga_data_response));
                    fpga_data_response_count = 0;
                    fpga_data_response_index = 0;

                    MbLog("  Type3Mailbox reply request=%u completedId=0x%08X\n", requestId, completedId);
                    // Reset word count so next packet starts fresh
                    fpga_mailbox_word_count = 0;

                    // Search Xbox memory for the event entry with matching ticket.
                    // The entry+32 area has: byte[0]=ticket, byte[1]=0, word[1]=STATUS,
                    // dword[1]=cmd. Use cached table base from first find.
                    // NOTE: The game XBE has SEGABOOT library code linked in, so
                    // the same entry table structure exists in the game process.
                    if (EmuInterruptList[10] && EmuInterruptList[10]->Connected) {
                        const uint8_t ticket = (uint8_t)(requestId & 0xFF);
                        static uint32_t s_tableBase32 = 0; // cached: first entry's +32 addr
                        s_type3TableBase = s_tableBase32; // export for state monitor
                        bool found = false;

                        // If table base known, search 16 entries at 96-byte stride
                        if (s_tableBase32 != 0) {
                            for (int i = 0; i < 16; i++) {
                                uint8_t* p = (uint8_t*)(uintptr_t)(s_tableBase32 + i * 96);
                                if (p[0] == ticket && p[1] == 0) {
                                    uint32_t* resp = (uint32_t*)(p - 32);
                                    // Dump entry BEFORE write (first 3 times)
                                    static int dumpCount = 0;
                                    if (dumpCount < 3) {
                                        MbLog("  Entry[%d] BEFORE write (96 bytes at 0x%08X):\n", i, (uint32_t)(uintptr_t)resp);
                                        for (int d = 0; d < 96; d += 4) {
                                            MbLog("    +%02d: 0x%08X\n", d, *(uint32_t*)(((uint8_t*)resp) + d));
                                        }
                                    }
                                    for (int j = 0; j < 8; j++) resp[j] = fpga_response_packet[j];
                                    if (dumpCount < 3) {
                                        MbLog("  Entry[%d] AFTER write (96 bytes at 0x%08X):\n", i, (uint32_t)(uintptr_t)resp);
                                        for (int d = 0; d < 96; d += 4) {
                                            MbLog("    +%02d: 0x%08X\n", d, *(uint32_t*)(((uint8_t*)resp) + d));
                                        }
                                        dumpCount++;
                                    }
                                    MbLog("  Direct write entry[%d] at 0x%08X (ticket=%u resp=0x%08X)\n",
                                          i, (uint32_t)(uintptr_t)resp, ticket, resp[0]);
                                    found = true;
                                    break;
                                }
                            }
                        }

                        // First time or table miss: full scan with strict pattern
                        if (!found) {
                            uint32_t intObjAddr = (uint32_t)(uintptr_t)EmuInterruptList[10]->ServiceContext;
                            uint32_t scanStart = (intObjAddr > 0x60000) ? intObjAddr - 0x60000 : 0x10000;
                            uint32_t scanEnd = intObjAddr + 0x10000;
                            uint16_t status = (uint16_t)(fpga_mailbox_packet[0] >> 16); // HIWORD
                            uint32_t cmd = fpga_mailbox_packet[1];
                            for (uint32_t addr = scanStart; addr < scanEnd; addr += 4) {
                                uint8_t* p = (uint8_t*)(uintptr_t)addr;
                                if (p[0] == ticket && p[1] == 0 &&
                                    *(uint16_t*)(p+2) == status &&
                                    *(uint32_t*)(p+4) == cmd) {
                                    // Calibrate table base: entry[i]+32 = addr, so base = addr - i*96
                                    // Since we don't know i, assume this is entry 0 (first free slot)
                                    // and verify by checking nearby entries
                                    // For now just align to 96 from the found address
                                    // Find entry index by checking if addr - n*96 is plausible
                                    uint32_t testBase = addr;
                                    // Walk backwards to find the real entry[0]
                                    for (int n = 0; n < 16; n++) {
                                        uint32_t candidate = addr - n * 96;
                                        // Check if candidate-32..candidate+64 is in a reasonable range
                                        testBase = candidate;
                                        // Just use the first found position
                                        break;
                                    }
                                    s_tableBase32 = testBase;
                                    uint32_t* resp = (uint32_t*)(p - 32);
                                    for (int j = 0; j < 8; j++) resp[j] = fpga_response_packet[j];
                                    MbLog("  Scan+write at 0x%08X (ticket=%u resp=0x%08X base=0x%08X)\n",
                                          addr, ticket, resp[0], s_tableBase32);
                                    found = true;
                                    break;
                                }
                            }
                        }
                        if (!found) {
                            MbLog("  WARNING: No entry for ticket=%u\n", ticket);
                        }

                        // After locating the entry table, prevent the satellite path
                        // (sub_3D7C0) from creating a satellite channel. sub_3D7C0
                        // creates channel 0xA9 when dword_A6DE8[3630] < 2 (count of
                        // registered channels). Setting it to 2 makes sub_3D7C0
                        // return 0 → state 3 goes directly to state 4 (READY).
                        // Address: s_tableBase32 = &dword_A6DE8[14792]
                        //   dword_A6DE8[3630] = s_tableBase32 - (14792-3630)*4
                        if (s_tableBase32 != 0) {
                            static bool channelCountPatched = false;
                            if (!channelCountPatched) {
                                uint32_t* pCount = (uint32_t*)(uintptr_t)(s_tableBase32 - (14792 - 3630) * 4);
                                uint32_t oldCount = *pCount;
                                if (oldCount < 2) {
                                    *pCount = 2;
                                    MbLog("  Patched channel count at 0x%08X: %u -> 2\n",
                                          (uint32_t)(uintptr_t)pCount, oldCount);
                                }
                                channelCountPatched = true;
                            }

                            // Patch the boot struct so sub_44330 proceeds to
                            // reboot into the game XBE. sub_44330 checks:
                            //   *(WORD*)(bootStruct+24): 2 or 3 → GDROM mode → boot
                            //   otherwise checks *(DWORD*)(bootStruct+28) != 0
                            // For Type-3 both fields are 0 (never populated by
                            // network boot), so sub_44330 returns without booting.
                            // Set field+24 = 2 (GDROM mode) to unblock.
                            // bootStruct = dword_A6DE8[3639] = sub_3DA60() = 698052
                            static bool bootStructPatched = false;
                            if (!bootStructPatched) {
                                uint16_t* pBootMode = (uint16_t*)(uintptr_t)(s_tableBase32 - (14792 - 3639) * 4 + 24);
                                uint16_t oldMode = *pBootMode;
                                *pBootMode = 2; // GDROM mode
                                MbLog("  Patched boot struct +24 at 0x%08X: %u -> 2 (GDROM mode)\n",
                                      (uint32_t)(uintptr_t)pBootMode, oldMode);
                                bootStructPatched = true;
                            }
                        }

                        // State monitor + forced boot: sub_40500 fails for Type-3
                        // (SEGABOOT ROM doesn't fully init the state machine).
                        // After enough polls, force fb40=1 and netState=4 so that
                        // sub_44400 runs and sub_44330 triggers the QuickReboot.
                        if (s_tableBase32 != 0) {
                            static int monitorCount = 0;
                            uint32_t* pState = (uint32_t*)(uintptr_t)(s_tableBase32 - (14792 - 4144) * 4);
                            uint32_t* pFB40 = (uint32_t*)(uintptr_t)(s_tableBase32 - (14792 - 4140) * 4);

                            if ((monitorCount % 50) == 0) {
                                uint16_t* pBootMode = (uint16_t*)(uintptr_t)(s_tableBase32 - (14792 - 3639) * 4 + 24);
                                uint32_t* pBoot28 = (uint32_t*)(uintptr_t)(s_tableBase32 - (14792 - 3639) * 4 + 28);
                                uint32_t* pCallback = (uint32_t*)(uintptr_t)(s_tableBase32 - (14792 - 4468) * 4);
                                MbLog("  STATE: netState=%u fb40=%u bootMode=%u boot28=0x%08X cb=0x%08X\n",
                                      *pState, *pFB40, *pBootMode, *pBoot28, *pCallback);
                            }

                            // After 10 polls (~0.5s), force-enable the main loop
                            // and set state to "network ready" so sub_44330 fires.
                            // If that doesn't work (callback never registered),
                            // directly trigger QuickReboot after 30 polls.
                            static bool stateForced = false;
                            static bool rebootTriggered = false;
                            if (!stateForced && monitorCount >= 10) {
                                if (*pFB40 == 0 || *pState < 4) {
                                    *pFB40 = 1;
                                    *pState = 4;
                                    MbLog("  FORCED: fb40=1, netState=4 (was %u/%u)\n",
                                          *pFB40, *pState);
                                    stateForced = true;
                                }
                            }
                            if (stateForced && !rebootTriggered && monitorCount >= 30) {
                                // sub_44400 callback was never registered — trigger
                                // QuickReboot directly. HalReturnToFirmware now
                                // handles NULL LaunchDataPage for Chihiro.
                                // ONLY for SEGABOOT — in the game process the
                                // library doesn't need another QuickReboot.
                                if (!g_QodGamePatchesActive) {
                                    MbLog("  Direct QuickReboot: sub_44400 not registered, forcing reboot\n");
                                    rebootTriggered = true;
                                    // Spawn a thread since HalReturnToFirmware is noreturn
                                    std::thread([]() {
                                        xbox::HalReturnToFirmware(xbox::ReturnFirmwareQuickReboot);
                                    }).detach();
                                } else {
                                    MbLog("  Skipping QuickReboot (game process)\n");
                                    rebootTriggered = true;
                                }
                            }
                            monitorCount++;
                        }
                    }

                    // For SEGABOOT: do NOT assert IRQ10 — the direct memory
                    // write above already placed the response in the entry's [0..31]
                    // area. The game thread checks sub_40930 → sub_3E5B0 which reads
                    // entry[2..3] (HIWORD of response DWORD[0]) → sees 0x8002 (non-zero)
                    // → returns TRUE → sub_40930 returns 0 (success).
                    //
                    // For the GAME process: the direct memory writes are skipped (wrong
                    // offsets for game memory layout). Instead, fire IRQ10 so the game's
                    // own ISR (sub_3F510) reads fpga_response_packet via 0x84000000
                    // and stores it in the entry table. Our response has HIWORD=0x0002
                    // (bit 15 clear), so sub_3E3C0 won't clear the entry — no race.
                    if (g_QodGamePatchesActive) {
                        HalSystemInterrupts[10].Assert(true);
                    }
                }
            }
        }
        return;
    }

    EmuLog(LOG_LEVEL::WARNING, "LpcWrite: Unknown addr 0x%04X = 0x%08X", addr, value);
}

void MediaBoard::ComRead(uint32_t offset, void* buffer, uint32_t length)
{
    // Quest of D's unsupported Type-3 path uses offset zero when its DRAM base
    // is unset. Supported titles retain the standard 0x900000/0x900200 path.
    if (offset == 0x00000000 && g_QodGamePatchesActive) {
        static int comRead0Count = 0;
        comRead0Count++;
        if (comRead0Count <= 5 || (comRead0Count % 10000) == 0)
            MbLog("ComRead [0x00000000] len=%u (count=%d)\n", length, comRead0Count);
        memcpy(buffer, buffer_offset0, std::min(length, (uint32_t)sizeof(buffer_offset0)));
        return;
    }

    static int comRead900000Count = 0;
    if (offset == 0x900000) {
        comRead900000Count++;
        if (comRead900000Count <= 5 || (comRead900000Count % 10000) == 0)
            MbLog("ComRead [0x%08X] len=%u (count=%d)\n", offset, length, comRead900000Count);
    } else {
        MbLog("ComRead [0x%08X] len=%u\n", offset, length);
    }

    if (offset == 0x00D00000) {
        EmuLog(LOG_LEVEL::DEBUG, "ComRead  [0xD00000] length=%u (ignored)", length);
        return;
    }

    if (offset == 0x005FFCE9) {
        EmuLog(LOG_LEVEL::DEBUG, "ComRead  [0x5FFCE9] -> 0x10000 (DMA size/flag)");
        memset(buffer, 0, length);
        *(uint32_t*)buffer = 0x10000;
        return;
    }

    if (offset == 0x800000) {
        memcpy(buffer, buffer_800000, length);
        return;
    }
    if (offset == 0x800200) {
        memcpy(buffer, buffer_800200, length);
        return;
    }

    // MAME: read_sector(LBA 0x4800) → read_buffer (response)
    if (offset == 0x900000) {
        // If a response is pending (game may have cleared buffer_900000),
        // re-deliver it now before the game reads.
        if (m_responsePending) {
            memcpy(buffer_900000, m_shadowResponse, 512);
            m_responsePending = false;
        }

        memcpy(buffer, buffer_900000, length);
        static int comRead900000Diag = 0;
        comRead900000Diag++;
        if (comRead900000Diag <= 10 || (m_statusInjected && comRead900000Diag <= (m_statusInjectRead + 5))) {
            MbLog("  ComRead900000 #%d first 32 bytes:", comRead900000Diag);
            uint8_t* b = (uint8_t*)buffer;
            for (int i = 0; i < 32; i++) MbLog(" %02X", b[i]);
            MbLog("\n");
        }
        return;
    }
    // MAME: read_sector(LBA 0x4801) → write_buffer (command, cleared after processing)
    if (offset == 0x900200) {
        memcpy(buffer, buffer_900200, length);
        return;
    }

    EmuLog(LOG_LEVEL::WARNING, "ComRead: Unknown offset 0x%08X length=%u", offset, length);
    memset(buffer, 0, length);
}

void MediaBoard::ComWrite(uint32_t offset, void* buffer, uint32_t length)
{
    // Offset-zero command/response mailbox is reserved for Quest of D's
    // unsupported Type-3 path; normal games use the standard DIMM channels.
    if (offset == 0x00000000 && g_QodGamePatchesActive) {
        static int comWrite0Count = 0;
        comWrite0Count++;
        if (comWrite0Count <= 5 || (comWrite0Count % 10000) == 0)
            MbLog("ComWrite [0x00000000] len=%u (count=%d)\n", length, comWrite0Count);

        // Store in the shared buffer
        memcpy(buffer_offset0, buffer, std::min(length, (uint32_t)sizeof(buffer_offset0)));

        // Check if this is a command (seq+cmd in first 4 bytes)
        uint8_t* inputBuffer = (uint8_t*)buffer_offset0;
        uint16_t seq     = *(uint16_t*)&inputBuffer[0];
        uint16_t command = *(uint16_t*)&inputBuffer[2];

        if (seq == 0 && command == 0) {
            // Game is clearing/ack — just store, no processing
            return;
        }

        // Process as command: copy to buffer_900200 and trigger the 0x900200 handler
        memcpy(buffer_900200, buffer, std::min(length, (uint32_t)512u));
        // Re-route to offset 0x900200 processing (will write response to buffer_900000)
        offset = 0x900200;
        m_offset0Pending = true;
        // Fall through to the 0x900200 handler below, which will process the command,
        // generate a response in buffer_900000, and assert IRQ10.
        // After it returns, copy the response back to buffer_offset0 for the game to read.
    }

    static int comWrite900000Count = 0;
    if (offset == 0x900000) {
        comWrite900000Count++;
        if (comWrite900000Count <= 5 || (comWrite900000Count % 10000) == 0)
            MbLog("ComWrite [0x%08X] len=%u (count=%d)\n", offset, length, comWrite900000Count);
    } else {
        MbLog("ComWrite [0x%08X] len=%u\n", offset, length);
    }

    if (offset == 0x005FFCE9) {
        EmuLog(LOG_LEVEL::DEBUG, "ComWrite [0x5FFCE9] assert IRQ10");
        HalSystemInterrupts[10].Assert(true);
        return;
    }

    // Network-related command channels
    if (offset == 0x800000) {
        EmuLog(LOG_LEVEL::DEBUG, "ComWrite [0x800000] net-cmd write length=%u", length);
        memcpy(buffer_800000, buffer, length);
        return;
    }
    if (offset == 0x800200) {
        // Input: string of an IP address — respond with a fixed local IP
        EmuLog(LOG_LEVEL::DEBUG, "ComWrite [0x800200] net IP resolve request: \"%.*s\" -> 10.0.0.1", length, (char*)buffer);
        memcpy(buffer_800200, buffer, length);
        uint8_t* outputBuffer = (uint8_t*)buffer_800000;
        *(uint32_t*)outputBuffer = 167772161; // 10.0.0.1
        // Clear command bytes like MAME
        buffer_800200[0] = buffer_800200[1] = buffer_800200[2] = buffer_800200[3] = 0;
        HalSystemInterrupts[10].Assert(true);
        return;
    }

    // MAME: write_sector(LBA 0x4800) just stores data, no IRQ
    if (offset == 0x900000) {
        static int comWrite900000Diag = 0;
        comWrite900000Diag++;
        if (comWrite900000Diag <= 10) {
            MbLog("  ComWrite900000 #%d first 32 bytes:", comWrite900000Diag);
            uint8_t* b = (uint8_t*)buffer;
            for (int i = 0; i < 32; i++) MbLog(" %02X", b[i]);
            MbLog("\n");
        }
        EmuLog(LOG_LEVEL::DEBUG, "ComWrite [0x900000] sys-cmd write length=%u", length);
        memcpy(buffer_900000, buffer, length);

        // MAME: write_sector(LBA 0x4800) just stores data, no IRQ.
        // Do NOT inject proactive STATUS here — let the firmware drive
        // the protocol naturally via ComWrite(0x900200).
        return;
    }

    if (offset == 0x900200) {
        // Store incoming command in write_buffer (MAME: write_sector LBA 0x4801)
        memcpy(buffer_900200, buffer, length);
        
        uint8_t* inputBuffer  = (uint8_t*)buffer_900200;
        // Write response to shadow buffer (NOT directly to buffer_900000)
        // because the game may clear buffer_900000 before reading it.
        uint8_t* outputBuffer = (uint8_t*)m_shadowResponse;
        memset(m_shadowResponse, 0, sizeof(m_shadowResponse));

        uint16_t seq     = *(uint16_t*)&inputBuffer[0];
        uint32_t command = *(uint16_t*)&inputBuffer[2];

        // MAME check: skip if both seq bytes are zero (no command pending)
        if (seq == 0 && command == 0) {
            MbLog("  ComWrite900200 seq=0 cmd=0 (skipped) first8:");
            for (int i = 0; i < 8; i++) MbLog(" %02X", inputBuffer[i]);
            MbLog("\n");
            return;
        }

        EmuLog(LOG_LEVEL::DEBUG, "ComWrite [0x900200] cmd=0x%04X seq=0x%04X length=%u", command, seq, length);
        MbLog("  CMD 0x%04X seq=0x%04X len=%u\n", command, seq, length);
        // Hex dump first 32 bytes of input
        {
            MbLog("  IN:");
            for (uint32_t i = 0; i < length && i < 32; i++) MbLog(" %02X", inputBuffer[i]);
            MbLog("\n");
        }

        switch (command) {
        case MB_CMD_DIMM_SIZE: { // 0x0001
            // MAME returns 0x00f00000 (partition-related size value)
            *(uint32_t*)&outputBuffer[4] = 0x00f00000;
            EmuLog(LOG_LEVEL::DEBUG, "  MB_CMD_DIMM_SIZE -> 0x00f00000");
            break;
        }
        case MB_CMD_STATUS: { // 0x0100
            *(uint32_t*)&outputBuffer[4] = MB_STATUS_READY;
            *(uint32_t*)&outputBuffer[8] = 0;
            EmuLog(LOG_LEVEL::DEBUG, "  MB_CMD_STATUS -> READY (phase=5, completion=0)");
            break;
        }
        case MB_CMD_FIRMWARE_VERSION: { // 0x0101
            *(uint32_t*)&outputBuffer[4] = 0x0317;
            EmuLog(LOG_LEVEL::DEBUG, "  MB_CMD_FIRMWARE_VERSION -> 0x0317");
            break;
        }
        case MB_CMD_SYSTEM_TYPE: { // 0x0102
            *(uint32_t*)&outputBuffer[4] = 0; // MAME returns 0; bit 16 = develop mode
            EmuLog(LOG_LEVEL::DEBUG, "  MB_CMD_SYSTEM_TYPE -> 0 (normal)");
            break;
        }
        case MB_CMD_SERIAL_NUMBER: { // 0x0103
            memcpy(&outputBuffer[4], "A89E-25A47354512", 17);
            EmuLog(LOG_LEVEL::DEBUG, "  MB_CMD_SERIAL_NUMBER -> A89E-25A47354512");
            break;
        }
        case 0x0104: {
            *(uint32_t*)&outputBuffer[4] = 0;
            EmuLog(LOG_LEVEL::DEBUG, "  CMD_0104 -> 0");
            break;
        }
        case MB_CMD_HARDWARE_TEST: { // 0x0301
            uint32_t testType       = *(uint32_t*)&inputBuffer[4];
            xbox::addr_xt resultPtr = *(uint32_t*)&inputBuffer[8];
            *(uint32_t*)&outputBuffer[4] = testType;
            EmuLog(LOG_LEVEL::DEBUG, "  MB_CMD_HARDWARE_TEST type=0x%X resultPtr=0x%08X -> TEST OK", testType, (uint32_t)resultPtr);
            memcpy((void*)resultPtr, "TEST OK", 8);
        } break;
        case 0x0204: {
            *(uint32_t*)&outputBuffer[4] = 0;
            EmuLog(LOG_LEVEL::DEBUG, "  CMD_0204 -> 0");
            break;
        }
        case 0x0415: {
            *(uint32_t*)&outputBuffer[4] = 167772161; // 10.0.0.1
            EmuLog(LOG_LEVEL::DEBUG, "  CMD_0415 (net IP?) -> 10.0.0.1");
            break;
        }
        case 0x0601: {
            *(uint32_t*)&outputBuffer[4] = 0;
            EmuLog(LOG_LEVEL::DEBUG, "  CMD_0601 (net init?) -> 0");
            break;
        }
        case 0x0602: {
            *(uint32_t*)&outputBuffer[4] = 0xFFFF;
            EmuLog(LOG_LEVEL::DEBUG, "  CMD_0602 -> 0xFFFF");
            break;
        }
        case 0x0605: {
            *(uint32_t*)&outputBuffer[4] = 0;
            EmuLog(LOG_LEVEL::DEBUG, "  CMD_0605 -> 0");
            break;
        }
        case 0x0606: {
            *(uint32_t*)&outputBuffer[4] = 0;
            EmuLog(LOG_LEVEL::DEBUG, "  CMD_0606 (net?) -> 0");
            break;
        }
        case 0x0607: {
            *(uint32_t*)&outputBuffer[4] = 0;
            *(uint32_t*)&outputBuffer[8] = 0;
            EmuLog(LOG_LEVEL::DEBUG, "  CMD_0607 -> 0,0");
            break;
        }
        case 0x0608: {
            *(uint32_t*)&outputBuffer[4] = 167772161; // 10.0.0.1
            EmuLog(LOG_LEVEL::DEBUG, "  CMD_0608 (net IP?) -> 10.0.0.1");
            break;
        }
        default: {
            MbLog("  UNHANDLED cmd 0x%04X!\n", command);
            EmuLog(LOG_LEVEL::WARNING, "  Unhandled MediaBoard command 0x%04X", command);
            break;
        }
        }

        // MAME response header: seq + hardcoded 0x8001
        *(uint16_t*)&outputBuffer[0] = seq;  // echo sequence number
        outputBuffer[2] = 0x01;              // MAME hardcoded
        outputBuffer[3] = 0x80;              // MAME hardcoded

        // Also write to buffer_900000 immediately (for games that read without kick)
        memcpy(buffer_900000, m_shadowResponse, 512);
        m_responsePending = true;

        MbLog("  RESP: seq=0x%04X ack=0x8001 data:", seq);
        for (int i = 0; i < 20; i++) MbLog(" %02X", outputBuffer[i]);
        MbLog("\n");

        // MAME: clear write_buffer[0:3] after processing (signals command consumed)
        inputBuffer[0] = inputBuffer[1] = inputBuffer[2] = inputBuffer[3] = 0;

        m_commandsProcessed++;

        // If this command came from offset 0, copy response back to shared buffer
        if (m_offset0Pending) {
            memcpy(buffer_offset0, m_shadowResponse, sizeof(buffer_offset0));
            m_offset0Pending = false;
            MbLog("  Copied response to buffer_offset0\n");
        }

        HalSystemInterrupts[10].Assert(true);
        return;
    }

    EmuLog(LOG_LEVEL::WARNING, "ComWrite: Unknown offset 0x%08X length=%u", offset, length);
}
