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

#if !defined(_DEBUG)
#undef EmuLog
#define EmuLog(...) do {} while (0)
#endif
#include "core\kernel\support\Emu.h"
#include "common\BetaConfig.h"
#include <intrin.h>
#include <subhook.h>

namespace
{
	using WanganImageRequestFn = int (__cdecl*)(uintptr_t, const char*);
	using WanganImageQueueFn = int (__cdecl*)(
		uintptr_t,
		uintptr_t,
		uintptr_t,
		uintptr_t,
		uintptr_t);
	using WanganImageSelectorFn = uintptr_t (__cdecl*)(
		uintptr_t,
		uintptr_t,
		uintptr_t,
		uintptr_t,
		uintptr_t);
	using WanganStreamReadFn = int (__cdecl*)(
		uintptr_t,
		void*,
		uintptr_t);
	using WanganImageLookupFn = uintptr_t (__cdecl*)(uintptr_t);
	using WanganResourceAllocateFn = uintptr_t (__cdecl*)(
		uintptr_t,
		uintptr_t,
		uintptr_t);
	using WanganRegisterLoaderFn = int (__cdecl*)(
		const char*,
		uintptr_t,
		uintptr_t);
	using WanganFindLoaderFn = uintptr_t (__cdecl*)(const char*);
	using WanganFindDeviceFn = uintptr_t (__cdecl*)(const char*);
	using WanganOpenResourceFn = uintptr_t (__cdecl*)(const char*);

	subhook::Hook g_WanganImageRequestHook;
	subhook::Hook g_WanganImageSelectorHook;
	subhook::Hook g_WanganImageLookupHook;
	subhook::Hook g_WanganResourceAllocateHook;
	subhook::Hook g_WanganRegisterLoaderHook;
	subhook::Hook g_WanganFindLoaderHook;
	subhook::Hook g_WanganFindDeviceHook;
	subhook::Hook g_WanganOpenResourceHook;

	int __cdecl TraceWanganImageRequest(
		uintptr_t resourceManager,
		const char* extension)
	{
		BetaTrace_Record(
			"WMMT_IMG_RIN",
			"manager=%08X extension=%p",
			static_cast<unsigned int>(resourceManager),
			extension);
		const auto original =
			reinterpret_cast<WanganImageRequestFn>(
				g_WanganImageRequestHook.GetTrampoline());
		const int result = original(resourceManager, extension);
		BetaTrace_Record(
			"WMMT_IMG_ROUT",
			"manager=%08X extension=%p result=%08X",
			static_cast<unsigned int>(resourceManager),
			extension,
			static_cast<unsigned int>(result));
		return result;
	}

	uintptr_t __cdecl TraceWanganResourceAllocate(
		uintptr_t selector,
		uintptr_t flags,
		uintptr_t context)
	{
		const uintptr_t caller =
			reinterpret_cast<uintptr_t>(_ReturnAddress());
		BetaTrace_Record(
			"WMMT_ALLOC_IN",
			"caller=%08X selector=%u flags=%08X context=%08X",
			static_cast<unsigned int>(caller),
			static_cast<unsigned int>(selector),
			static_cast<unsigned int>(flags),
			static_cast<unsigned int>(context));
		const auto original =
			reinterpret_cast<WanganResourceAllocateFn>(
				g_WanganResourceAllocateHook.GetTrampoline());
		const uintptr_t result =
			original(selector, flags, context);
		BetaTrace_Record(
			"WMMT_ALLOC_OUT",
			"caller=%08X selector=%u result=%08X",
			static_cast<unsigned int>(caller),
			static_cast<unsigned int>(selector),
			static_cast<unsigned int>(result));
		return result;
	}

