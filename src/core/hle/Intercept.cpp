// This is an open source non-commercial project. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
// ******************************************************************
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
// *  (c) 2002-2003 Aaron Robinson <caustik@caustik.com>
// *  (c) 2016-2018 Luke Usher <luke.usher@outlook.com>
// *  (c) 2016-2018 Patrick van Logchem <pvanlogchem@gmail.com>
// *  (c) 2017-2018 RadWolfie
// *  (c) 2017-2018 jarupxx
// *  (c) 2018 x1nixmzeng
// *
// *  All rights reserved
// *
// ******************************************************************

#define LOG_PREFIX CXBXR_MODULE::HLE

#include <cmath>
#include <iomanip> // For std::setfill and std::setw
#include <filesystem>

#include "core\kernel\init\CxbxKrnl.h"
#include "core\kernel\support\Emu.h"
#include "EmuShared.h"
#include "common\CxbxDebugger.h"
#include "Logging.h"
#include <libXbSymbolDatabase.h>
#include "Intercept.hpp"
#include "Patches.hpp"
#include "common\util\hasher.h"
#include "common/FilePaths.hpp"

#include <Shlwapi.h>
#include <shlobj.h>
#include <unordered_map>
#include <map>
#include <sstream>
#include <clocale>

static const char* section_info = "Info";
static struct {
	const char* SymbolDatabaseVersionHash = "SymbolDatabaseVersionHash";
} sect_info_keys;

static const char* section_certificate = "Certificate";
static struct {
	const char* Name = "Name";
	const char* TitleID = "TitleID";
	const char* TitleIDHex = "TitleIDHex";
	const char* Region = "Region";
	const char* RegionHex = "RegionHex";
	const char* Version = "Version";
	const char* VersionHex = "VersionHex";
} sect_certificate_keys;

static const char* section_libs = "Libs";
static struct {
	const char* BuildVersion = "BuildVersion";
} sect_libs_keys;

static const char* section_symbols = "Symbols";

std::map<std::string, xbox::addr_xt> g_SymbolAddresses;
bool g_SymbolCacheUsed = false;

bool bLLE_APU = false; // Set this to true for experimental APU (sound) LLE
bool bLLE_GPU = false; // Set this to true for experimental GPU (graphics) LLE
bool bLLE_USB = false; // Set this to true for experimental USB (input) LLE
bool bLLE_JIT = false; // Set this to true for experimental JIT

void* GetXboxSymbolPointer(std::string symbolName)
{
    auto symbol = g_SymbolAddresses.find(symbolName);
    if (symbol != g_SymbolAddresses.end()) {
        return (void*)symbol->second;
    }

    return nullptr;
}

void* GetXboxFunctionPointer(std::string functionName)
{
	void* ptr = GetPatchedFunctionTrampoline(functionName);
	if (ptr != nullptr) {
		return ptr;
	}

    // If we got here, the function wasn't patched, so we can just look it up the symbol cache
    // and return the correct offset
    return GetXboxSymbolPointer(functionName);
}

// NOTE: GetDetectedSymbolName do not get to be in XbSymbolDatabase, get symbol string in Cxbx project only.
std::string GetDetectedSymbolName(const xbox::addr_xt address, int * const symbolOffset)
{
    std::string result = "";
    int closestMatch = MAXINT;

    for (auto it = g_SymbolAddresses.begin(); it != g_SymbolAddresses.end(); ++it) {
        xbox::addr_xt symbolAddr = it->second;
        if (symbolAddr == xbox::zero)
            continue;

        if (symbolAddr <= address)
        {
            int distance = address - symbolAddr;
            if (closestMatch > distance)
            {
                closestMatch = distance;
                result = it->first;
            }
        }
    }

    if (closestMatch < MAXINT)
    {
        *symbolOffset = closestMatch;
        return result;
    }

    *symbolOffset = 0;
    return "unknown";
}

// NOTE: VerifySymbolAddressAgainstXRef do not get to be in XbSymbolDatabase, perform verification in Cxbx project only.
/*
bool VerifySymbolAddressAgainstXRef(char *SymbolName, xbox::addr_xt Address, int XRef)
{
    // Temporary verification - is XREF_D3DTSS_TEXCOORDINDEX derived correctly?
    // TODO : Remove this when XREF_D3DTSS_TEXCOORDINDEX derivation is deemed stable
    xbox::addr_xt XRefAddr = XRefDataBase[XRef];
    if (XRefAddr == Address)
        return true;

    if (XRefAddr == XREF_ADDR_DERIVE) {
        printf("HLE: XRef #%d derived 0x%.08X -> %s\n", XRef, Address, SymbolName);
        XRefDataBase[XRef] = Address;
        return true;
    }

    PopupCustom(LOG_LEVEL::WARNING, CxbxMsgDlgIcon_Warn,
		"Verification of %s failed : XREF was 0x%.8X while lookup gave 0x%.8X", SymbolName, XRefAddr, Address);
    // test case : Kabuki Warriors (for XREF_D3DTSS_TEXCOORDINDEX)
    return false;
}*/

// x1nixmzeng: Hack to notify CxbxDebugger of the SymbolCache file, which is currently a hashed XBE header AND stripped title (see EmuHLEIntercept)
class CxbxDebuggerScopedMessage
{
    std::string& message;

    CxbxDebuggerScopedMessage() = delete;
    CxbxDebuggerScopedMessage(const CxbxDebuggerScopedMessage&) = delete;
public:

    CxbxDebuggerScopedMessage(std::string& message_string)
        : message(message_string)
    { }

    ~CxbxDebuggerScopedMessage()
    {
        if (CxbxDebugger::CanReport())
        {
            CxbxDebugger::ReportHLECacheFile(message.c_str());
        }
    }
};

void CDECL EmuOutputMessage(xb_output_message mFlag, 
                            const char* message)
{
    switch (mFlag) {
        case XB_OUTPUT_MESSAGE_INFO: {
            printf("%s\n", message);
            break;
        }
        case XB_OUTPUT_MESSAGE_WARN: {
            EmuLog(LOG_LEVEL::WARNING, "%s", message);
            break;
        }
        case XB_OUTPUT_MESSAGE_ERROR: {
            CxbxrAbort("%s", message);
            break;
        }
        case XB_OUTPUT_MESSAGE_DEBUG:
        default: {
            EmuLog(LOG_LEVEL::DEBUG, "%s", message);
            break;
        }
    }
}

void CDECL EmuRegisterSymbol(const char* library_str,
                             uint32_t library_flag,
                             uint32_t xref_index,
                             const char* symbol_str,
                             xbaddr symbol_addr,
                             uint32_t build_version,
                             uint32_t symbol_type,
                             uint32_t call_type,
                             uint32_t param_count,
                             const XbSDBSymbolParam* param_list)
{
    // Ignore registered symbol in current database.
    uint32_t hasSymbol = g_SymbolAddresses[symbol_str];
    if (hasSymbol != 0)
        return;

    // Output some details
    std::stringstream output;
    output << "Symbol: 0x" << std::setfill('0') << std::setw(8) << std::hex << symbol_addr
        << " -> " << symbol_str << " " << std::dec << build_version;

#if 0 // TODO: XbSymbolDatabase - Need to create a structure for patch and stuff.
    bool IsXRef = OovpaTable->Oovpa->XRefSaveIndex != XRefNoSaveIndex;
    if (IsXRef) {
        output << "\t(XREF)";

        // do we need to save the found address?
        OOVPA* Oovpa = OovpaTable->Oovpa;
        if (Oovpa->XRefSaveIndex != XRefNoSaveIndex) {
            // is the XRef not saved yet?
            switch (XRefDataBase[Oovpa->XRefSaveIndex]) {
                case XREF_ADDR_NOT_FOUND:
                {
                    EmuLog(LOG_LEVEL::WARNING, "Found OOVPA after first finding nothing?");
                    // fallthrough to XREF_ADDR_UNDETERMINED
                }
                case XREF_ADDR_UNDETERMINED:
                {
                    // save and count the found address
                    UnResolvedXRefs--;
                    XRefDataBase[Oovpa->XRefSaveIndex] = pFunc;
                    break;
                }
                case XREF_ADDR_DERIVE:
                {
                    EmuLog(LOG_LEVEL::WARNING, "Cannot derive a save index!");
                    break;
                }
                default:
                {
                    if (XRefDataBase[OovpaTable->Oovpa->XRefSaveIndex] != pFunc) {
                        EmuLog(LOG_LEVEL::WARNING, "Found OOVPA on other address than in XRefDataBase!");
                        EmuLog(LOG_LEVEL::WARNING, "%s: %4d - pFunc: %08X, stored: %08X", OovpaTable->szFuncName, Oovpa->XRefSaveIndex, pFunc, XRefDataBase[Oovpa->XRefSaveIndex]);
                    }
                    break;
                }
            }
        }
    }

    // Retrieve the associated patch, if any is available
    void* addr = GetEmuPatchAddr(std::string(OovpaTable->szFuncName));

    if (addr != nullptr) {
        EmuInstallPatch(OovpaTable->szFuncName, pFunc, addr);
        output << "\t*PATCHED*";
    } else {
        const char* checkDisableStr = nullptr;
        size_t getFuncStrLength = strlen(OovpaTable->szFuncName);

        if (getFuncStrLength > 10) {
            checkDisableStr = &OovpaTable->szFuncName[getFuncStrLength - 10];
        }

        if (checkDisableStr != nullptr && strcmp(checkDisableStr, "_UNPATCHED") == 0) {
            output << "\t*UNPATCHED*";

            // Mention there's no patch available, if it was to be applied
        } else if (!IsXRef) {
            output << "\t*NO PATCH AVAILABLE!*";
        }
    }
#endif

	output << "\n";

	g_SymbolAddresses[symbol_str] = symbol_addr;
    printf(output.str().c_str());
}

// Update shared structure with GUI process
void EmuUpdateLLEStatus(uint32_t XbLibScan)
{
    unsigned int FlagsLLE;
    g_EmuShared->GetFlagsLLE(&FlagsLLE);

    // Wangan Midnight Maximum Tune 1/2 use D3D8LTCG (Chihiro D3D8I) which is handled
    // via hardcoded symbol tables — the XbLibScan flags are never set for it, so the
    // auto-detection below would wrongly force LLE GPU. Skip it entirely for these builds
    // and leave bLLE_GPU exactly as the user configured it.
    // All other titles get the original auto-detection: if D3D8/D3D8LTCG is absent from
    // the XBE, fall back to LLE GPU so the title can still render something.
    static const uint64_t kWanganHashes[] = {
        0x712f117b89129fa8ULL, // WMMT1 V307 Export
        0x74e8c73f60cb6e00ULL, // WMMT1 V307 Test Export
        0xb468b41f0928e6b6ULL, // WMMT1 V307 Japan
        0x2a409af4248a5588ULL, // WMMT1 V307 Test Japan
        0x3e6304c00e6c2894ULL, // WMMT2 V322 Export
        0x6d56294f90d4d222ULL, // WMMT2 V322 Test Export
        0xd6343c4e8811efaaULL, // WMMT2 V322 Japan
        0xa6a443b1a36b3905ULL, // WMMT2 V322 Test Japan
    };
    {
        uint64_t xbeHash = ComputeHash((void*)&CxbxKrnl_Xbe->m_Header, sizeof(Xbe::Header));
        bool isWangan = false;
        for (uint64_t h : kWanganHashes) { if (h == xbeHash) { isWangan = true; break; } }
        if (!isWangan
            && (FlagsLLE & LLE_GPU) == false
            && !((XbLibScan & XBSDBLIB_D3D8) > 0
                || (XbLibScan & XBSDBLIB_D3D8LTCG) > 0)) {
            bLLE_GPU = true;
            FlagsLLE ^= LLE_GPU;
            EmuOutputMessage(XB_OUTPUT_MESSAGE_INFO, "Fallback to LLE GPU.");
        }
    }

    if ((FlagsLLE & LLE_APU) == false
        && (XbLibScan & XBSDBLIB_DSOUND) == 0) {
        bLLE_APU = true;
        FlagsLLE ^= LLE_APU;
        EmuOutputMessage(XB_OUTPUT_MESSAGE_INFO, "Fallback to LLE APU.");
    }
#if 0 // Reenable this when LLE USB actually works
	if ((FlagsLLE & LLE_USB) == false
		&& (XbLibScan & XBSDBLIB_XAPILIB) == 0) {
		bLLE_USB = true;
		FlagsLLE ^= LLE_USB;
		EmuOutputMessage(XB_OUTPUT_MESSAGE_INFO, "Fallback to LLE USB.");
	}
#endif
    ipc_send_gui_update(IPC_UPDATE_GUI::LLE_FLAGS, FlagsLLE);
    //return FlagsLLE;
}

