// This is an open source non-commercial project. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
// ******************************************************************
// *
// *  This file is part of Cxbx-Reloaded.
// *
// *  Cxbx-Reloaded is free software; you can redistribute it
// *  and/or modify it under the terms of the GNU General Public
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
// *  (c) 2017-2019 Patrick van Logchem <pvanlogchem@gmail.com>
// *
// *  All rights reserved
// *
// ******************************************************************

// cxbxr-emu.cpp : Defines the exported functions for the DLL application.

#define LOG_PREFIX CXBXR_MODULE::CXBXR

#include "Cxbx.h" // For FUNC_EXPORTS
#include "VerifyAddressRanges.h" // For VerifyBaseAddr()
//#include "CxbxKrnl/Emu.h"
#include "EmuShared.h"
#include "common/PerfTrace.h"
#include "common/RenderTrace.h"
#include "core\kernel\init\CxbxKrnl.h" // For HandleFirstLaunch() and LaunchEmulation()
#include "core\hle\D3D8\Direct3D9\Shader.h" // For ShaderCacheShutdown()
//#include <commctrl.h>
#include "common/util/cliConverter.hpp"
#include "common/util/cliConfig.hpp"

namespace
{
	constexpr SIZE_T AndroidWineEmulationStackSize = 16 * 1024 * 1024;

	struct AndroidWineEmulationContext
	{
		unsigned int reservedSystems;
		blocks_reserved_t blocksReserved;
	};

	DWORD WINAPI AndroidWineEmulationThread(LPVOID parameter)
	{
		auto* context =
			static_cast<AndroidWineEmulationContext*>(parameter);
		CxbxKrnlEmulate(
			context->reservedSystems,
			context->blocksReserved);
		return EXIT_SUCCESS;
	}

	bool UseAndroidWineEmulationStack()
	{
		char enabled[2] = {};
		return GetEnvironmentVariableA(
			"TP_ANDROID_CXBXR_LARGE_STACK",
			enabled,
			sizeof(enabled)) != 0 &&
			enabled[0] == '1';
	}
}

PCHAR*
CommandLineToArgvA(
	PCHAR CmdLine,
	int* _argc
)
{
	PCHAR* argv;
	PCHAR  _argv;
	ULONG   len;
	ULONG   argc;
	CHAR   a;
	ULONG   i, j;

	BOOLEAN  in_QM;
	BOOLEAN  in_TEXT;
	BOOLEAN  in_SPACE;

	len = strlen(CmdLine);
	i = ((len + 2) / 2) * sizeof(PVOID) + sizeof(PVOID);

	argv = (PCHAR*)LocalAlloc(GMEM_FIXED,
		i + (len + 2) * sizeof(CHAR));

	_argv = (PCHAR)(((PUCHAR)argv) + i);

	argc = 0;
	argv[argc] = _argv;
	in_QM = FALSE;
	in_TEXT = FALSE;
	in_SPACE = TRUE;
	i = 0;
	j = 0;

	while (a = CmdLine[i]) {
		if (in_QM) {
			if (a == '\"') {
				in_QM = FALSE;
			}
			else {
				_argv[j] = a;
				j++;
			}
		}
		else {
			switch (a) {
			case '\"':
				in_QM = TRUE;
				in_TEXT = TRUE;
				if (in_SPACE) {
					argv[argc] = _argv + j;
					argc++;
				}
				in_SPACE = FALSE;
				break;
			case ' ':
			case '\t':
			case '\n':
			case '\r':
				if (in_TEXT) {
					_argv[j] = '\0';
					j++;
				}
				in_TEXT = FALSE;
				in_SPACE = TRUE;
				break;
			default:
				in_TEXT = TRUE;
				if (in_SPACE) {
					argv[argc] = _argv + j;
					argc++;
				}
				_argv[j] = a;
				j++;
				in_SPACE = FALSE;
				break;
			}
		}
		i++;
	}
	_argv[j] = '\0';
	argv[argc] = NULL;

	(*_argc) = argc;
	return argv;
}

