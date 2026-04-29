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
#include "common/BetaConfig.h"
#include <cstdio>
#include <cstdarg>
#include <string>

// File-based media board command log (stdout may be buffered)
static FILE* g_mbLog = nullptr;
static void MbLog(const char* fmt, ...) {
    if (!g_mbLog) g_mbLog = fopen("C:\\temp\\mb_log.txt", "w");
    if (!g_mbLog) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(g_mbLog, fmt, ap);
    va_end(ap);
    fflush(g_mbLog);
}

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
    case 0x40F4: result = g_BetaConfig.mb_dimm_size ? 0x0002 : 0x03;
                desc = g_BetaConfig.mb_dimm_size ? "DIMM_SIZE(512MB)" : "DIMM_SIZE(1GB)";
                break;
    case 0x4026: result = 0;            desc = "HANDSHAKE(ack)";     break;
    case 0x40F0: result = g_BetaConfig.mb_board_type ? 0x0001 : 0x0000;
                desc = g_BetaConfig.mb_board_type ? "BOARD_TYPE(Type-3)" : "BOARD_TYPE(Type-1)";
                break;
    case 0x4084: result = 0;          desc = "UNK_4084";           break;
    default:
        MbLog("LpcRead UNKNOWN 0x%04X sz=%d\n", addr, size);
        EmuLog(LOG_LEVEL::WARNING, "LpcRead: Unknown addr 0x%04X size=%d", addr, size);
        return 0;
    }

    MbLog("LpcRead [0x%04X] -> 0x%04X (%s)\n", addr, result, desc);
    return result;
}

void MediaBoard::LpcWrite(uint32_t addr, uint32_t value, int size)
{
    static int write4026Count = 0;
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
        // from buffer_900000, re-deliver it now (AFTER the game's clear).
        temp_0x4026 = value;
        if (m_responsePending) {
            memcpy(buffer_900000, m_shadowResponse, 512);
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

    EmuLog(LOG_LEVEL::WARNING, "LpcWrite: Unknown addr 0x%04X = 0x%08X", addr, value);
}

void MediaBoard::ComRead(uint32_t offset, void* buffer, uint32_t length)
{
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

        // After init commands are done, the game clears read_buffer then
        // kicks 0x4026 expecting the media board to push a STATUS response.
        // Persistently refill the buffer with STATUS phase=5 after every
        // clear so the game sees it regardless of read timing.
        if (m_commandsProcessed >= 3) {
            // Check if the game just wrote zeros (clear operation)
            bool allZero = true;
            uint8_t* b = (uint8_t*)buffer;
            for (int i = 0; i < 16; i++) {
                if (b[i] != 0) { allZero = false; break; }
            }
            if (allZero) {
                uint16_t nextSeq = (uint16_t)(m_commandsProcessed + 1);
                *(uint16_t*)&buffer_900000[0] = nextSeq;
                buffer_900000[2] = 0x01;   // MAME ack
                buffer_900000[3] = 0x80;   // MAME ack
                *(uint32_t*)&buffer_900000[4] = 5;  // phase = READY
                *(uint32_t*)&buffer_900000[8] = 0;  // completion = 0%
                if (!m_statusInjected) {
                    m_statusInjected = true;
                    m_statusInjectRead = comWrite900000Diag;
                    MbLog("STATUS PERSIST: seq=%u phase=5 refilling buffer_900000 (first time)\n", nextSeq);
                }
            }
        }
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
            uint32_t sz = g_BetaConfig.mb_dimm_size ? (512 * ONE_MB) : (1024 * ONE_MB);
            *(uint32_t*)&outputBuffer[4] = sz;
            EmuLog(LOG_LEVEL::DEBUG, "  MB_CMD_DIMM_SIZE -> %u MB", sz / ONE_MB);
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
            *(uint32_t*)&outputBuffer[4] = MB_SYSTEM_TYPE_DEVELOPER | MB_SYSTEM_TYPE_GDROM;
            EmuLog(LOG_LEVEL::DEBUG, "  MB_CMD_SYSTEM_TYPE -> DEVELOPER|GDROM");
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

        HalSystemInterrupts[10].Assert(true);
        return;
    }

    EmuLog(LOG_LEVEL::WARNING, "ComWrite: Unknown offset 0x%08X length=%u", offset, length);
}