// Hardcoded D3D8I symbol tables for Chihiro games (e.g. Wangan Midnight Maximum Tune 2).
// XbSymbolDatabase has no OOVPAs for the D3D8I library in Chihiro builds.
// When the scanner finds D3DX8/D3D8 but cannot locate Direct3D_CreateDevice,
// this function injects known addresses keyed on the XBE header hash.
static void EmuApplyD3D8IHardcodedSymbols(uint64_t uiHash)
{
    struct SymEntry { const char* name; xbox::addr_xt addr; };

    // Wangan Midnight Maximum Tune 2 - V322 Export (USA)
    static const SymEntry wm2_v322e[] = {
        { "Direct3D_CreateDevice",                              0x1b3dd0 },
        { "D3D_g_pDevice",                                     0x1cc208 },
        { "D3D_g_RenderState",                                 0x1cbf70 },
        { "D3D_g_DeferredTextureState",                        0x1cbd70 },
        { "D3DDevice_Begin",                                   0x1b2830 },
        { "D3DDevice_End",                                     0x1b2890 },
        { "D3DDevice_Swap",                                    0x1b39d0 },
        { "D3DDevice_Clear",                                   0x1b3e90 },
        { "D3DDevice_Reset",                                   0x1adbb0 },
        { "D3DDevice_Release",                                 0x1ad9b0 },
        { "D3DDevice_MakeSpace",                               0x1b0190 },
        { "D3DDevice_IsBusy",                                  0x1aeb90 },
        { "D3DDevice_BlockUntilVerticalBlank",                 0x1aec00 },
        { "D3DDevice_GetCreationParameters",                   0x1ad7f0 },
        { "D3DDevice_SetViewport",                             0x1ae2a0 },
        { "D3DDevice_SetTransform",                            0x1ad840 },
        { "D3DDevice_SetRenderTarget",                         0x1adc70 },
        { "D3DDevice_GetRenderTarget2",                        0x1ae240 },
        { "D3DDevice_GetDepthStencilSurface2",                 0x1ae270 },
        { "D3DDevice_GetBackBuffer2",                          0x1adeb0 },
        { "D3DDevice_SetTexture",                              0x1ae8e0 },
        { "D3DDevice_SetPalette",                              0x1aeac0 },
        { "D3DDevice_SetLight",                                0x1ae4d0 },
        { "D3DDevice_LightEnable",                             0x1ae780 },
        { "D3DDevice_SetMaterial",                             0x1ad970 },
        { "D3DDevice_SetStreamSource",                         0x1acc70 },
        { "D3DDevice_SetVertexShader",                         0x1ad0f0 },
        { "D3DDevice_GetVertexShader",                         0x1acbf0 },
        { "D3DDevice_SelectVertexShader",                      0x1acdb0 },
        { "D3DDevice_LoadVertexShader",                        0x1acd40 },
        { "D3DDevice_CreateVertexShader",                      0x1acaa0 },
        { "D3DDevice_SetShaderConstantMode",                   0x1ace60 },
        { "D3DDevice_SetVertexShaderConstant1Fast",            0x1acc20 },
        { "D3DDevice_SetPixelShader",                          0x1ad300 },
        { "D3DDevice_CreatePixelShader",                       0x1ad270 },
        { "D3DDevice_SetPixelShaderConstant",                  0x1ad510 },
        { "D3DDevice_SetFlickerFilter",                        0x1ad9f0 },
        { "D3DDevice_SetSoftDisplayFilter",                    0x1ada50 },
        { "D3DDevice_DrawVertices",                            0x1b2320 },
        { "D3DDevice_DrawVerticesUP",                          0x1b2150 },
        { "D3DDevice_DrawIndexedVertices",                     0x1b2420 },
        { "D3DDevice_SetVertexData2f",                         0x1b26e0 },
        { "D3DDevice_SetVertexData4f",                         0x1b2730 },
        { "D3DDevice_SetVertexDataColor",                      0x1b27d0 },
        { "D3DDevice_CopyRects",                               0x1adf10 },
        { "D3DDevice_InsertCallback",                          0x1af750 },
        { "D3DDevice_BeginPush_4",                             0x1af7d0 },
        { "D3DDevice_EndPush",                                 0x1af830 },
        { "D3DDevice_SetScreenSpaceOffset",                    0x1af060 },
        { "D3DDevice_SetRenderState_Simple",                   0x1b0816 },
        { "D3DDevice_SetRenderStateNotInline",                 0x1b1080 },
        { "D3DDevice_SetRenderState_BackFillMode",             0x1b0fa0 },
        { "D3DDevice_SetRenderState_CullMode",                 0x1b0bb0 },
        { "D3DDevice_SetRenderState_DoNotCullUncompressed",    0x1b1ee0 },
        { "D3DDevice_SetRenderState_Dxt1NoiseEnable",          0x1b0dc0 },
        { "D3DDevice_SetRenderState_EdgeAntiAlias",            0x1b0ab0 },
        { "D3DDevice_SetRenderState_FillMode",                 0x1b0f40 },
        { "D3DDevice_SetRenderState_FogColor",                 0x1b0b50 },
        { "D3DDevice_SetRenderState_FrontFace",                0x1b0c30 },
        { "D3DDevice_SetRenderState_LineWidth",                0x1b0d40 },
        { "D3DDevice_SetRenderState_LogicOp",                  0x1b0ed0 },
        { "D3DDevice_SetRenderState_MultiSampleAntiAlias",     0x1b1f80 },
        { "D3DDevice_SetRenderState_MultiSampleMask",          0x1b2000 },
        { "D3DDevice_SetRenderState_MultiSampleMode",          0x1b1f00 },
        { "D3DDevice_SetRenderState_MultiSampleRenderTargetMode", 0x1b1f40 },
        { "D3DDevice_SetRenderState_NormalizeNormals",         0x1b0c80 },
        { "D3DDevice_SetRenderState_OcclusionCullEnable",      0x1b1da0 },
        { "D3DDevice_SetRenderState_PSTextureModes",           0x1b07e0 },
        { "D3DDevice_SetRenderState_RopZCmpAlwaysRead",        0x1b1ea0 },
        { "D3DDevice_SetRenderState_RopZRead",                 0x1b1ec0 },
        { "D3DDevice_SetRenderState_SampleAlpha",              0x1b2060 },
        { "D3DDevice_SetRenderState_ShadowFunc",               0x1b0b00 },
        { "D3DDevice_SetRenderState_StencilCullEnable",        0x1b1e20 },
        { "D3DDevice_SetRenderState_StencilEnable",            0x1b1c40 },
        { "D3DDevice_SetRenderState_StencilFail",              0x1b1ce0 },
        { "D3DDevice_SetRenderState_TextureFactor",            0x1b0cd0 },
        { "D3DDevice_SetRenderState_TwoSidedLighting",         0x1b0fd0 },
        { "D3DDevice_SetRenderState_VertexBlend",              0x1b1030 },
        { "D3DDevice_SetRenderState_YuvEnable",                0x1b1d60 },
        { "D3DDevice_SetRenderState_ZBias",                    0x1b0e50 },
        { "D3DDevice_SetRenderState_ZEnable",                  0x1b1ba0 },
        { "D3DDevice_SetTextureState_BumpEnv",                 0x1b1270 },
        { "D3DDevice_SetTextureState_TexCoordIndex",           0x1b1140 },
        { "D3DDevice_SetTextureState_BorderColor",             0x1b12f0 },
        { "D3DDevice_SetTextureState_ColorKeyColor",           0x1b1340 },
        { "D3D_BlockOnTime",                                   0x1afaa0 },
        { "D3D_DestroyResource",                               0x1b2cb0 },
        { "D3D_AllocContiguousMemory",                         0x1b4380 },
        { "D3D_FreeContiguousMemory",                          0x1b43d0 },
        { "D3D_GetAdapterModeCount",                           0x1b3bc0 },
        { "D3D_EnumAdapterModes",                              0x1b3c70 },
        { "D3DResource_AddRef",                                0x1b2dd0 },
        { "D3DResource_Release",                               0x1b2e20 },
        { "D3DResource_Register",                              0x1b2e80 },
        { "D3DResource_BlockUntilNotBusy",                     0x1b2ec0 },
        { "D3DVertexBuffer_Lock2",                             0x1ac4e0 },
        { "D3DTexture_GetSurfaceLevel2",                       0x1b29e0 },
        { "D3DTexture_LockRect",                               0x1b2a30 },
        { "D3DCubeTexture_GetCubeMapSurface2",                 0x1b2a70 },
        { "D3DCubeTexture_LockRect",                           0x1b2ad0 },
        { "D3DSurface_LockRect",                               0x1b2c20 },
        { "D3DPalette_Lock2",                                  0x1ac550 },
        { "D3DPalette_GetSize",                                0x1ac590 },
        { "Lock2DSurface",                                     0x1b8640 },
        // JVS — required for all JVS patches to install (JVS OOVPA is only for v4831; WMMT is v5849)
        { "JVS_SendCommand2",                                  0x1f3060 },
        { "JVS_g_pPINSA",                                      0x1eb0bc },
        { "JVS_g_pPINSB",                                      0x1eb0b4 },
        { "JvsBACKUP_Read",                                    0x1f4f90 },
        { "JvsBACKUP_Write",                                   0x1f50e0 },
        { "JvsEEPROM_Read2",                                   0x1f4bd0 },
        { "JvsEEPROM_Write2",                                  0x1f4db0 },
        { "JvsFirmwareDownload2",                              0x1f5600 },
        { "JvsFirmwareUpload2",                                0x1f53e0 },
        { "JvsNodeReceivePacket",                              0x1f5230 },
        { "JvsNodeSendPacket",                                 0x1f5310 },
        { "JvsRTC_Read2",                                      0x1f5840 },
        { "JvsScFirmwareDownload2",                            0x1f42d0 },
        { "JvsScFirmwareUpload2",                              0x1f4510 },
        { "JvsScReceiveMidi",                                  0x1f4a70 },
        { "JvsScReceiveRs323c",                                0x1f4910 },
        { "JvsScSendMidi",                                     0x1f4980 },
        { "JvsScSendRs323c",                                   0x1f48a0 },
    };

    // Wangan Midnight Maximum Tune 2 - V322 Japan
    static const SymEntry wm2_v322j[] = {
        { "Direct3D_CreateDevice",                              0x1bc510 },
        { "D3D_g_pDevice",                                     0x1d4b48 },
        { "D3D_g_RenderState",                                 0x1d48b0 },
        { "D3D_g_DeferredTextureState",                        0x1d46b0 },
        { "D3DDevice_Begin",                                   0x1bae70 },
        { "D3DDevice_End",                                     0x1baed0 },
        { "D3DDevice_Swap",                                    0x1bc110 },
        { "D3DDevice_Clear",                                   0x1bc5d0 },
        { "D3DDevice_Reset",                                   0x1b61f0 },
        { "D3DDevice_MakeSpace",                               0x1b87d0 },
        { "D3DDevice_IsBusy",                                  0x1b71d0 },
        { "D3DDevice_BlockUntilVerticalBlank",                 0x1b7240 },
        { "D3DDevice_GetCreationParameters",                   0x1b5d60 },
        { "D3DDevice_SetViewport",                             0x1b68e0 },
        { "D3DDevice_SetTransform",                            0x1b5db0 },
        { "D3DDevice_SetRenderTarget",                         0x1b62b0 },
        { "D3DDevice_GetRenderTarget",                         0x1b6880 },
        { "D3DDevice_GetDepthStencilSurface",                  0x1b68b0 },
        { "D3DDevice_SetTexture",                              0x1b6f20 },
        { "D3DDevice_SetPalette",                              0x1b7100 },
        { "D3DDevice_SetLight",                                0x1b6b10 },
        { "D3DDevice_LightEnable",                             0x1b6dc0 },
        { "D3DDevice_SetStreamSource",                         0x1b5190 },
        { "D3DDevice_SetVertexShader",                         0x1b5610 },
        { "D3DDevice_GetVertexShader",                         0x1b5110 },
        { "D3DDevice_SelectVertexShader",                      0x1b52d0 },
        { "D3DDevice_LoadVertexShader",                        0x1b5260 },
        { "D3DDevice_CreateVertexShader",                      0x1b4fc0 },
        { "D3DDevice_SetShaderConstantMode",                   0x1b5380 },
        { "D3DDevice_SetVertexShaderConstant4",                0x1b5140 },
        { "D3DDevice_SetPixelShader",                          0x1b5820 },
        { "D3DDevice_CreatePixelShader",                       0x1b5790 },
        { "D3DDevice_SetFlickerFilter",                        0x1b5f60 },
        { "D3DDevice_SetSoftDisplayFilter",                    0x1b5fc0 },
        { "D3DDevice_DrawVertices",                            0x1ba960 },
        { "D3DDevice_DrawVerticesUP",                          0x1ba790 },
        { "D3DDevice_DrawIndexedVertices",                     0x1baa60 },
        { "D3DDevice_SetVertexData2f",                         0x1bad20 },
        { "D3DDevice_SetVertexData4f",                         0x1bad70 },
        { "D3DDevice_SetVertexDataColor",                      0x1bae10 },
        { "D3DDevice_InsertCallback",                          0x1b7d90 },
        { "D3DDevice_BeginPush_4",                             0x1b7e10 },
        { "D3DDevice_EndPush",                                 0x1b7e70 },
        { "D3DDevice_SetRenderState_Simple",                   0x1b8e56 },
        { "D3DDevice_SetRenderStateNotInline",                 0x1b96c0 },
        { "D3DDevice_SetRenderState_BackFillMode",             0x1b95e0 },
        { "D3DDevice_SetRenderState_CullMode",                 0x1b91f0 },
        { "D3DDevice_SetRenderState_DoNotCullUncompressed",    0x1ba520 },
        { "D3DDevice_SetRenderState_Dxt1NoiseEnable",          0x1b9400 },
        { "D3DDevice_SetRenderState_EdgeAntiAlias",            0x1b90f0 },
        { "D3DDevice_SetRenderState_FillMode",                 0x1b9580 },
        { "D3DDevice_SetRenderState_FogColor",                 0x1b9190 },
        { "D3DDevice_SetRenderState_FrontFace",                0x1b9270 },
        { "D3DDevice_SetRenderState_LineWidth",                0x1b9380 },
        { "D3DDevice_SetRenderState_LogicOp",                  0x1b9510 },
        { "D3DDevice_SetRenderState_MultiSampleAntiAlias",     0x1ba5c0 },
        { "D3DDevice_SetRenderState_MultiSampleMask",          0x1ba640 },
        { "D3DDevice_SetRenderState_MultiSampleMode",          0x1ba540 },
        { "D3DDevice_SetRenderState_MultiSampleRenderTargetMode", 0x1ba580 },
        { "D3DDevice_SetRenderState_NormalizeNormals",         0x1b92c0 },
        { "D3DDevice_SetRenderState_OcclusionCullEnable",      0x1ba3e0 },
        { "D3DDevice_SetRenderState_PSTextureModes",           0x1b8e20 },
        { "D3DDevice_SetRenderState_RopZCmpAlwaysRead",        0x1ba4e0 },
        { "D3DDevice_SetRenderState_RopZRead",                 0x1ba500 },
        { "D3DDevice_SetRenderState_ShadowFunc",               0x1b9140 },
        { "D3DDevice_SetRenderState_StencilCullEnable",        0x1ba460 },
        { "D3DDevice_SetRenderState_StencilEnable",            0x1ba280 },
        { "D3DDevice_SetRenderState_StencilFail",              0x1ba320 },
        { "D3DDevice_SetRenderState_TextureFactor",            0x1b9310 },
        { "D3DDevice_SetRenderState_TwoSidedLighting",         0x1b9610 },
        { "D3DDevice_SetRenderState_VertexBlend",              0x1b9670 },
        { "D3DDevice_SetRenderState_YuvEnable",                0x1ba3a0 },
        { "D3DDevice_SetRenderState_ZBias",                    0x1b9490 },
        { "D3DDevice_SetRenderState_ZEnable",                  0x1ba1e0 },
        { "D3DDevice_SetTextureState_BumpEnv",                 0x1b98b0 },
        { "D3DDevice_SetTextureState_TexCoordIndex",           0x1b9780 },
        { "D3DDevice_SetTextureState_BorderColor",             0x1b9930 },
        { "D3DDevice_SetTextureState_ColorKeyColor",           0x1b9980 },
        { "D3D_BlockOnTime",                                   0x1b80e0 },
        { "D3D_DestroyResource",                               0x1bb350 },
        { "D3D_AllocContiguousMemory",                         0x1bcac0 },
        { "D3D_FreeContiguousMemory",                          0x1bcb10 },
        { "D3D_GetAdapterModeCount",                           0x1bc300 },
        { "D3D_EnumAdapterModes",                              0x1bc3b0 },
        { "D3DResource_AddRef",                                0x1bb470 },
        { "D3DResource_Release",                               0x1bb4c0 },
        { "D3DResource_Register",                              0x1bb5c0 },
        { "D3DResource_BlockUntilNotBusy",                     0x1bb600 },
        { "D3DVertexBuffer_Lock2",                             0x1b4a00 },
        { "D3DTexture_GetSurfaceLevel2",                       0x1bb050 },
        { "D3DTexture_LockRect",                               0x1bb0a0 },
        { "D3DCubeTexture_GetCubeMapSurface2",                 0x1bb0e0 },
        { "D3DCubeTexture_LockRect",                           0x1bb140 },
        { "D3DSurface_LockRect",                               0x1bb2c0 },
        { "D3DPalette_Lock2",                                  0x1b4a70 },
        { "D3DPalette_GetSize",                                0x1b4ab0 },
        // JVS — required for all JVS patches to install (JVS OOVPA is only for v4831; WMMT is v5849)
        { "JVS_SendCommand2",                                  0x210880 },
        { "JVS_g_pPINSA",                                      0x2086fc },
        { "JVS_g_pPINSB",                                      0x2086f4 },
        { "JvsBACKUP_Read",                                    0x2127b0 },
        { "JvsBACKUP_Write",                                   0x212900 },
        { "JvsEEPROM_Read2",                                   0x2123f0 },
        { "JvsEEPROM_Write2",                                  0x2125d0 },
        { "JvsFirmwareDownload2",                              0x212e20 },
        { "JvsFirmwareUpload2",                                0x212c00 },
        { "JvsNodeReceivePacket",                              0x212a50 },
        { "JvsNodeSendPacket",                                 0x212b30 },
        { "JvsRTC_Read2",                                      0x213060 },
        { "JvsScFirmwareDownload2",                            0x211af0 },
        { "JvsScFirmwareUpload2",                              0x211d30 },
        { "JvsScReceiveMidi",                                  0x212290 },
        { "JvsScReceiveRs323c",                                0x212130 },
        { "JvsScSendMidi",                                     0x2121a0 },
        { "JvsScSendRs323c",                                   0x2120c0 },
    };

    // Wangan Midnight Maximum Tune 1 - V307
    static const SymEntry wm1_v307[] = {
        { "Direct3D_CreateDevice",                              0x16d430 },
        { "D3D_g_pDevice",                                     0x185848 },
        { "D3D_g_RenderState",                                 0x1855b0 },
        { "D3D_g_DeferredTextureState",                        0x1853b0 },
        { "D3DDevice_Begin",                                   0x16be90 },
        { "D3DDevice_End",                                     0x16bef0 },
        { "D3DDevice_Swap",                                    0x16d030 },
        { "D3DDevice_Clear",                                   0x16d4f0 },
        { "D3DDevice_Reset",                                   0x167210 },
        { "D3DDevice_Release",                                 0x167010 },
        { "D3DDevice_MakeSpace",                               0x1697f0 },
        { "D3DDevice_IsBusy",                                  0x1681f0 },
        { "D3DDevice_BlockUntilVerticalBlank",                 0x168260 },
        { "D3DDevice_GetCreationParameters",                   0x166e50 },
        { "D3DDevice_SetViewport",                             0x167900 },
        { "D3DDevice_SetTransform",                            0x166ea0 },
        { "D3DDevice_SetRenderTarget",                         0x1672d0 },
        { "D3DDevice_GetRenderTarget2",                        0x1678a0 },
        { "D3DDevice_GetDepthStencilSurface2",                 0x1678d0 },
        { "D3DDevice_GetBackBuffer2",                          0x167510 },
        { "D3DDevice_SetTexture",                              0x167f40 },
        { "D3DDevice_SetPalette",                              0x168120 },
        { "D3DDevice_SetLight",                                0x167b30 },
        { "D3DDevice_LightEnable",                             0x167de0 },
        { "D3DDevice_SetMaterial",                             0x166fd0 },
        { "D3DDevice_SetStreamSource",                         0x1662d0 },
        { "D3DDevice_SetVertexShader",                         0x166750 },
        { "D3DDevice_GetVertexShader",                         0x166250 },
        { "D3DDevice_SelectVertexShader",                      0x166410 },
        { "D3DDevice_LoadVertexShader",                        0x1663a0 },
        { "D3DDevice_CreateVertexShader",                      0x166100 },
        { "D3DDevice_DeleteVertexShader",                      0x166630 },
        { "D3DDevice_SetShaderConstantMode",                   0x1664c0 },
        { "D3DDevice_SetVertexShaderConstant1Fast",            0x166280 },
        { "D3DDevice_SetPixelShader",                          0x166960 },
        { "D3DDevice_CreatePixelShader",                       0x1668d0 },
        { "D3DDevice_DeletePixelShader",                       0x166930 },
        { "D3DDevice_SetPixelShaderConstant",                  0x166b70 },
        { "D3DDevice_SetFlickerFilter",                        0x167050 },
        { "D3DDevice_SetSoftDisplayFilter",                    0x1670b0 },
        { "D3DDevice_DrawVertices",                            0x16b980 },
        { "D3DDevice_DrawVerticesUP",                          0x16b7b0 },
        { "D3DDevice_DrawIndexedVertices",                     0x16ba80 },
        { "D3DDevice_SetVertexData2f",                         0x16bd40 },
        { "D3DDevice_SetVertexData4f",                         0x16bd90 },
        { "D3DDevice_SetVertexDataColor",                      0x16be30 },
        { "D3DDevice_CopyRects",                               0x167570 },
        { "D3DDevice_InsertCallback",                          0x168db0 },
        { "D3DDevice_BeginPush_4",                             0x168e30 },
        { "D3DDevice_EndPush",                                 0x168e90 },
        { "D3DDevice_SetScreenSpaceOffset",                    0x1686c0 },
        { "D3DDevice_SetScissors",                             0x1683d0 },
        { "D3DDevice_SetRenderState_Simple",                   0x169e76 },
        { "D3DDevice_SetRenderStateNotInline",                 0x16a6e0 },
        { "D3DDevice_SetRenderState_BackFillMode",             0x16a600 },
        { "D3DDevice_SetRenderState_CullMode",                 0x16a210 },
        { "D3DDevice_SetRenderState_DoNotCullUncompressed",    0x16b540 },
        { "D3DDevice_SetRenderState_Dxt1NoiseEnable",          0x16a420 },
        { "D3DDevice_SetRenderState_EdgeAntiAlias",            0x16a110 },
        { "D3DDevice_SetRenderState_FillMode",                 0x16a5a0 },
        { "D3DDevice_SetRenderState_FrontFace",                0x16a290 },
        { "D3DDevice_SetRenderState_LineWidth",                0x16a3a0 },
        { "D3DDevice_SetRenderState_LogicOp",                  0x16a530 },
        { "D3DDevice_SetRenderState_MultiSampleAntiAlias",     0x16b5e0 },
        { "D3DDevice_SetRenderState_MultiSampleMask",          0x16b660 },
        { "D3DDevice_SetRenderState_MultiSampleMode",          0x16b560 },
        { "D3DDevice_SetRenderState_MultiSampleRenderTargetMode", 0x16b5a0 },
        { "D3DDevice_SetRenderState_NormalizeNormals",         0x16a2e0 },
        { "D3DDevice_SetRenderState_OcclusionCullEnable",      0x16b400 },
        { "D3DDevice_SetRenderState_PSTextureModes",           0x169e40 },
        { "D3DDevice_SetRenderState_RopZCmpAlwaysRead",        0x16b500 },
        { "D3DDevice_SetRenderState_RopZRead",                 0x16b520 },
        { "D3DDevice_SetRenderState_SampleAlpha",              0x16b6c0 },
        { "D3DDevice_SetRenderState_ShadowFunc",               0x16a160 },
        { "D3DDevice_SetRenderState_StencilCullEnable",        0x16b480 },
        { "D3DDevice_SetRenderState_StencilEnable",            0x16b2a0 },
        { "D3DDevice_SetRenderState_StencilFail",              0x16b340 },
        { "D3DDevice_SetRenderState_TextureFactor",            0x16a330 },
        { "D3DDevice_SetRenderState_TwoSidedLighting",         0x16a630 },
        { "D3DDevice_SetRenderState_VertexBlend",              0x16a690 },
        { "D3DDevice_SetRenderState_YuvEnable",                0x16b3c0 },
        { "D3DDevice_SetRenderState_ZBias",                    0x16a4b0 },
        { "D3DDevice_SetRenderState_ZEnable",                  0x16b200 },
        { "D3DDevice_SetTextureState_BumpEnv",                 0x16a8d0 },
        { "D3DDevice_SetTextureState_TexCoordIndex",           0x16a7a0 },
        { "D3DDevice_SetTextureState_BorderColor",             0x16a950 },
        { "D3DDevice_SetTextureState_ColorKeyColor",           0x16a9a0 },
        { "D3D_BlockOnTime",                                   0x169100 },
        { "D3D_DestroyResource",                               0x16c310 },
        { "D3D_AllocContiguousMemory",                         0x16d9e0 },
        { "D3D_FreeContiguousMemory",                          0x16da30 },
        { "D3D_GetAdapterModeCount",                           0x16d220 },
        { "D3D_EnumAdapterModes",                              0x16d2d0 },
        { "D3DResource_AddRef",                                0x16c430 },
        { "D3DResource_Release",                               0x16c480 },
        { "D3DResource_Register",                              0x16c4e0 },
        { "D3DResource_BlockUntilNotBusy",                     0x16c520 },
        { "D3DVertexBuffer_Lock2",                             0x165b40 },
        { "D3DTexture_GetSurfaceLevel2",                       0x16c040 },
        { "D3DTexture_LockRect",                               0x16c090 },
        { "D3DCubeTexture_GetCubeMapSurface2",                 0x16c0d0 },
        { "D3DCubeTexture_LockRect",                           0x16c130 },
        { "D3DSurface_LockRect",                               0x16c280 },
        { "D3DPalette_Lock2",                                  0x165bb0 },
        { "D3DPalette_GetSize",                                0x165bf0 },
        // JVS — required for all JVS patches to install (JVS OOVPA is only for v4831; WMMT is v5849)
        { "JVS_SendCommand2",                                  0x1ac6a0 },
        { "JVS_g_pPINSA",                                      0x1a46fc },
        { "JVS_g_pPINSB",                                      0x1a46f4 },
        { "JvsBACKUP_Read",                                    0x1ae5d0 },
        { "JvsBACKUP_Write",                                   0x1ae720 },
        { "JvsEEPROM_Read2",                                   0x1ae210 },
        { "JvsEEPROM_Write2",                                  0x1ae3f0 },
        { "JvsFirmwareDownload2",                              0x1aec40 },
        { "JvsFirmwareUpload2",                                0x1aea20 },
        { "JvsNodeReceivePacket",                              0x1ae870 },
        { "JvsNodeSendPacket",                                 0x1ae950 },
        { "JvsRTC_Read2",                                      0x1aee80 },
        { "JvsScFirmwareDownload2",                            0x1ad910 },
        { "JvsScFirmwareUpload2",                              0x1adb50 },
        { "JvsScReceiveMidi",                                  0x1ae0b0 },
        { "JvsScReceiveRs323c",                                0x1adf50 },
        { "JvsScSendMidi",                                     0x1adfc0 },
        { "JvsScSendRs323c",                                   0x1adee0 },
    };

    // Wangan Midnight Maximum Tune 1 - V307 Test
    static const SymEntry wm1_v307t[] = {
        { "Direct3D_CreateDevice",                              0x174010 },
        { "D3D_g_pDevice",                                     0x18c648 },
        { "D3D_g_RenderState",                                 0x18c3b0 },
        { "D3D_g_DeferredTextureState",                        0x18c1b0 },
        { "D3DDevice_Begin",                                   0x172970 },
        { "D3DDevice_End",                                     0x1729d0 },
        { "D3DDevice_Swap",                                    0x173c10 },
        { "D3DDevice_Clear",                                   0x1740d0 },
        { "D3DDevice_Reset",                                   0x16dcf0 },
        { "D3DDevice_Release",                                 0x16da20 },
        { "D3DDevice_MakeSpace",                               0x1702d0 },
        { "D3DDevice_IsBusy",                                  0x16ecd0 },
        { "D3DDevice_BlockUntilVerticalBlank",                 0x16ed40 },
        { "D3DDevice_GetCreationParameters",                   0x16d860 },
        { "D3DDevice_SetViewport",                             0x16e3e0 },
        { "D3DDevice_SetTransform",                            0x16d8b0 },
        { "D3DDevice_SetRenderTarget",                         0x16ddb0 },
        { "D3DDevice_GetRenderTarget2",                        0x16e380 },
        { "D3DDevice_GetDepthStencilSurface2",                 0x16e3b0 },
        { "D3DDevice_GetBackBuffer2",                          0x16dff0 },
        { "D3DDevice_SetTexture",                              0x16ea20 },
        { "D3DDevice_SetPalette",                              0x16ec00 },
        { "D3DDevice_SetLight",                                0x16e610 },
        { "D3DDevice_LightEnable",                             0x16e8c0 },
        { "D3DDevice_SetMaterial",                             0x16d9e0 },
        { "D3DDevice_SetStreamSource",                         0x16cc90 },
        { "D3DDevice_SetVertexShader",                         0x16d110 },
        { "D3DDevice_GetVertexShader",                         0x16cc10 },
        { "D3DDevice_SelectVertexShader",                      0x16cdd0 },
        { "D3DDevice_LoadVertexShader",                        0x16cd60 },
        { "D3DDevice_CreateVertexShader",                      0x16cac0 },
        { "D3DDevice_DeleteVertexShader",                      0x16cff0 },
        { "D3DDevice_SetShaderConstantMode",                   0x16ce80 },
        { "D3DDevice_SetVertexShaderConstant1Fast",            0x16cc40 },
        { "D3DDevice_SetPixelShader",                          0x16d320 },
        { "D3DDevice_CreatePixelShader",                       0x16d290 },
        { "D3DDevice_DeletePixelShader",                       0x16d2f0 },
        { "D3DDevice_SetPixelShaderConstant",                  0x16d530 },
        { "D3DDevice_SetFlickerFilter",                        0x16da60 },
        { "D3DDevice_SetSoftDisplayFilter",                    0x16dac0 },
        { "D3DDevice_DrawVertices",                            0x172460 },
        { "D3DDevice_DrawVerticesUP",                          0x172290 },
        { "D3DDevice_DrawIndexedVertices",                     0x172560 },
        { "D3DDevice_SetVertexData2f",                         0x172820 },
        { "D3DDevice_SetVertexData4f",                         0x172870 },
        { "D3DDevice_SetVertexDataColor",                      0x172910 },
        { "D3DDevice_CopyRects",                               0x16e050 },
        { "D3DDevice_InsertCallback",                          0x16f890 },
        { "D3DDevice_BeginPush_4",                             0x16f910 },
        { "D3DDevice_EndPush",                                 0x16f970 },
        { "D3DDevice_SetScreenSpaceOffset",                    0x16f1a0 },
        { "D3DDevice_SetScissors",                             0x16eeb0 },
        { "D3DDevice_SetRenderState_Simple",                   0x170956 },
        { "D3DDevice_SetRenderStateNotInline",                 0x1711c0 },
        { "D3DDevice_SetRenderState_BackFillMode",             0x1710e0 },
        { "D3DDevice_SetRenderState_CullMode",                 0x170cf0 },
        { "D3DDevice_SetRenderState_DoNotCullUncompressed",    0x172020 },
        { "D3DDevice_SetRenderState_Dxt1NoiseEnable",          0x170f00 },
        { "D3DDevice_SetRenderState_EdgeAntiAlias",            0x170bf0 },
        { "D3DDevice_SetRenderState_FillMode",                 0x171080 },
        { "D3DDevice_SetRenderState_FrontFace",                0x170d70 },
        { "D3DDevice_SetRenderState_LineWidth",                0x170e80 },
        { "D3DDevice_SetRenderState_LogicOp",                  0x171010 },
        { "D3DDevice_SetRenderState_MultiSampleAntiAlias",     0x1720c0 },
        { "D3DDevice_SetRenderState_MultiSampleMask",          0x172140 },
        { "D3DDevice_SetRenderState_MultiSampleMode",          0x172040 },
        { "D3DDevice_SetRenderState_MultiSampleRenderTargetMode", 0x172080 },
        { "D3DDevice_SetRenderState_NormalizeNormals",         0x170dc0 },
        { "D3DDevice_SetRenderState_OcclusionCullEnable",      0x171ee0 },
        { "D3DDevice_SetRenderState_PSTextureModes",           0x170920 },
        { "D3DDevice_SetRenderState_RopZCmpAlwaysRead",        0x171fe0 },
        { "D3DDevice_SetRenderState_RopZRead",                 0x172000 },
        { "D3DDevice_SetRenderState_SampleAlpha",              0x1721a0 },
        { "D3DDevice_SetRenderState_ShadowFunc",               0x170c40 },
        { "D3DDevice_SetRenderState_StencilCullEnable",        0x171f60 },
        { "D3DDevice_SetRenderState_StencilEnable",            0x171d80 },
        { "D3DDevice_SetRenderState_StencilFail",              0x171e20 },
        { "D3DDevice_SetRenderState_TextureFactor",            0x170e10 },
        { "D3DDevice_SetRenderState_TwoSidedLighting",         0x171110 },
        { "D3DDevice_SetRenderState_VertexBlend",              0x171170 },
        { "D3DDevice_SetRenderState_YuvEnable",                0x171ea0 },
        { "D3DDevice_SetRenderState_ZBias",                    0x170f90 },
        { "D3DDevice_SetRenderState_ZEnable",                  0x171ce0 },
        { "D3DDevice_SetTextureState_BumpEnv",                 0x1713b0 },
        { "D3DDevice_SetTextureState_TexCoordIndex",           0x171280 },
        { "D3DDevice_SetTextureState_BorderColor",             0x171430 },
        { "D3DDevice_SetTextureState_ColorKeyColor",           0x171480 },
        { "D3D_BlockOnTime",                                   0x16fbe0 },
        { "D3D_DestroyResource",                               0x172e50 },
        { "D3D_AllocContiguousMemory",                         0x1745c0 },
        { "D3D_FreeContiguousMemory",                          0x174610 },
        { "D3D_GetAdapterModeCount",                           0x173e00 },
        { "D3D_EnumAdapterModes",                              0x173eb0 },
        { "D3DResource_AddRef",                                0x172f70 },
        { "D3DResource_Release",                               0x172fc0 },
        { "D3DResource_Register",                              0x1730c0 },
        { "D3DResource_BlockUntilNotBusy",                     0x173100 },
        { "D3DVertexBuffer_Lock2",                             0x16c500 },
        { "D3DTexture_GetSurfaceLevel2",                       0x172b50 },
        { "D3DTexture_LockRect",                               0x172ba0 },
        { "D3DCubeTexture_GetCubeMapSurface2",                 0x172be0 },
        { "D3DCubeTexture_LockRect",                           0x172c40 },
        { "D3DSurface_LockRect",                               0x172dc0 },
        { "D3DPalette_Lock2",                                  0x16c570 },
        { "D3DPalette_GetSize",                                0x16c5b0 },
        { "Lock2DSurface",                                     0x178a80 },
        // JVS — required for all JVS patches to install (JVS OOVPA is only for v4831; WMMT is v5849)
        { "JVS_SendCommand2",                                  0x1c8380 },
        { "JVS_g_pPINSA",                                      0x1c01fc },
        { "JVS_g_pPINSB",                                      0x1c01f4 },
        { "JvsBACKUP_Read",                                    0x1ca2b0 },
        { "JvsBACKUP_Write",                                   0x1ca400 },
        { "JvsEEPROM_Read2",                                   0x1c9ef0 },
        { "JvsEEPROM_Write2",                                  0x1ca0d0 },
        { "JvsFirmwareDownload2",                              0x1ca920 },
        { "JvsFirmwareUpload2",                                0x1ca700 },
        { "JvsNodeReceivePacket",                              0x1ca550 },
        { "JvsNodeSendPacket",                                 0x1ca630 },
        { "JvsRTC_Read2",                                      0x1cab60 },
        { "JvsScFirmwareDownload2",                            0x1c95f0 },
        { "JvsScFirmwareUpload2",                              0x1c9830 },
        { "JvsScReceiveMidi",                                  0x1c9d90 },
        { "JvsScReceiveRs323c",                                0x1c9c30 },
        { "JvsScSendMidi",                                     0x1c9ca0 },
        { "JvsScSendRs323c",                                   0x1c9bc0 },
    };

    // Wangan Midnight Maximum Tune 1 - V307 Japan
    static const SymEntry wm1_v307j[] = {
        { "Direct3D_CreateDevice",                             0x16d470 },
        { "D3D_g_pDevice",                                     0x185888 },
        { "D3D_g_RenderState",                                 0x1855f0 },
        { "D3D_g_DeferredTextureState",                        0x1853f0 },
        { "D3DDevice_Begin",                                   0x16bed0 },
        { "D3DDevice_End",                                     0x16bf30 },
        { "D3DDevice_Swap",                                    0x16d070 },
        { "D3DDevice_Clear",                                   0x16d530 },
        { "D3DDevice_Reset",                                   0x167250 },
        { "D3DDevice_Release",                                 0x167050 },
        { "D3DDevice_MakeSpace",                               0x169830 },
        { "D3DDevice_IsBusy",                                  0x168230 },
        { "D3DDevice_BlockUntilVerticalBlank",                 0x1682a0 },
        { "D3DDevice_GetCreationParameters",                   0x166e90 },
        { "D3DDevice_SetViewport",                             0x167940 },
        { "D3DDevice_SetTransform",                            0x166ee0 },
        { "D3DDevice_SetRenderTarget",                         0x167310 },
        { "D3DDevice_GetRenderTarget2",                        0x1678e0 },
        { "D3DDevice_GetDepthStencilSurface2",                 0x167910 },
        { "D3DDevice_GetBackBuffer2",                          0x167550 },
        { "D3DDevice_SetTexture",                              0x167f80 },
        { "D3DDevice_SetPalette",                              0x168160 },
        { "D3DDevice_SetLight",                                0x167b70 },
        { "D3DDevice_LightEnable",                             0x167e20 },
        { "D3DDevice_SetMaterial",                             0x167010 },
        { "D3DDevice_SetStreamSource",                         0x166310 },
        { "D3DDevice_SetVertexShader",                         0x166790 },
        { "D3DDevice_GetVertexShader",                         0x166290 },
        { "D3DDevice_SelectVertexShader",                      0x166450 },
        { "D3DDevice_LoadVertexShader",                        0x1663e0 },
        { "D3DDevice_CreateVertexShader",                      0x166140 },
        { "D3DDevice_DeleteVertexShader",                      0x166670 },
        { "D3DDevice_SetShaderConstantMode",                   0x166500 },
        { "D3DDevice_SetVertexShaderConstant1Fast",            0x1662c0 },
        { "D3DDevice_SetPixelShader",                          0x1669a0 },
        { "D3DDevice_CreatePixelShader",                       0x166910 },
        { "D3DDevice_DeletePixelShader",                       0x166970 },
        { "D3DDevice_SetPixelShaderConstant",                  0x166bb0 },
        { "D3DDevice_SetFlickerFilter",                        0x167090 },
        { "D3DDevice_SetSoftDisplayFilter",                    0x1670f0 },
        { "D3DDevice_DrawVertices",                            0x16b9c0 },
        { "D3DDevice_DrawVerticesUP",                          0x16b7f0 },
        { "D3DDevice_DrawIndexedVertices",                     0x16bac0 },
        { "D3DDevice_SetVertexData2f",                         0x16bd80 },
        { "D3DDevice_SetVertexData4f",                         0x16bdd0 },
        { "D3DDevice_SetVertexDataColor",                      0x16be70 },
        { "D3DDevice_CopyRects",                               0x1675b0 },
        { "D3DDevice_InsertCallback",                          0x168df0 },
        { "D3DDevice_BeginPush_4",                             0x168e70 },
        { "D3DDevice_EndPush",                                 0x168ed0 },
        { "D3DDevice_SetScreenSpaceOffset",                    0x168700 },
        { "D3DDevice_SetScissors",                             0x168410 },
        { "D3DDevice_SetRenderState_Simple",                   0x169eb6 },
        { "D3DDevice_SetRenderStateNotInline",                 0x16a720 },
        { "D3DDevice_SetRenderState_BackFillMode",             0x16a640 },
        { "D3DDevice_SetRenderState_CullMode",                 0x16a250 },
        { "D3DDevice_SetRenderState_DoNotCullUncompressed",    0x16b580 },
        { "D3DDevice_SetRenderState_Dxt1NoiseEnable",          0x16a460 },
        { "D3DDevice_SetRenderState_EdgeAntiAlias",            0x16a150 },
        { "D3DDevice_SetRenderState_FillMode",                 0x16a5e0 },
        { "D3DDevice_SetRenderState_FrontFace",                0x16a2d0 },
        { "D3DDevice_SetRenderState_LineWidth",                0x16a3e0 },
        { "D3DDevice_SetRenderState_LogicOp",                  0x16a570 },
        { "D3DDevice_SetRenderState_MultiSampleAntiAlias",     0x16b620 },
        { "D3DDevice_SetRenderState_MultiSampleMask",          0x16b6a0 },
        { "D3DDevice_SetRenderState_MultiSampleMode",          0x16b5a0 },
        { "D3DDevice_SetRenderState_MultiSampleRenderTargetMode", 0x16b5e0 },
        { "D3DDevice_SetRenderState_NormalizeNormals",         0x16a320 },
        { "D3DDevice_SetRenderState_OcclusionCullEnable",      0x16b440 },
        { "D3DDevice_SetRenderState_PSTextureModes",           0x169e80 },
        { "D3DDevice_SetRenderState_RopZCmpAlwaysRead",        0x16b540 },
        { "D3DDevice_SetRenderState_RopZRead",                 0x16b560 },
        { "D3DDevice_SetRenderState_SampleAlpha",              0x16b700 },
        { "D3DDevice_SetRenderState_ShadowFunc",               0x16a1a0 },
        { "D3DDevice_SetRenderState_StencilCullEnable",        0x16b4c0 },
        { "D3DDevice_SetRenderState_StencilEnable",            0x16b2e0 },
        { "D3DDevice_SetRenderState_StencilFail",              0x16b380 },
        { "D3DDevice_SetRenderState_TextureFactor",            0x16a370 },
        { "D3DDevice_SetRenderState_TwoSidedLighting",         0x16a670 },
        { "D3DDevice_SetRenderState_VertexBlend",              0x16a6d0 },
        { "D3DDevice_SetRenderState_YuvEnable",                0x16b400 },
        { "D3DDevice_SetRenderState_ZBias",                    0x16a4f0 },
        { "D3DDevice_SetRenderState_ZEnable",                  0x16b240 },
        { "D3DDevice_SetTextureState_BumpEnv",                 0x16a910 },
        { "D3DDevice_SetTextureState_TexCoordIndex",           0x16a7e0 },
        { "D3DDevice_SetTextureState_BorderColor",             0x16a990 },
        { "D3DDevice_SetTextureState_ColorKeyColor",           0x16a9e0 },
        { "D3D_BlockOnTime",                                   0x169140 },
        { "D3D_DestroyResource",                               0x16c350 },
        { "D3D_AllocContiguousMemory",                         0x16da20 },
        { "D3D_FreeContiguousMemory",                          0x16da70 },
        { "D3D_GetAdapterModeCount",                           0x16d260 },
        { "D3D_EnumAdapterModes",                              0x16d310 },
        { "D3DResource_AddRef",                                0x16c470 },
        { "D3DResource_Release",                               0x16c4c0 },
        { "D3DResource_Register",                              0x16c520 },
        { "D3DResource_BlockUntilNotBusy",                     0x16c560 },
        { "D3DVertexBuffer_Lock2",                             0x165b80 },
        { "D3DTexture_GetSurfaceLevel2",                       0x16c080 },
        { "D3DTexture_LockRect",                               0x16c0d0 },
        { "D3DCubeTexture_GetCubeMapSurface2",                 0x16c110 },
        { "D3DCubeTexture_LockRect",                           0x16c170 },
        { "D3DSurface_LockRect",                               0x16c2c0 },
        { "D3DPalette_Lock2",                                  0x165bf0 },
        { "D3DPalette_GetSize",                                0x165c30 },
        // JVS
        { "JVS_SendCommand2",                                  0x1ac6e0 },
        { "JVS_g_pPINSA",                                      0x1a473c },
        { "JVS_g_pPINSB",                                      0x1a4734 },
        { "JvsBACKUP_Read",                                    0x1ae610 },
        { "JvsBACKUP_Write",                                   0x1ae760 },
        { "JvsEEPROM_Read2",                                   0x1ae250 },
        { "JvsEEPROM_Write2",                                  0x1ae430 },
        { "JvsFirmwareDownload2",                              0x1aec80 },
        { "JvsFirmwareUpload2",                                0x1aea60 },
        { "JvsNodeReceivePacket",                              0x1ae8b0 },
        { "JvsNodeSendPacket",                                 0x1ae990 },
        { "JvsRTC_Read2",                                      0x1aeec0 },
        { "JvsScFirmwareDownload2",                            0x1ad950 },
        { "JvsScFirmwareUpload2",                              0x1adb90 },
        { "JvsScReceiveMidi",                                  0x1ae0f0 },
        { "JvsScReceiveRs323c",                                0x1adf90 },
        { "JvsScSendMidi",                                     0x1ae000 },
        { "JvsScSendRs323c",                                   0x1adf20 },
    };

    // Wangan Midnight Maximum Tune 1 - V307 Test Japan
    static const SymEntry wm1_v307tj[] = {
        { "Direct3D_CreateDevice",                             0x174050 },
        { "D3D_g_pDevice",                                     0x18c688 },
        { "D3D_g_RenderState",                                 0x18c3f0 },
        { "D3D_g_DeferredTextureState",                        0x18c1f0 },
        { "D3DDevice_Begin",                                   0x1729b0 },
        { "D3DDevice_End",                                     0x172a10 },
        { "D3DDevice_Swap",                                    0x173c50 },
        { "D3DDevice_Clear",                                   0x174110 },
        { "D3DDevice_Reset",                                   0x16dd30 },
        { "D3DDevice_Release",                                 0x16da60 },
        { "D3DDevice_MakeSpace",                               0x170310 },
        { "D3DDevice_IsBusy",                                  0x16ed10 },
        { "D3DDevice_BlockUntilVerticalBlank",                 0x16ed80 },
        { "D3DDevice_GetCreationParameters",                   0x16d8a0 },
        { "D3DDevice_SetViewport",                             0x16e420 },
        { "D3DDevice_SetTransform",                            0x16d8f0 },
        { "D3DDevice_SetRenderTarget",                         0x16ddf0 },
        { "D3DDevice_GetRenderTarget2",                        0x16e3c0 },
        { "D3DDevice_GetDepthStencilSurface2",                 0x16e3f0 },
        { "D3DDevice_GetBackBuffer2",                          0x16e030 },
        { "D3DDevice_SetTexture",                              0x16ea60 },
        { "D3DDevice_SetPalette",                              0x16ec40 },
        { "D3DDevice_SetLight",                                0x16e650 },
        { "D3DDevice_LightEnable",                             0x16e900 },
        { "D3DDevice_SetMaterial",                             0x16da20 },
        { "D3DDevice_SetStreamSource",                         0x16ccd0 },
        { "D3DDevice_SetVertexShader",                         0x16d150 },
        { "D3DDevice_GetVertexShader",                         0x16cc50 },
        { "D3DDevice_SelectVertexShader",                      0x16ce10 },
        { "D3DDevice_LoadVertexShader",                        0x16cda0 },
        { "D3DDevice_CreateVertexShader",                      0x16cb00 },
        { "D3DDevice_DeleteVertexShader",                      0x16d030 },
        { "D3DDevice_SetShaderConstantMode",                   0x16cec0 },
        { "D3DDevice_SetVertexShaderConstant1Fast",            0x16cc80 },
        { "D3DDevice_SetPixelShader",                          0x16d360 },
        { "D3DDevice_CreatePixelShader",                       0x16d2d0 },
        { "D3DDevice_DeletePixelShader",                       0x16d330 },
        { "D3DDevice_SetPixelShaderConstant",                  0x16d570 },
        { "D3DDevice_SetFlickerFilter",                        0x16daa0 },
        { "D3DDevice_SetSoftDisplayFilter",                    0x16db00 },
        { "D3DDevice_DrawVertices",                            0x1724a0 },
        { "D3DDevice_DrawVerticesUP",                          0x1722d0 },
        { "D3DDevice_DrawIndexedVertices",                     0x1725a0 },
        { "D3DDevice_SetVertexData2f",                         0x172860 },
        { "D3DDevice_SetVertexData4f",                         0x1728b0 },
        { "D3DDevice_SetVertexDataColor",                      0x172950 },
        { "D3DDevice_CopyRects",                               0x16e090 },
        { "D3DDevice_InsertCallback",                          0x16f8d0 },
        { "D3DDevice_BeginPush_4",                             0x16f950 },
        { "D3DDevice_EndPush",                                 0x16f9b0 },
        { "D3DDevice_SetScreenSpaceOffset",                    0x16f1e0 },
        { "D3DDevice_SetScissors",                             0x16eef0 },
        { "D3DDevice_SetRenderState_Simple",                   0x170996 },
        { "D3DDevice_SetRenderStateNotInline",                 0x171200 },
        { "D3DDevice_SetRenderState_BackFillMode",             0x171120 },
        { "D3DDevice_SetRenderState_CullMode",                 0x170d30 },
        { "D3DDevice_SetRenderState_DoNotCullUncompressed",    0x172060 },
        { "D3DDevice_SetRenderState_Dxt1NoiseEnable",          0x170f40 },
        { "D3DDevice_SetRenderState_EdgeAntiAlias",            0x170c30 },
        { "D3DDevice_SetRenderState_FillMode",                 0x1710c0 },
        { "D3DDevice_SetRenderState_FrontFace",                0x170db0 },
        { "D3DDevice_SetRenderState_LineWidth",                0x170ec0 },
        { "D3DDevice_SetRenderState_LogicOp",                  0x171050 },
        { "D3DDevice_SetRenderState_MultiSampleAntiAlias",     0x172100 },
        { "D3DDevice_SetRenderState_MultiSampleMask",          0x172180 },
        { "D3DDevice_SetRenderState_MultiSampleMode",          0x172080 },
        { "D3DDevice_SetRenderState_MultiSampleRenderTargetMode", 0x1720c0 },
        { "D3DDevice_SetRenderState_NormalizeNormals",         0x170e00 },
        { "D3DDevice_SetRenderState_OcclusionCullEnable",      0x171f20 },
        { "D3DDevice_SetRenderState_PSTextureModes",           0x170960 },
        { "D3DDevice_SetRenderState_RopZCmpAlwaysRead",        0x172020 },
        { "D3DDevice_SetRenderState_RopZRead",                 0x172040 },
        { "D3DDevice_SetRenderState_SampleAlpha",              0x1721e0 },
        { "D3DDevice_SetRenderState_ShadowFunc",               0x170c80 },
        { "D3DDevice_SetRenderState_StencilCullEnable",        0x171fa0 },
        { "D3DDevice_SetRenderState_StencilEnable",            0x171dc0 },
        { "D3DDevice_SetRenderState_StencilFail",              0x171e60 },
        { "D3DDevice_SetRenderState_TextureFactor",            0x170e50 },
        { "D3DDevice_SetRenderState_TwoSidedLighting",         0x171150 },
        { "D3DDevice_SetRenderState_VertexBlend",              0x1711b0 },
        { "D3DDevice_SetRenderState_YuvEnable",                0x171ee0 },
        { "D3DDevice_SetRenderState_ZBias",                    0x170fd0 },
        { "D3DDevice_SetRenderState_ZEnable",                  0x171d20 },
        { "D3DDevice_SetTextureState_BumpEnv",                 0x1713f0 },
        { "D3DDevice_SetTextureState_TexCoordIndex",           0x1712c0 },
        { "D3DDevice_SetTextureState_BorderColor",             0x171470 },
        { "D3DDevice_SetTextureState_ColorKeyColor",           0x1714c0 },
        { "D3D_BlockOnTime",                                   0x16fc20 },
        { "D3D_DestroyResource",                               0x172e90 },
        { "D3D_AllocContiguousMemory",                         0x174600 },
        { "D3D_FreeContiguousMemory",                          0x174650 },
        { "D3D_GetAdapterModeCount",                           0x173e40 },
        { "D3D_EnumAdapterModes",                              0x173ef0 },
        { "D3DResource_AddRef",                                0x172fb0 },
        { "D3DResource_Release",                               0x173000 },
        { "D3DResource_Register",                              0x173100 },
        { "D3DResource_BlockUntilNotBusy",                     0x173140 },
        { "D3DVertexBuffer_Lock2",                             0x16c540 },
        { "D3DTexture_GetSurfaceLevel2",                       0x172b90 },
        { "D3DTexture_LockRect",                               0x172be0 },
        { "D3DCubeTexture_GetCubeMapSurface2",                 0x172c20 },
        { "D3DCubeTexture_LockRect",                           0x172c80 },
        { "D3DSurface_LockRect",                               0x172e00 },
        { "D3DPalette_Lock2",                                  0x16c5b0 },
        { "D3DPalette_GetSize",                                0x16c5f0 },
        { "Lock2DSurface",                                     0x178ac0 },
        // JVS
        { "JVS_SendCommand2",                                  0x1c83c0 },
        { "JVS_g_pPINSA",                                      0x1c023c },
        { "JVS_g_pPINSB",                                      0x1c0234 },
        { "JvsBACKUP_Read",                                    0x1ca2f0 },
        { "JvsBACKUP_Write",                                   0x1ca440 },
        { "JvsEEPROM_Read2",                                   0x1c9f30 },
        { "JvsEEPROM_Write2",                                  0x1ca110 },
        { "JvsFirmwareDownload2",                              0x1ca960 },
        { "JvsFirmwareUpload2",                                0x1ca740 },
        { "JvsNodeReceivePacket",                              0x1ca590 },
        { "JvsNodeSendPacket",                                 0x1ca670 },
        { "JvsRTC_Read2",                                      0x1caba0 },
        { "JvsScFirmwareDownload2",                            0x1c9630 },
        { "JvsScFirmwareUpload2",                              0x1c9870 },
        { "JvsScReceiveMidi",                                  0x1c9dd0 },
        { "JvsScReceiveRs323c",                                0x1c9c70 },
        { "JvsScSendMidi",                                     0x1c9ce0 },
        { "JvsScSendRs323c",                                   0x1c9c00 },
    };

    // Wangan Midnight Maximum Tune 2 - V322 Japan
    static const SymEntry wm2_v322jp[] = {
        { "Direct3D_CreateDevice",                             0x1b3d90 },
        { "D3D_g_pDevice",                                     0x1cc1c8 },
        { "D3D_g_RenderState",                                 0x1cbf30 },
        { "D3D_g_DeferredTextureState",                        0x1cbd30 },
        { "D3DDevice_Begin",                                   0x1b27f0 },
        { "D3DDevice_End",                                     0x1b2850 },
        { "D3DDevice_Swap",                                    0x1b3990 },
        { "D3DDevice_Clear",                                   0x1b3e50 },
        { "D3DDevice_Reset",                                   0x1adb70 },
        { "D3DDevice_Release",                                 0x1ad970 },
        { "D3DDevice_MakeSpace",                               0x1b0150 },
        { "D3DDevice_IsBusy",                                  0x1aeb50 },
        { "D3DDevice_BlockUntilVerticalBlank",                 0x1aebc0 },
        { "D3DDevice_GetCreationParameters",                   0x1ad7b0 },
        { "D3DDevice_SetViewport",                             0x1ae260 },
        { "D3DDevice_SetTransform",                            0x1ad800 },
        { "D3DDevice_SetRenderTarget",                         0x1adc30 },
        { "D3DDevice_GetRenderTarget2",                        0x1ae200 },
        { "D3DDevice_GetDepthStencilSurface2",                 0x1ae230 },
        { "D3DDevice_GetBackBuffer2",                          0x1ade70 },
        { "D3DDevice_SetTexture",                              0x1ae8a0 },
        { "D3DDevice_SetPalette",                              0x1aea80 },
        { "D3DDevice_SetLight",                                0x1ae490 },
        { "D3DDevice_LightEnable",                             0x1ae740 },
        { "D3DDevice_SetMaterial",                             0x1ad930 },
        { "D3DDevice_SetStreamSource",                         0x1acc30 },
        { "D3DDevice_SetVertexShader",                         0x1ad0b0 },
        { "D3DDevice_GetVertexShader",                         0x1acbb0 },
        { "D3DDevice_SelectVertexShader",                      0x1acd70 },
        { "D3DDevice_LoadVertexShader",                        0x1acd00 },
        { "D3DDevice_CreateVertexShader",                      0x1aca60 },
        { "D3DDevice_SetShaderConstantMode",                   0x1ace20 },
        { "D3DDevice_SetVertexShaderConstant1Fast",            0x1acbe0 },
        { "D3DDevice_SetPixelShader",                          0x1ad2c0 },
        { "D3DDevice_CreatePixelShader",                       0x1ad230 },
        { "D3DDevice_SetPixelShaderConstant",                  0x1ad4d0 },
        { "D3DDevice_SetFlickerFilter",                        0x1ad9b0 },
        { "D3DDevice_SetSoftDisplayFilter",                    0x1ada10 },
        { "D3DDevice_DrawVertices",                            0x1b22e0 },
        { "D3DDevice_DrawVerticesUP",                          0x1b2110 },
        { "D3DDevice_DrawIndexedVertices",                     0x1b23e0 },
        { "D3DDevice_SetVertexData2f",                         0x1b26a0 },
        { "D3DDevice_SetVertexData4f",                         0x1b26f0 },
        { "D3DDevice_SetVertexDataColor",                      0x1b2790 },
        { "D3DDevice_CopyRects",                               0x1aded0 },
        { "D3DDevice_InsertCallback",                          0x1af710 },
        { "D3DDevice_BeginPush_4",                             0x1af790 },
        { "D3DDevice_EndPush",                                 0x1af7f0 },
        { "D3DDevice_SetScreenSpaceOffset",                    0x1af020 },
        { "D3DDevice_SetRenderState_Simple",                   0x1b07d6 },
        { "D3DDevice_SetRenderStateNotInline",                 0x1b1040 },
        { "D3DDevice_SetRenderState_BackFillMode",             0x1b0f60 },
        { "D3DDevice_SetRenderState_CullMode",                 0x1b0b70 },
        { "D3DDevice_SetRenderState_DoNotCullUncompressed",    0x1b1ea0 },
        { "D3DDevice_SetRenderState_Dxt1NoiseEnable",          0x1b0d80 },
        { "D3DDevice_SetRenderState_EdgeAntiAlias",            0x1b0a70 },
        { "D3DDevice_SetRenderState_FillMode",                 0x1b0f00 },
        { "D3DDevice_SetRenderState_FogColor",                 0x1b0b10 },
        { "D3DDevice_SetRenderState_FrontFace",                0x1b0bf0 },
        { "D3DDevice_SetRenderState_LineWidth",                0x1b0d00 },
        { "D3DDevice_SetRenderState_LogicOp",                  0x1b0e90 },
        { "D3DDevice_SetRenderState_MultiSampleAntiAlias",     0x1b1f40 },
        { "D3DDevice_SetRenderState_MultiSampleMask",          0x1b1fc0 },
        { "D3DDevice_SetRenderState_MultiSampleMode",          0x1b1ec0 },
        { "D3DDevice_SetRenderState_MultiSampleRenderTargetMode", 0x1b1f00 },
        { "D3DDevice_SetRenderState_NormalizeNormals",         0x1b0c40 },
        { "D3DDevice_SetRenderState_OcclusionCullEnable",      0x1b1d60 },
        { "D3DDevice_SetRenderState_PSTextureModes",           0x1b07a0 },
        { "D3DDevice_SetRenderState_RopZCmpAlwaysRead",        0x1b1e60 },
        { "D3DDevice_SetRenderState_RopZRead",                 0x1b1e80 },
        { "D3DDevice_SetRenderState_SampleAlpha",              0x1b2020 },
        { "D3DDevice_SetRenderState_ShadowFunc",               0x1b0ac0 },
        { "D3DDevice_SetRenderState_StencilCullEnable",        0x1b1de0 },
        { "D3DDevice_SetRenderState_StencilEnable",            0x1b1c00 },
        { "D3DDevice_SetRenderState_StencilFail",              0x1b1ca0 },
        { "D3DDevice_SetRenderState_TextureFactor",            0x1b0c90 },
        { "D3DDevice_SetRenderState_TwoSidedLighting",         0x1b0f90 },
        { "D3DDevice_SetRenderState_VertexBlend",              0x1b0ff0 },
        { "D3DDevice_SetRenderState_YuvEnable",                0x1b1d20 },
        { "D3DDevice_SetRenderState_ZBias",                    0x1b0e10 },
        { "D3DDevice_SetRenderState_ZEnable",                  0x1b1b60 },
        { "D3DDevice_SetTextureState_BumpEnv",                 0x1b1230 },
        { "D3DDevice_SetTextureState_TexCoordIndex",           0x1b1100 },
        { "D3DDevice_SetTextureState_BorderColor",             0x1b12b0 },
        { "D3DDevice_SetTextureState_ColorKeyColor",           0x1b1300 },
        { "D3D_BlockOnTime",                                   0x1afa60 },
        { "D3D_DestroyResource",                               0x1b2c70 },
        { "D3D_AllocContiguousMemory",                         0x1b4340 },
        { "D3D_FreeContiguousMemory",                          0x1b4390 },
        { "D3D_GetAdapterModeCount",                           0x1b3b80 },
        { "D3D_EnumAdapterModes",                              0x1b3c30 },
        { "D3DResource_AddRef",                                0x1b2d90 },
        { "D3DResource_Release",                               0x1b2de0 },
        { "D3DResource_Register",                              0x1b2e40 },
        { "D3DResource_BlockUntilNotBusy",                     0x1b2e80 },
        { "D3DVertexBuffer_Lock2",                             0x1ac4a0 },
        { "D3DTexture_GetSurfaceLevel2",                       0x1b29a0 },
        { "D3DTexture_LockRect",                               0x1b29f0 },
        { "D3DCubeTexture_GetCubeMapSurface2",                 0x1b2a30 },
        { "D3DCubeTexture_LockRect",                           0x1b2a90 },
        { "D3DSurface_LockRect",                               0x1b2be0 },
        { "D3DPalette_Lock2",                                  0x1ac510 },
        { "D3DPalette_GetSize",                                0x1ac550 },
        { "Lock2DSurface",                                     0x1b8600 },
        // JVS
        { "JVS_SendCommand2",                                  0x1f3020 },
        { "JVS_g_pPINSA",                                      0x1eb07c },
        { "JVS_g_pPINSB",                                      0x1eb074 },
        { "JvsBACKUP_Read",                                    0x1f4f50 },
        { "JvsBACKUP_Write",                                   0x1f50a0 },
        { "JvsEEPROM_Read2",                                   0x1f4b90 },
        { "JvsEEPROM_Write2",                                  0x1f4d70 },
        { "JvsFirmwareDownload2",                              0x1f55c0 },
        { "JvsFirmwareUpload2",                                0x1f53a0 },
        { "JvsNodeReceivePacket",                              0x1f51f0 },
        { "JvsNodeSendPacket",                                 0x1f52d0 },
        { "JvsRTC_Read2",                                      0x1f5800 },
        { "JvsScFirmwareDownload2",                            0x1f4290 },
        { "JvsScFirmwareUpload2",                              0x1f44d0 },
        { "JvsScReceiveMidi",                                  0x1f4a30 },
        { "JvsScReceiveRs323c",                                0x1f48d0 },
        { "JvsScSendMidi",                                     0x1f4940 },
        { "JvsScSendRs323c",                                   0x1f4860 },
    };

    // Wangan Midnight Maximum Tune 2 - V322 Test Japan
    static const SymEntry wm2_v322tj[] = {
        { "Direct3D_CreateDevice",                             0x1bc550 },
        { "D3D_g_pDevice",                                     0x1d4b88 },
        { "D3D_g_RenderState",                                 0x1d48f0 },
        { "D3D_g_DeferredTextureState",                        0x1d46f0 },
        { "D3DDevice_Begin",                                   0x1baeb0 },
        { "D3DDevice_End",                                     0x1baf10 },
        { "D3DDevice_Swap",                                    0x1bc150 },
        { "D3DDevice_Clear",                                   0x1bc610 },
        { "D3DDevice_Reset",                                   0x1b6230 },
        { "D3DDevice_MakeSpace",                               0x1b8810 },
        { "D3DDevice_IsBusy",                                  0x1b7210 },
        { "D3DDevice_BlockUntilVerticalBlank",                 0x1b7280 },
        { "D3DDevice_GetCreationParameters",                   0x1b5da0 },
        { "D3DDevice_SetViewport",                             0x1b6920 },
        { "D3DDevice_SetTransform",                            0x1b5df0 },
        { "D3DDevice_SetRenderTarget",                         0x1b62f0 },
        { "D3DDevice_GetRenderTarget",                         0x1b68c0 },
        { "D3DDevice_GetDepthStencilSurface",                  0x1b68f0 },
        { "D3DDevice_SetTexture",                              0x1b6f60 },
        { "D3DDevice_SetPalette",                              0x1b7140 },
        { "D3DDevice_SetLight",                                0x1b6b50 },
        { "D3DDevice_LightEnable",                             0x1b6e00 },
        { "D3DDevice_SetStreamSource",                         0x1b51d0 },
        { "D3DDevice_SetVertexShader",                         0x1b5650 },
        { "D3DDevice_GetVertexShader",                         0x1b5150 },
        { "D3DDevice_SelectVertexShader",                      0x1b5310 },
        { "D3DDevice_LoadVertexShader",                        0x1b52a0 },
        { "D3DDevice_CreateVertexShader",                      0x1b5000 },
        { "D3DDevice_SetShaderConstantMode",                   0x1b53c0 },
        { "D3DDevice_SetVertexShaderConstant4",                0x1b5180 },
        { "D3DDevice_SetPixelShader",                          0x1b5860 },
        { "D3DDevice_CreatePixelShader",                       0x1b57d0 },
        { "D3DDevice_SetFlickerFilter",                        0x1b5fa0 },
        { "D3DDevice_SetSoftDisplayFilter",                    0x1b6000 },
        { "D3DDevice_DrawVertices",                            0x1ba9a0 },
        { "D3DDevice_DrawVerticesUP",                          0x1ba7d0 },
        { "D3DDevice_DrawIndexedVertices",                     0x1baaa0 },
        { "D3DDevice_SetVertexData2f",                         0x1bad60 },
        { "D3DDevice_SetVertexData4f",                         0x1badb0 },
        { "D3DDevice_SetVertexDataColor",                      0x1bae50 },
        { "D3DDevice_InsertCallback",                          0x1b7dd0 },
        { "D3DDevice_BeginPush_4",                             0x1b7e50 },
        { "D3DDevice_EndPush",                                 0x1b7eb0 },
        { "D3DDevice_SetRenderState_Simple",                   0x1b8e96 },
        { "D3DDevice_SetRenderStateNotInline",                 0x1b9700 },
        { "D3DDevice_SetRenderState_BackFillMode",             0x1b9620 },
        { "D3DDevice_SetRenderState_CullMode",                 0x1b9230 },
        { "D3DDevice_SetRenderState_DoNotCullUncompressed",    0x1ba560 },
        { "D3DDevice_SetRenderState_Dxt1NoiseEnable",          0x1b9440 },
        { "D3DDevice_SetRenderState_EdgeAntiAlias",            0x1b9130 },
        { "D3DDevice_SetRenderState_FillMode",                 0x1b95c0 },
        { "D3DDevice_SetRenderState_FogColor",                 0x1b91d0 },
        { "D3DDevice_SetRenderState_FrontFace",                0x1b92b0 },
        { "D3DDevice_SetRenderState_LineWidth",                0x1b93c0 },
        { "D3DDevice_SetRenderState_LogicOp",                  0x1b9550 },
        { "D3DDevice_SetRenderState_MultiSampleAntiAlias",     0x1ba600 },
        { "D3DDevice_SetRenderState_MultiSampleMask",          0x1ba680 },
        { "D3DDevice_SetRenderState_MultiSampleMode",          0x1ba580 },
        { "D3DDevice_SetRenderState_MultiSampleRenderTargetMode", 0x1ba5c0 },
        { "D3DDevice_SetRenderState_NormalizeNormals",         0x1b9300 },
        { "D3DDevice_SetRenderState_OcclusionCullEnable",      0x1ba420 },
        { "D3DDevice_SetRenderState_PSTextureModes",           0x1b8e60 },
        { "D3DDevice_SetRenderState_RopZCmpAlwaysRead",        0x1ba520 },
        { "D3DDevice_SetRenderState_RopZRead",                 0x1ba540 },
        { "D3DDevice_SetRenderState_ShadowFunc",               0x1b9180 },
        { "D3DDevice_SetRenderState_StencilCullEnable",        0x1ba4a0 },
        { "D3DDevice_SetRenderState_StencilEnable",            0x1ba2c0 },
        { "D3DDevice_SetRenderState_StencilFail",              0x1ba360 },
        { "D3DDevice_SetRenderState_TextureFactor",            0x1b9350 },
        { "D3DDevice_SetRenderState_TwoSidedLighting",         0x1b9650 },
        { "D3DDevice_SetRenderState_VertexBlend",              0x1b96b0 },
        { "D3DDevice_SetRenderState_YuvEnable",                0x1ba3e0 },
        { "D3DDevice_SetRenderState_ZBias",                    0x1b94d0 },
        { "D3DDevice_SetRenderState_ZEnable",                  0x1ba220 },
        { "D3DDevice_SetTextureState_BumpEnv",                 0x1b98f0 },
        { "D3DDevice_SetTextureState_TexCoordIndex",           0x1b97c0 },
        { "D3DDevice_SetTextureState_BorderColor",             0x1b9970 },
        { "D3DDevice_SetTextureState_ColorKeyColor",           0x1b99c0 },
        { "D3D_BlockOnTime",                                   0x1b8120 },
        { "D3D_DestroyResource",                               0x1bb390 },
        { "D3D_AllocContiguousMemory",                         0x1bcb00 },
        { "D3D_FreeContiguousMemory",                          0x1bcb50 },
        { "D3D_GetAdapterModeCount",                           0x1bc340 },
        { "D3D_EnumAdapterModes",                              0x1bc3f0 },
        { "D3DResource_AddRef",                                0x1bb4b0 },
        { "D3DResource_Release",                               0x1bb500 },
        { "D3DResource_Register",                              0x1bb600 },
        { "D3DResource_BlockUntilNotBusy",                     0x1bb640 },
        { "D3DVertexBuffer_Lock2",                             0x1b4a40 },
        { "D3DTexture_GetSurfaceLevel2",                       0x1bb090 },
        { "D3DTexture_LockRect",                               0x1bb0e0 },
        { "D3DCubeTexture_GetCubeMapSurface2",                 0x1bb120 },
        { "D3DCubeTexture_LockRect",                           0x1bb180 },
        { "D3DSurface_LockRect",                               0x1bb300 },
        { "D3DPalette_Lock2",                                  0x1b4ab0 },
        { "D3DPalette_GetSize",                                0x1b4af0 },
        // JVS
        { "JVS_SendCommand2",                                  0x2108c0 },
        { "JVS_g_pPINSA",                                      0x20873c },
        { "JVS_g_pPINSB",                                      0x208734 },
        { "JvsBACKUP_Read",                                    0x2127f0 },
        { "JvsBACKUP_Write",                                   0x212940 },
        { "JvsEEPROM_Read2",                                   0x212430 },
        { "JvsEEPROM_Write2",                                  0x212610 },
        { "JvsFirmwareDownload2",                              0x212e60 },
        { "JvsFirmwareUpload2",                                0x212c40 },
        { "JvsNodeReceivePacket",                              0x212a90 },
        { "JvsNodeSendPacket",                                 0x212b70 },
        { "JvsRTC_Read2",                                      0x2130a0 },
        { "JvsScFirmwareDownload2",                            0x211b30 },
        { "JvsScFirmwareUpload2",                              0x211d70 },
        { "JvsScReceiveMidi",                                  0x2122d0 },
        { "JvsScReceiveRs323c",                                0x212170 },
        { "JvsScSendMidi",                                     0x2121e0 },
        { "JvsScSendRs323c",                                   0x212100 },
    };

    struct KnownBuild { uint64_t hash; const SymEntry* table; size_t count; };
    static const KnownBuild known_builds[] = {
        { 0x712f117b89129fa8ULL, wm1_v307,   std::size(wm1_v307)   }, // WM1 V307 Export
        { 0x74e8c73f60cb6e00ULL, wm1_v307t,  std::size(wm1_v307t)  }, // WM1 V307 Test Export
        { 0xb468b41f0928e6b6ULL, wm1_v307j,  std::size(wm1_v307j)  }, // WM1 V307 Japan
        { 0x2a409af4248a5588ULL, wm1_v307tj, std::size(wm1_v307tj) }, // WM1 V307 Test Japan
        { 0x3e6304c00e6c2894ULL, wm2_v322e,  std::size(wm2_v322e)  }, // WM2 V322 Export
        { 0x6d56294f90d4d222ULL, wm2_v322j,  std::size(wm2_v322j)  }, // WM2 V322 Test Export
        { 0xd6343c4e8811efaaULL, wm2_v322jp, std::size(wm2_v322jp) }, // WM2 V322 Japan
        { 0xa6a443b1a36b3905ULL, wm2_v322tj, std::size(wm2_v322tj) }, // WM2 V322 Test Japan
    };

    for (const auto& build : known_builds) {
        if (build.hash == uiHash) {
            std::printf("D3D8I: Applying hardcoded symbol table for XBE hash %016llX\n", uiHash);
            for (size_t i = 0; i < build.count; i++) {
                g_SymbolAddresses[build.table[i].name] = build.table[i].addr;
            }
            break;
        }
    }

    // Emit the "no table" warning only when no symbol table matched at all.
    bool found = false;
    for (const auto& build : known_builds) { if (build.hash == uiHash) { found = true; break; } }
    if (!found) {
        std::printf("D3D8I: No hardcoded symbol table for XBE hash %016llX, D3D will not work\n", uiHash);
    }
}