	int __cdecl TraceWanganRegisterLoader(
		const char* extension,
		uintptr_t callback,
		uintptr_t flags)
	{
		char extensionCopy[48] = {};
		__try {
			if (extension != nullptr) {
				strncpy_s(
					extensionCopy,
					sizeof(extensionCopy),
					extension,
					_TRUNCATE);
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			strcpy_s(extensionCopy, "<fault>");
		}
		BetaTrace_Record(
			"WMMT_REG_IN",
			"caller=%08X extension=\"%s\" callback=%08X flags=%08X",
			static_cast<unsigned int>(
				reinterpret_cast<uintptr_t>(_ReturnAddress())),
			extensionCopy,
			static_cast<unsigned int>(callback),
			static_cast<unsigned int>(flags));
		const auto original =
			reinterpret_cast<WanganRegisterLoaderFn>(
				g_WanganRegisterLoaderHook.GetTrampoline());
		const int result = original(extension, callback, flags);
		BetaTrace_Record(
			"WMMT_REG_OUT",
			"extension=\"%s\" callback=%08X result=%d",
			extensionCopy,
			static_cast<unsigned int>(callback),
			result);
		return result;
	}

	uintptr_t __cdecl TraceWanganFindLoader(const char* path)
	{
		char pathCopy[160] = {};
		__try {
			if (path != nullptr) {
				strncpy_s(
					pathCopy,
					sizeof(pathCopy),
					path,
					_TRUNCATE);
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			strcpy_s(pathCopy, "<fault>");
		}
		const auto original =
			reinterpret_cast<WanganFindLoaderFn>(
				g_WanganFindLoaderHook.GetTrampoline());
		const uintptr_t result = original(path);
		BetaTrace_Record(
			"WMMT_FIND_LOADER",
			"caller=%08X path=\"%s\" result=%08X",
			static_cast<unsigned int>(
				reinterpret_cast<uintptr_t>(_ReturnAddress())),
			pathCopy,
			static_cast<unsigned int>(result));
		return result;
	}

	uintptr_t __cdecl TraceWanganFindDevice(const char* path)
	{
		const auto original =
			reinterpret_cast<WanganFindDeviceFn>(
				g_WanganFindDeviceHook.GetTrampoline());
		const uintptr_t result = original(path);
		if (reinterpret_cast<uintptr_t>(_ReturnAddress()) == 0x0010520B) {
			static LONG deviceLayoutRecorded = 0;
			if (result != 0 &&
				InterlockedCompareExchange(
					&deviceLayoutRecorded,
					1,
					0) == 0) {
				uintptr_t words[24] = {};
				__try {
					memcpy(
						words,
						reinterpret_cast<const void*>(result),
						sizeof(words));
				}
				__except (EXCEPTION_EXECUTE_HANDLER) {
					words[0] = 0xFFFFFFFFU;
				}
				BetaTrace_Record(
					"WMMT_DEVICE_LAYOUT",
					"handler=%08X w00=%08X w04=%08X w08=%08X w0C=%08X w10=%08X w14=%08X w18=%08X w1C=%08X w20=%08X w24=%08X w28=%08X w2C=%08X w30=%08X w34=%08X w38=%08X w3C=%08X w40=%08X w44=%08X w48=%08X w4C=%08X w50=%08X w54=%08X w58=%08X w5C=%08X",
					static_cast<unsigned int>(result),
					static_cast<unsigned int>(words[0]),
					static_cast<unsigned int>(words[1]),
					static_cast<unsigned int>(words[2]),
					static_cast<unsigned int>(words[3]),
					static_cast<unsigned int>(words[4]),
					static_cast<unsigned int>(words[5]),
					static_cast<unsigned int>(words[6]),
					static_cast<unsigned int>(words[7]),
					static_cast<unsigned int>(words[8]),
					static_cast<unsigned int>(words[9]),
					static_cast<unsigned int>(words[10]),
					static_cast<unsigned int>(words[11]),
					static_cast<unsigned int>(words[12]),
					static_cast<unsigned int>(words[13]),
					static_cast<unsigned int>(words[14]),
					static_cast<unsigned int>(words[15]),
					static_cast<unsigned int>(words[16]),
					static_cast<unsigned int>(words[17]),
					static_cast<unsigned int>(words[18]),
					static_cast<unsigned int>(words[19]),
					static_cast<unsigned int>(words[20]),
					static_cast<unsigned int>(words[21]),
					static_cast<unsigned int>(words[22]),
					static_cast<unsigned int>(words[23]));
			}
			BetaTrace_Record(
				"WMMT_FIND_DEVICE",
				"path=%p result=%08X",
				path,
				static_cast<unsigned int>(result));
		}
		return result;
	}

	uintptr_t __cdecl TraceWanganOpenResource(const char* path)
	{
		char pathCopy[192] = {};
		__try {
			if (path != nullptr) {
				strncpy_s(
					pathCopy,
					sizeof(pathCopy),
					path,
					_TRUNCATE);
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			strcpy_s(pathCopy, "<fault>");
		}
		const uintptr_t caller =
			reinterpret_cast<uintptr_t>(_ReturnAddress());
		const auto original =
			reinterpret_cast<WanganOpenResourceFn>(
				g_WanganOpenResourceHook.GetTrampoline());
		const uintptr_t result = original(path);
		if (caller == 0x0002EC02 || caller == 0x0002EC1A) {
			BetaTrace_Record(
				"WMMT_OPEN_RESOURCE",
				"caller=%08X path=\"%s\" result=%08X",
				static_cast<unsigned int>(caller),
				pathCopy,
				static_cast<unsigned int>(result));
		}
		return result;
	}

	uintptr_t __cdecl TraceWanganImageLookup(uintptr_t key)
	{
		unsigned int keyWords[4] = {};
		__try {
			if (key != 0) {
				memcpy(
					keyWords,
					reinterpret_cast<const void*>(key),
					sizeof(keyWords));
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			keyWords[0] = 0xFFFFFFFFU;
		}
		BetaTrace_Record(
			"WMMT_LOOK_IN",
			"caller=%08X key=%08X words=%08X,%08X,%08X,%08X",
			static_cast<unsigned int>(
				reinterpret_cast<uintptr_t>(_ReturnAddress())),
			static_cast<unsigned int>(key),
			keyWords[0],
			keyWords[1],
			keyWords[2],
			keyWords[3]);
		const auto original =
			reinterpret_cast<WanganImageLookupFn>(
				g_WanganImageLookupHook.GetTrampoline());
		const uintptr_t result = original(key);
		BetaTrace_Record(
			"WMMT_LOOK_OUT",
			"key=%08X result=%08X",
			static_cast<unsigned int>(key),
			static_cast<unsigned int>(result));
		return result;
	}

	uintptr_t __cdecl TraceWanganImageSelector(
		uintptr_t output,
		uintptr_t resource,
		uintptr_t selector,
		uintptr_t flags,
		uintptr_t context)
	{
		BetaTrace_Record(
			"WMMT_IMG_SIN",
			"caller=%08X output=%08X resource=%08X selector=%u flags=%08X context=%08X",
			static_cast<unsigned int>(
				reinterpret_cast<uintptr_t>(_ReturnAddress())),
			static_cast<unsigned int>(output),
			static_cast<unsigned int>(resource),
			static_cast<unsigned int>(selector),
			static_cast<unsigned int>(flags),
			static_cast<unsigned int>(context));
		const auto original =
			reinterpret_cast<WanganImageSelectorFn>(
				g_WanganImageSelectorHook.GetTrampoline());
		const uintptr_t result =
			original(output, resource, selector, flags, context);
		BetaTrace_Record(
			"WMMT_IMG_SOUT",
			"output=%08X selector=%u result=%08X",
			static_cast<unsigned int>(output),
			static_cast<unsigned int>(selector),
			static_cast<unsigned int>(result));
		return result;
	}

	int __cdecl TraceWanganResourceHeaderRead(
		uintptr_t stream,
		void* destination,
		uintptr_t size)
	{
		const int result =
			reinterpret_cast<WanganStreamReadFn>(0x0002B710)(
				stream,
				destination,
				size);
		unsigned int header = 0xFFFFFFFFU;
		__try {
			if (destination != nullptr && size >= sizeof(header) &&
				result >= static_cast<int>(sizeof(header))) {
				header = *reinterpret_cast<const unsigned int*>(destination);
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			header = 0xFFFFFFFEU;
		}
		BetaTrace_Record(
			"WMMT_HDR_READ",
			"stream=%08X destination=%p size=%u result=%d header=%08X textureBit=%u",
			static_cast<unsigned int>(stream),
			destination,
			static_cast<unsigned int>(size),
			result,
			header,
			(header >> 16) & 1U);
		return result;
	}

	int __cdecl TraceWanganImageQueue(
		uintptr_t resourceManager,
		uintptr_t resource,
		uintptr_t flags,
		uintptr_t callback,
		uintptr_t callbackContext)
	{
		unsigned int slotCount = 0;
		__try {
			if (resourceManager != 0) {
				slotCount = *reinterpret_cast<const unsigned int*>(
					resourceManager + 4);
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			slotCount = 0xFFFFFFFFU;
		}

		BetaTrace_Record(
			"WMMT_IMG_QIN",
			"manager=%08X slots=%u resource=%08X flags=%08X callback=%08X context=%08X",
			static_cast<unsigned int>(resourceManager),
			slotCount,
			static_cast<unsigned int>(resource),
			static_cast<unsigned int>(flags),
			static_cast<unsigned int>(callback),
			static_cast<unsigned int>(callbackContext));

		const int result =
			reinterpret_cast<WanganImageQueueFn>(0x00105290)(
				resourceManager,
				resource,
				flags,
				callback,
				callbackContext);

		BetaTrace_Record(
			"WMMT_IMG_QOUT",
			"manager=%08X resource=%08X result=%d",
			static_cast<unsigned int>(resourceManager),
			static_cast<unsigned int>(resource),
			result);
		return result;
	}
}

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

	// Diagnostic-only V322 probe. The image loader reaches this queue on
	// native Windows before it opens PNG/BMP/DDS resources. Keeping the call
	// probe behind scheduler_io_trace lets Windows and Wine/Android runs be
	// compared without affecting normal packages.
	if (gameType == JvsGameType::WanganMT2 &&
		g_BetaConfig.scheduler_io_trace) {
		constexpr uintptr_t kImageRequest = 0x00105380;
		constexpr uintptr_t kImageQueueCall = 0x00105433;
		constexpr uintptr_t kImageSelector = 0x0002B620;
		constexpr uintptr_t kResourceHeaderReadCall = 0x000199A4;
		constexpr uintptr_t kImageLookup = 0x00019ED0;
		constexpr uintptr_t kResourceAllocate = 0x0002BC00;
		constexpr uintptr_t kRegisterLoader = 0x0002EC80;
		constexpr uintptr_t kFindLoader = 0x0002EC50;
		constexpr uintptr_t kFindDevice = 0x00105030;
		constexpr uintptr_t kOpenResource = 0x00105200;
		if (g_WanganImageSelectorHook.Install(
				reinterpret_cast<void*>(kImageSelector),
				reinterpret_cast<void*>(&TraceWanganImageSelector))) {
			FlushInstructionCache(
				GetCurrentProcess(),
				reinterpret_cast<const void*>(kImageSelector),
				16);
			EmuLog(
				LOG_LEVEL::INFO,
				"WanganPatch: enabled V322 image-selector trace at VA 0x%08X",
				static_cast<unsigned int>(kImageSelector));
		}
		else {
			EmuLog(
				LOG_LEVEL::WARNING,
				"WanganPatch: failed to install V322 image-selector trace");
		}

		if (g_WanganResourceAllocateHook.Install(
				reinterpret_cast<void*>(kResourceAllocate),
				reinterpret_cast<void*>(&TraceWanganResourceAllocate))) {
			FlushInstructionCache(
				GetCurrentProcess(),
				reinterpret_cast<const void*>(kResourceAllocate),
				16);
			EmuLog(
				LOG_LEVEL::INFO,
				"WanganPatch: enabled V322 resource-allocation trace at VA 0x%08X",
				static_cast<unsigned int>(kResourceAllocate));
		}
		else {
			EmuLog(
				LOG_LEVEL::WARNING,
				"WanganPatch: failed to install V322 resource-allocation trace");
		}

		if (g_WanganRegisterLoaderHook.Install(
				reinterpret_cast<void*>(kRegisterLoader),
				reinterpret_cast<void*>(&TraceWanganRegisterLoader))) {
			FlushInstructionCache(
				GetCurrentProcess(),
				reinterpret_cast<const void*>(kRegisterLoader),
				16);
			EmuLog(
				LOG_LEVEL::INFO,
				"WanganPatch: enabled V322 loader-registration trace at VA 0x%08X",
				static_cast<unsigned int>(kRegisterLoader));
		}
		else {
			EmuLog(
				LOG_LEVEL::WARNING,
				"WanganPatch: failed to install V322 loader-registration trace");
		}

		if (g_WanganFindLoaderHook.Install(
				reinterpret_cast<void*>(kFindLoader),
				reinterpret_cast<void*>(&TraceWanganFindLoader))) {
			FlushInstructionCache(
				GetCurrentProcess(),
				reinterpret_cast<const void*>(kFindLoader),
				16);
			EmuLog(
				LOG_LEVEL::INFO,
				"WanganPatch: enabled V322 loader-lookup trace at VA 0x%08X",
				static_cast<unsigned int>(kFindLoader));
		}
		else {
			EmuLog(
				LOG_LEVEL::WARNING,
				"WanganPatch: failed to install V322 loader-lookup trace");
		}

		if (g_WanganFindDeviceHook.Install(
				reinterpret_cast<void*>(kFindDevice),
				reinterpret_cast<void*>(&TraceWanganFindDevice))) {
			FlushInstructionCache(
				GetCurrentProcess(),
				reinterpret_cast<const void*>(kFindDevice),
				16);
		}
		else {
			EmuLog(
				LOG_LEVEL::WARNING,
				"WanganPatch: failed to install V322 device-lookup trace");
		}

		if (g_WanganOpenResourceHook.Install(
				reinterpret_cast<void*>(kOpenResource),
				reinterpret_cast<void*>(&TraceWanganOpenResource))) {
			FlushInstructionCache(
				GetCurrentProcess(),
				reinterpret_cast<const void*>(kOpenResource),
				16);
		}
		else {
			EmuLog(
				LOG_LEVEL::WARNING,
				"WanganPatch: failed to install V322 resource-open trace");
		}

		if (g_WanganImageLookupHook.Install(
				reinterpret_cast<void*>(kImageLookup),
				reinterpret_cast<void*>(&TraceWanganImageLookup))) {
			FlushInstructionCache(
				GetCurrentProcess(),
				reinterpret_cast<const void*>(kImageLookup),
				16);
			EmuLog(
				LOG_LEVEL::INFO,
				"WanganPatch: enabled V322 image-lookup trace at VA 0x%08X",
				static_cast<unsigned int>(kImageLookup));
		}
		else {
			EmuLog(
				LOG_LEVEL::WARNING,
				"WanganPatch: failed to install V322 image-lookup trace");
		}

		static const uint8_t kExpectedHeaderReadCall[] = {
			0xE8, 0x67, 0x1D, 0x01, 0x00
		};
		if (memcmp(
				reinterpret_cast<const void*>(kResourceHeaderReadCall),
				kExpectedHeaderReadCall,
				sizeof(kExpectedHeaderReadCall)) == 0) {
			PatchWithCall(
				kResourceHeaderReadCall,
				reinterpret_cast<const void*>(&TraceWanganResourceHeaderRead),
				sizeof(kExpectedHeaderReadCall));
			EmuLog(
				LOG_LEVEL::INFO,
				"WanganPatch: enabled V322 resource-header trace at VA 0x%08X",
				static_cast<unsigned int>(kResourceHeaderReadCall));
		}
		else {
			EmuLog(
				LOG_LEVEL::WARNING,
				"WanganPatch: V322 resource-header trace signature mismatch");
		}

		if (g_WanganImageRequestHook.Install(
				reinterpret_cast<void*>(kImageRequest),
				reinterpret_cast<void*>(&TraceWanganImageRequest))) {
			FlushInstructionCache(
				GetCurrentProcess(),
				reinterpret_cast<const void*>(kImageRequest),
				16);
			EmuLog(
				LOG_LEVEL::INFO,
				"WanganPatch: enabled V322 image-request trace at VA 0x%08X",
				static_cast<unsigned int>(kImageRequest));
		}
		else {
			EmuLog(
				LOG_LEVEL::WARNING,
				"WanganPatch: failed to install V322 image-request trace");
		}

		static const uint8_t kExpectedCall[] = {
			0xE8, 0x58, 0xFE, 0xFF, 0xFF
		};
		if (memcmp(
				reinterpret_cast<const void*>(kImageQueueCall),
				kExpectedCall,
				sizeof(kExpectedCall)) == 0) {
			PatchWithCall(
				kImageQueueCall,
				reinterpret_cast<const void*>(&TraceWanganImageQueue),
				sizeof(kExpectedCall));
			EmuLog(
				LOG_LEVEL::INFO,
				"WanganPatch: enabled V322 image-queue trace at VA 0x%08X",
				static_cast<unsigned int>(kImageQueueCall));
		}
		else {
			EmuLog(
				LOG_LEVEL::WARNING,
				"WanganPatch: V322 image-queue trace signature mismatch");
		}
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