DWORD WINAPI Emulate(unsigned int reserved_systems, blocks_reserved_t blocks_reserved)
{
	FUNC_EXPORTS

	/*! Verify our host executable, cxbxr-ldr.exe, is loaded to base address 0x00010000 */
	if (!VerifyBaseAddr()) {
		PopupError(nullptr, "cxbx-ldr.exe was not loaded to base address 0x00010000, which is a requirement for Xbox emulation.");
		return EXIT_FAILURE;
	}

	LPSTR CommandLine = GetCommandLine();
	if (!CommandLine) {
		PopupError(nullptr, "Couldn't retrieve command line!");
		return EXIT_FAILURE;
	}

	int argc = 0;
	PCHAR *argv = CommandLineToArgvA(CommandLine, &argc);
	if (!argv) {
		PopupError(nullptr, "Couldn't parse command line!");
		return EXIT_FAILURE;
	}

	if (!cli_config::GenConfig(argv, argc)) {
		PopupError(nullptr, "Couldn't convert parsed command line!");
		LocalFree(argv);
		return EXIT_FAILURE;
	}

	// The CLI normalizes both /key and --key into the config map. Reading
	// tracing from that map also preserves it across Chihiro QuickReboot.
#if defined(_DEBUG)
	g_FullTraceEnabled = cli_config::hasKey(cli_config::full_trace);
	g_PerfTraceEnabled =
		g_FullTraceEnabled || cli_config::hasKey(cli_config::perf_trace);
	g_RenderTraceEnabled =
		g_FullTraceEnabled || cli_config::hasKey(cli_config::render_trace);
#else
	g_FullTraceEnabled = false;
	g_PerfTraceEnabled = false;
	g_RenderTraceEnabled = false;
#endif
	LocalFree(argv);

	/*! verify load argument is included */
	if (!cli_config::hasKey("load")) {
		PopupError(nullptr, "No /load argument in command line!");
		return EXIT_FAILURE;
	}

	/*! initialize shared memory */
	if (!EmuShared::Init(cli_config::GetSessionID())) {
		PopupError(nullptr, "Could not map shared memory!");
		return EXIT_FAILURE;
	}

	// Check if the loader version matches the emu version and abort otherwise
	if (std::strncmp(GetGitVersionStr(), reinterpret_cast<char *>(PHYSICAL_MAP1_BASE + 0x1000), GetGitVersionLength()) != 0) {
		PopupError(nullptr, "Mismatch detected between cxbxr-ldr.exe and cxbxr-emu.dll, aborting.");
		EmuShared::Cleanup();
		return EXIT_FAILURE;
	}

	if (!HandleFirstLaunch()) {
		PopupError(nullptr, "First launch failed!");
		EmuShared::Cleanup();
		return EXIT_FAILURE;
	}

	if (!reserved_systems) {
		PopupError(nullptr, "Unable to preserve any system's memory ranges!");
		EmuShared::Cleanup();
		return EXIT_FAILURE;
	}

	if (UseAndroidWineEmulationStack()) {
		// Wine's WoW64 SEH dispatcher rejects an exception frame once CXBXR
		// crosses the original host thread's stack limit. Keep the loader PE
		// header unchanged because increasing its image stack reservation
		// interferes with CXBXR's fixed low-address mappings. Instead, copy the
		// reservation bitmap and run emulation on an explicitly larger stack.
		AndroidWineEmulationContext context = {};
		context.reservedSystems = reserved_systems;
		memcpy(
			context.blocksReserved,
			blocks_reserved,
			sizeof(context.blocksReserved));

		HANDLE emulationThread = CreateThread(
			nullptr,
			AndroidWineEmulationStackSize,
			AndroidWineEmulationThread,
			&context,
			STACK_SIZE_PARAM_IS_A_RESERVATION,
			nullptr);
		if (emulationThread == nullptr) {
			PopupError(
				nullptr,
				"Could not create the Android Wine emulation thread.");
			EmuShared::Cleanup();
			return EXIT_FAILURE;
		}

		WaitForSingleObject(emulationThread, INFINITE);
		CloseHandle(emulationThread);
	}
	else {
		CxbxKrnlEmulate(reserved_systems, blocks_reserved);
	}

	// QoD debug: log normal exit
#if defined(_DEBUG)
	{
		FILE* _f = fopen("C:\\temp\\qod_patches.log", "a");
		if (_f) {
			SYSTEMTIME _st; GetLocalTime(&_st);
			fprintf(_f, "[%02d:%02d:%02d.%03d] QoD: *** CxbxKrnlEmulate returned - normal exit path ***\n",
				_st.wHour, _st.wMinute, _st.wSecond, _st.wMilliseconds);
			fclose(_f);
		}
	}
#endif

	/*! cleanup shared memory */
	EmuShared::Cleanup();

	// Flush shader cache saves to disk before terminating — the save thread is
	// detached and TerminateProcess would kill it mid-write, losing all shaders
	// compiled this session.
	ShaderCacheShutdown();

	// Note : Emulate() must never return to it's caller (rawMain() in loader.cpp),
	// because that function resides in a block of memory that's overwritten with
	// xbox executable contents. Returning there would lead to undefined behaviour.
	// Since we're done emulating, it's al right to terminate here :
	TerminateProcess(GetCurrentProcess(), EXIT_SUCCESS);

	// This line will never be reached:
	return EXIT_FAILURE;
}