// NOTE: EmuHLEIntercept do not get to be in XbSymbolDatabase, do the intecept in Cxbx project only.
void EmuHLEIntercept(Xbe::Header *pXbeHeader)
{
	Xbe::LibraryVersion *pLibraryVersion = (Xbe::LibraryVersion*)pXbeHeader->dwLibraryVersionsAddr;

	uint16_t xdkVersion = 0;
	uint32_t XbLibScan = 0;

	// NOTE: We need to check if title has library header to optimize verification process.
	if (pLibraryVersion != nullptr) {
		uint32_t dwLibraryVersions = pXbeHeader->dwLibraryVersions;
		const char* SectionName = nullptr;
		Xbe::SectionHeader* pSectionHeaders = (Xbe::SectionHeader*)pXbeHeader->dwSectionHeadersAddr;
		uint32_t XbLibFlag;

		// Get the highest revision build and prefix library to scan.
		for (uint32_t v = 0; v < dwLibraryVersions; v++) {
			uint16_t BuildVersion, QFEVersion;
			BuildVersion = pLibraryVersion[v].wBuildVersion;
			QFEVersion = pLibraryVersion[v].wFlags.QFEVersion;

			if (xdkVersion < BuildVersion) {
				xdkVersion = BuildVersion;
			}
			XbLibFlag = XbSDB_LibraryToFlag(std::string(pLibraryVersion[v].szName, pLibraryVersion[v].szName + 8).c_str());
			XbLibScan |= XbLibFlag;

			// Keep certain library versions for plugin usage.
			if ((XbLibFlag & (XBSDBLIB_D3D8 | XBSDBLIB_D3D8LTCG | XBSDBLIB_D3DX8)) > 0) {
				if (g_LibVersion_D3D8 < BuildVersion) {
					g_LibVersion_D3D8 = BuildVersion;
				}
			}
			else if ((XbLibFlag & XBSDBLIB_DSOUND) > 0) {
				g_LibVersion_DSOUND = BuildVersion;
			}
		}

		// Since XDK 4039 title does not have library version for DSOUND, let's check section header if it exists or not.
		for (unsigned int v = 0; v < pXbeHeader->dwSections; v++) {
			SectionName = (const char*)pSectionHeaders[v].dwSectionNameAddr;
			if (strncmp(SectionName, LIB_DSOUND, 8) == 0) {
				XbLibScan |= XBSDBLIB_DSOUND;

				// If DSOUND version is not set, we need to force set it.
				if (g_LibVersion_DSOUND == 0) {
					g_LibVersion_DSOUND = xdkVersion;
				}
				break;
			}
		}
	}

	EmuUpdateLLEStatus(XbLibScan);

	std::cout << "\n"
	    "*******************************************************************************\n"
	    "* Cxbx-Reloaded High Level Emulation database\n"
	    "*******************************************************************************\n"
	    << std::endl;

	// Make sure the Symbol Cache directory exists
	std::string cachePath = g_DataFilePath + "\\SymbolCache\\";
	if (!std::filesystem::exists(cachePath) && !std::filesystem::create_directory(cachePath)) {
		CxbxrAbort("Couldn't create Cxbx-Reloaded SymbolCache folder!");
	}

	// Hash the loaded XBE's header, use it as a filename
	uint64_t uiHash = ComputeHash((void*)&CxbxKrnl_Xbe->m_Header, sizeof(Xbe::Header));
	std::stringstream sstream;
	char tAsciiTitle[40] = "Unknown";
	std::setlocale(LC_ALL, "English");
	// Convert the title name character buffer into a string
	// If all chars are used, it won't be null terminated - we make sure to get the correct length
	std::wcstombs(tAsciiTitle, CxbxKrnl_Xbe->m_Certificate.wsTitleName, sizeof(tAsciiTitle));
	std::string szTitleName(tAsciiTitle, strnlen_s(tAsciiTitle, sizeof(tAsciiTitle)));
	CxbxKrnl_Xbe->PurgeBadChar(szTitleName);
	sstream << cachePath << szTitleName << "-" << std::hex << uiHash << ".ini";
	std::string filename = sstream.str();

	// This will fire when we exit this function scope; either after detecting a previous cache file, or when one is created
	CxbxDebuggerScopedMessage symbolCacheFilename(filename);

	CSimpleIniA symbolCacheData;

	if (std::filesystem::exists(filename.c_str())) {
		std::printf("Found Symbol Cache File: %08llX.ini\n", uiHash);

		symbolCacheData.LoadFile(filename.c_str());

		xdkVersion = (uint16_t)symbolCacheData.GetLongValue(section_libs, sect_libs_keys.BuildVersion, /*Default=*/0);

		// Verify the version of the cache file against the Symbol Database version hash
		const uint32_t SymbolDatabaseVersionHash = symbolCacheData.GetLongValue(section_info, sect_info_keys.SymbolDatabaseVersionHash, /*Default=*/0);

		if (SymbolDatabaseVersionHash == XbSDB_LibraryVersion()) {
			g_SymbolCacheUsed = true;
			CSimpleIniA::TNamesDepend symbol_names;

			if (g_SymbolCacheUsed) {
				std::printf("Using Symbol Cache\n");

				// Parse the .INI file into the map of symbol addresses
				symbolCacheData.GetAllKeys(section_symbols, symbol_names);
				for (auto it = symbol_names.begin(); it != symbol_names.end(); ++it) {
					g_SymbolAddresses[it->pItem] = symbolCacheData.GetLongValue(section_symbols, it->pItem, /*Default=*/0);
				}

				// Iterate through the map of symbol addresses, calling GetEmuPatchAddr on all functions.
				for (auto it = g_SymbolAddresses.begin(); it != g_SymbolAddresses.end(); ++it) {
					std::string functionName = it->first;
					xbox::addr_xt location = it->second;

					std::stringstream output;
					output << "SymbolCache: 0x" << std::setfill('0') << std::setw(8) << std::hex << location
					    << " -> " << functionName << "\n";
					std::printf(output.str().c_str());
				}
			}
		}

		// If g_SymbolAddresses didn't get filled, then symbol cache is invalid
		if (g_SymbolAddresses.empty()) {
			std::printf("Symbol Cache file is outdated and will be regenerated\n");
			g_SymbolCacheUsed = false;
		}
	}

	// If the Symbol Cache was used, go straight to patching, no need to re-scan
	if (g_SymbolCacheUsed) {
		// D3D8I Chihiro builds: always merge hardcoded JVS+D3D symbols so they
		// survive across cache rebuilds and version-hash mismatches.  The hardcoded
		// table only adds entries that aren't already present, so this is a no-op
		// when the cache already contains them, and a rescue when it doesn't.
		EmuApplyD3D8IHardcodedSymbols(uiHash);
		EmuInstallPatches();
		return;
	}

	//
	// initialize Microsoft XDK emulation
	//
	if(pLibraryVersion != nullptr) {

		std::printf("Symbol: Detected Microsoft XDK application...\n");

#if 0 // NOTE: This code is currently disabled due to not optimized and require more work to do.

        XbSymbolRegisterLibrary(XbLibScan);

        while (true) {

            size_t SymbolSize = g_SymbolAddresses.size();

            Xbe::SectionHeader* pSectionHeaders = reinterpret_cast<Xbe::SectionHeader*>(pXbeHeader->dwSectionHeadersAddr);
            Xbe::SectionHeader* pSectionScan = nullptr;

            for (uint32_t v = 0; v < pXbeHeader->dwSections; v++) {

                pSectionScan = pSectionHeaders + v;

                XbSymbolScanSection((uint32_t)pXbeHeader, 64 * ONE_MB, (const char*)pSectionScan->dwSectionNameAddr, pSectionScan->dwVirtualAddr, pSectionScan->dwSizeofRaw, xdkVersion, EmuRegisterSymbol);
            }

            // If symbols are not adding to array, break the loop.
            if (SymbolSize == g_SymbolAddresses.size()) {
                break;
            }
        }
#endif

        XbSDB_SetOutputMessage(EmuOutputMessage);

        XbSDB_Scan(pXbeHeader, EmuRegisterSymbol, false);
	}

	// D3D8I (Chihiro) builds have no OOVPAs in XbSymbolDatabase.
	// If D3D was detected (via D3DX8/D3D8 library entry) but Direct3D_CreateDevice was not
	// found by the scanner, inject hardcoded addresses for known builds.
	if (g_LibVersion_D3D8 > 0 && g_SymbolAddresses.find("Direct3D_CreateDevice") == g_SymbolAddresses.end()) {
		EmuApplyD3D8IHardcodedSymbols(uiHash);
	}

	std::printf("\n");

	// Perform a reset just in case a cached file data still exist.
	symbolCacheData.Reset();

	// Store Symbol Database version
	symbolCacheData.SetLongValue(section_info, sect_info_keys.SymbolDatabaseVersionHash, XbSDB_LibraryVersion(), nullptr, /*UseHex =*/false);

	// Store Certificate Details
	symbolCacheData.SetValue(section_certificate, sect_certificate_keys.Name, tAsciiTitle);
	symbolCacheData.SetValue(section_certificate, sect_certificate_keys.TitleID, FormatTitleId(CxbxKrnl_Xbe->m_Certificate.dwTitleId).c_str());
	symbolCacheData.SetLongValue(section_certificate, sect_certificate_keys.TitleIDHex, CxbxKrnl_Xbe->m_Certificate.dwTitleId, nullptr, /*UseHex =*/true);
	symbolCacheData.SetValue(section_certificate, sect_certificate_keys.Region, CxbxKrnl_Xbe->GameRegionToString().c_str());
	symbolCacheData.SetLongValue(section_certificate, sect_certificate_keys.RegionHex, CxbxKrnl_Xbe->m_Certificate.dwGameRegion, nullptr, /*UseHex =*/true);
	symbolCacheData.SetValue(section_certificate, sect_certificate_keys.Version, CxbxKrnl_Xbe->VersionToString().c_str());
	symbolCacheData.SetLongValue(section_certificate, sect_certificate_keys.VersionHex, CxbxKrnl_Xbe->m_Certificate.dwVersion, nullptr, /*UseHex =*/true);

	// Store Library Details
	for (unsigned int i = 0; i < pXbeHeader->dwLibraryVersions; i++) {
		std::string LibraryName(pLibraryVersion[i].szName, pLibraryVersion[i].szName + 8);
		symbolCacheData.SetLongValue(section_libs, LibraryName.c_str(), pLibraryVersion[i].wBuildVersion, nullptr, /*UseHex =*/false);
	}

	symbolCacheData.SetLongValue(section_libs, sect_libs_keys.BuildVersion, xdkVersion, nullptr, /*UseHex =*/false);

	// Store detected symbol addresses
	for(auto it = g_SymbolAddresses.begin(); it != g_SymbolAddresses.end(); ++it) {
		symbolCacheData.SetLongValue(section_symbols, it->first.c_str(), it->second, nullptr, /*UseHex =*/true);
	}

	// Save data to unique symbol cache file
	symbolCacheData.SaveFile(filename.c_str());

	EmuInstallPatches();
}


#if 0 // TODO: Need to move this into XbSymbolDatabase for depth verification usage.
#ifdef _DEBUG_TRACE

struct HLEVerifyContext {
    const HLEData *main_data;
    OOVPA *oovpa, *against;
    const HLEData *against_data;
    uint32_t main_index, against_index;
};

std::string HLEErrorString(const HLEData *data, uint16_t buildVersion, uint32_t index)
{
    std::string result =
        "OOVPATable " + (std::string)(data->LibSec.library) + "_" + std::to_string(buildVersion)
        + "[" + std::to_string(index) + "] "
        + (std::string)(data->OovpaTable[index].szFuncName);

    return result;
}

void HLEError(HLEVerifyContext *context, uint16_t buildVersion, char *format, ...)
{
    std::string output = "HLE Error ";
    if (context->main_data != nullptr) {
        output += "in " + HLEErrorString(context->main_data, buildVersion, context->main_index);
    }

    if (context->against != nullptr && context->against_data != nullptr) {
        output += ", comparing against " + HLEErrorString(context->against_data, buildVersion, context->against_index);
    }

    // format specific error message
    char buffer[200];
    va_list args;
    va_start(args, format);
    vsprintf(buffer, format, args);
    va_end(args);

    output += " : " + (std::string)buffer + (std::string)"\n";
    printf(output.c_str());
}

void VerifyHLEDataBaseAgainst(HLEVerifyContext *context); // forward

void VerifyHLEOOVPA(HLEVerifyContext *context, uint16_t buildVersion, OOVPA *oovpa)
{
    if (context->against == nullptr) {
        // TODO : verify XRefSaveIndex and XRef's (how?)

        // verify offsets are in increasing order
        uint32_t prev_offset;
        uint8_t dummy_value;
        GetOovpaEntry(oovpa, oovpa->XRefCount, prev_offset, dummy_value);
        for (int p = oovpa->XRefCount + 1; p < oovpa->Count; p++) {
            uint32_t curr_offset;
            GetOovpaEntry(oovpa, p, curr_offset, dummy_value);
            if (!(curr_offset > prev_offset)) {
                HLEError(context, buildVersion, "Lovp[%d] : Offset (0x%x) must be larger then previous offset (0x%x)",
                         p, curr_offset, prev_offset);
            }
        }

        // find duplicate OOVPA's across all other data-table-oovpa's
        context->oovpa = oovpa;
        context->against = oovpa;
        VerifyHLEDataBaseAgainst(context);
        context->against = nullptr; // reset scanning state
        return;
    }

    // prevent checking an oovpa against itself
    if (context->against == oovpa) {
        return;
    }

    // compare {Offset, Value}-pairs between two OOVPA's
    OOVPA *left = context->against, *right = oovpa;
    int l = 0, r = 0;
    uint32_t left_offset, right_offset;
    uint8_t left_value, right_value;
    GetOovpaEntry(left, l, left_offset, left_value);
    GetOovpaEntry(right, r, right_offset, right_value);
    int unique_offset_left = 0;
    int unique_offset_right = 0;
    int equal_offset_value = 0;
    int equal_offset_different_value = 0;
    while (true) {
        bool left_next = true;
        bool right_next = true;

        if (left_offset < right_offset) {
            unique_offset_left++;
            right_next = false;
        } else if (left_offset > right_offset) {
            unique_offset_right++;
            left_next = false;
        } else if (left_value == right_value) {
            equal_offset_value++;
        } else {
            equal_offset_different_value++;
        }

        // increment r before use (in left_next)
        if (right_next) {
            r++;
        }

        if (left_next) {
            l++;
            if (l >= left->Count) {
                unique_offset_right += right->Count - r;
                break;
            }

            GetOovpaEntry(left, l, left_offset, left_value);
        }

        if (right_next) {
            if (r >= right->Count) {
                unique_offset_left += left->Count - l;
                break;
            }

            GetOovpaEntry(right, r, right_offset, right_value);
        }
    }

    // no mismatching values on identical offsets?
    if (equal_offset_different_value == 0) {
        // enough matching OV-pairs?
        if (equal_offset_value > 4) {
            // no unique OV-pairs on either side?
            if (unique_offset_left + unique_offset_right == 0) {
                HLEError(context, buildVersion, "OOVPA's are identical",
                         unique_offset_left,
                         unique_offset_right);
            } else {
                // not too many new OV-pairs on the left side?
                if (unique_offset_left < 6) {
                    // not too many new OV-parirs on the right side?
                    if (unique_offset_right < 6) {
                        HLEError(context, buildVersion, "OOVPA's are expanded (left +%d, right +%d)",
                                 unique_offset_left,
                                 unique_offset_right);
                    }
                }
            }
        }
    }
}

void VerifyHLEDataEntry(HLEVerifyContext *context, const OOVPATable *table, uint32_t index)
{
    if (context->against == nullptr) {
        context->main_index = index;
    } else {
        context->against_index = index;
    }

    if (context->against == nullptr) {
        const char* checkDisableStr = nullptr;
        size_t getFuncStrLength = strlen(table[index].szFuncName);

        if (getFuncStrLength > 10) {
            checkDisableStr = &table[index].szFuncName[getFuncStrLength - 10];
        }

        if (checkDisableStr != nullptr && strcmp(checkDisableStr, "_UNPATCHED") == 0) {
            if (GetEmuPatchAddr((std::string)table[index].szFuncName)) {
                HLEError(context, table[index].Version, "OOVPA registration UNPATCHED while a patch exists!");
            }
        } else if (table[index].Oovpa->XRefSaveIndex != XRefNoSaveIndex) {
            if (GetEmuPatchAddr((std::string)table[index].szFuncName)) {
                HLEError(context, table[index].Version, "OOVPA registration XREF while a patch exists!");
            }
        }
    }

    // verify the OOVPA of this entry
    if (table[index].Oovpa != nullptr) {
        VerifyHLEOOVPA(context, table[index].Version, table[index].Oovpa);
    }
}

void VerifyHLEData(HLEVerifyContext *context, const HLEData *data)
{
    if (context->against == nullptr) {
        context->main_data = data;
    } else {
        context->against_data = data;
    }

    // Don't check a database against itself :
    if (context->main_data == context->against_data) {
        return;
    }

    // verify each entry in this HLEData
    for (uint32_t e = 0; e < data->OovpaTableCount; e++) {
        VerifyHLEDataEntry(context, data->OovpaTable, e);
    }
}

void VerifyHLEDataBaseAgainst(HLEVerifyContext *context)
{
    // verify all HLEData's
    for (uint32_t d = 0; d < HLEDataBaseCount; d++) {
        VerifyHLEData(context, &HLEDataBase[d]);
    }
}

void VerifyHLEDataBase()
{
    HLEVerifyContext context = { 0 };
    VerifyHLEDataBaseAgainst(&context);
}
#endif // _DEBUG_TRACE
#endif
