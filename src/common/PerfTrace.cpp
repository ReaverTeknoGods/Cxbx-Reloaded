// ******************************************************************
// * PerfTrace.cpp - Implementation for lightweight perf tracing
// ******************************************************************
#include "PerfTrace.h"
#include <DbgHelp.h>
#include <TlHelp32.h>
#include <winternl.h>
#include <cstdarg>
#include <cstring>
#include <string>

#pragma comment(lib, "Dbghelp.lib")

bool g_PerfTraceEnabled = true;

ULONG g_VtxCacheHits = 0;
ULONG g_VtxCacheMisses = 0;

namespace PerfTraceInternal {

State g_state = {};

static bool EnsureSymbolsInitialized(State& s)
{
    if (s.symbolsReady) {
        return true;
    }

    BOOL initialized = SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    if (!initialized && GetLastError() == ERROR_INVALID_PARAMETER) {
        initialized = TRUE;
        SymRefreshModuleList(GetCurrentProcess());
    }

    s.symbolsReady = initialized == TRUE;
    return s.symbolsReady;
}

static int FindSampledThreadSlotById(const State& s, DWORD nativeThreadId)
{
    for (int i = 0; i < PERF_MAX_SAMPLED_THREADS; i++) {
        if (s.sampledThreads[i].handle != nullptr && s.sampledThreads[i].nativeThreadId == nativeThreadId) {
            return i;
        }
    }

    return -1;
}

static int FindFreeSampledThreadSlot(const State& s)
{
    for (int i = 0; i < PERF_MAX_SAMPLED_THREADS; i++) {
        if (s.sampledThreads[i].handle == nullptr) {
            return i;
        }
    }

    return -1;
}

static int FindXboxThreadIndexByNativeId(const State& s, DWORD nativeThreadId)
{
    for (int i = 0; i < s.xboxThreadCount; i++) {
        if (s.xboxThreads[i] != nullptr && s.xboxNativeThreadIds[i] == nativeThreadId) {
            return i;
        }
    }

    return -1;
}

static std::string ResolveSamplePc(State& s, uintptr_t pc);

static std::string FormatThreadRoutineLabel(State& s, const void* routine)
{
    if (routine == nullptr) {
        return "0x0";
    }

    return ResolveSamplePc(s, reinterpret_cast<uintptr_t>(routine));
}

static bool QueryThreadStartPc(HANDLE threadHandle, uintptr_t* startPc)
{
    if (threadHandle == nullptr || startPc == nullptr) {
        return false;
    }

    using NtQueryInformationThreadFn = NTSTATUS(NTAPI*)(HANDLE, THREADINFOCLASS, PVOID, ULONG, PULONG);
    static NtQueryInformationThreadFn s_ntQueryInformationThread = []() -> NtQueryInformationThreadFn {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (ntdll == nullptr) {
            return nullptr;
        }

        return reinterpret_cast<NtQueryInformationThreadFn>(
            GetProcAddress(ntdll, "NtQueryInformationThread"));
    }();

    if (s_ntQueryInformationThread == nullptr) {
        return false;
    }

    PVOID startAddress = nullptr;
    NTSTATUS status = s_ntQueryInformationThread(
        threadHandle,
        static_cast<THREADINFOCLASS>(9),
        &startAddress,
        static_cast<ULONG>(sizeof(startAddress)),
        nullptr);
    if (status < 0 || startAddress == nullptr) {
        return false;
    }

    *startPc = reinterpret_cast<uintptr_t>(startAddress);
    return true;
}

static bool TryFormatThreadDescription(HANDLE threadHandle, char* buffer, size_t bufferSize)
{
    if (threadHandle == nullptr || buffer == nullptr || bufferSize == 0) {
        return false;
    }

    using GetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    static GetThreadDescriptionFn s_getThreadDescription = []() -> GetThreadDescriptionFn {
        HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
        if (kernel32 == nullptr) {
            return nullptr;
        }

        return reinterpret_cast<GetThreadDescriptionFn>(
            GetProcAddress(kernel32, "GetThreadDescription"));
    }();

    if (s_getThreadDescription == nullptr) {
        return false;
    }

    PWSTR description = nullptr;
    HRESULT hr = s_getThreadDescription(threadHandle, &description);
    if (FAILED(hr) || description == nullptr || description[0] == L'\0') {
        if (description != nullptr) {
            LocalFree(description);
        }
        return false;
    }

    char utf8[PERF_MAX_XBOX_THREAD_LABEL] = {};
    int converted = WideCharToMultiByte(CP_UTF8, 0, description, -1, utf8, sizeof(utf8), nullptr, nullptr);
    LocalFree(description);
    if (converted <= 0 || utf8[0] == '\0') {
        return false;
    }

    strncpy_s(buffer, bufferSize, utf8, _TRUNCATE);
    return true;
}

static void FormatSampledThreadLabel(State& s, HANDLE threadHandle, DWORD nativeThreadId, char* buffer, size_t bufferSize)
{
    if (nativeThreadId == s.renderThreadId) {
        _snprintf_s(buffer, bufferSize, _TRUNCATE, "render-thread nt=%04X", (unsigned int)(nativeThreadId & 0xFFFFu));
        return;
    }

    int xboxIdx = FindXboxThreadIndexByNativeId(s, nativeThreadId);
    if (xboxIdx >= 0 && s.xboxThreadLabels[xboxIdx][0] != '\0') {
        strncpy_s(buffer, bufferSize, s.xboxThreadLabels[xboxIdx], _TRUNCATE);
        return;
    }

    if (TryFormatThreadDescription(threadHandle, buffer, bufferSize)) {
        return;
    }

    uintptr_t startPc = 0;
    if (QueryThreadStartPc(threadHandle, &startPc)) {
        std::string startLabel = ResolveSamplePc(s, startPc);
        _snprintf_s(
            buffer,
            bufferSize,
            _TRUNCATE,
            "%s nt=%04X",
            startLabel.c_str(),
            (unsigned int)(nativeThreadId & 0xFFFFu));
        return;
    }

    _snprintf_s(buffer, bufferSize, _TRUNCATE, "nt=%04X", (unsigned int)(nativeThreadId & 0xFFFFu));
}

static void ResetSampledThread(SampledThread& sampledThread)
{
    if (sampledThread.handle != nullptr) {
        CloseHandle(sampledThread.handle);
    }

    memset(&sampledThread, 0, sizeof(sampledThread));
}

static void RefreshSampledThreads(State& s)
{
    bool seen[PERF_MAX_SAMPLED_THREADS] = {};
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return;
    }

    THREADENTRY32 entry = {};
    entry.dwSize = sizeof(entry);
    const DWORD processId = GetCurrentProcessId();
    const DWORD samplerThreadId = GetCurrentThreadId();

    if (Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID != processId || entry.th32ThreadID == samplerThreadId) {
                continue;
            }

            int slot = FindSampledThreadSlotById(s, entry.th32ThreadID);
            if (slot < 0) {
                slot = FindFreeSampledThreadSlot(s);
                if (slot < 0) {
                    continue;
                }

                HANDLE threadHandle = OpenThread(
                    THREAD_QUERY_INFORMATION | THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME,
                    FALSE,
                    entry.th32ThreadID);
                if (threadHandle == nullptr) {
                    continue;
                }

                ULONG64 cycles = 0;
                QueryThreadCycleTime(threadHandle, &cycles);

                s.sampledThreads[slot].handle = threadHandle;
                s.sampledThreads[slot].nativeThreadId = entry.th32ThreadID;
                s.sampledThreads[slot].cycleStart = cycles;
                s.sampledThreads[slot].hits = 0;
                memset(s.sampledThreads[slot].pcSamples, 0, sizeof(s.sampledThreads[slot].pcSamples));
            }

            FormatSampledThreadLabel(
                s,
                s.sampledThreads[slot].handle,
                entry.th32ThreadID,
                s.sampledThreads[slot].label,
                sizeof(s.sampledThreads[slot].label));
            seen[slot] = true;
        } while (Thread32Next(snapshot, &entry));
    }

    CloseHandle(snapshot);

    for (int i = 0; i < PERF_MAX_SAMPLED_THREADS; i++) {
        if (s.sampledThreads[i].handle != nullptr && !seen[i]) {
            ResetSampledThread(s.sampledThreads[i]);
        }
    }
}

static bool TryGetModuleNameForPc(uintptr_t pc, char* buffer, size_t bufferSize)
{
    if (buffer == nullptr || bufferSize == 0) {
        return false;
    }

    buffer[0] = '\0';
    HMODULE moduleHandle = nullptr;
    if (!GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(pc),
            &moduleHandle)
        || moduleHandle == nullptr) {
        return false;
    }

    char modulePath[MAX_PATH] = {};
    if (GetModuleFileNameA(moduleHandle, modulePath, sizeof(modulePath)) == 0) {
        return false;
    }

    const char* slash = strrchr(modulePath, '\\');
    strncpy_s(buffer, bufferSize, slash ? slash + 1 : modulePath, _TRUNCATE);
    return true;
}

static bool IsWindowsWaitHelperPc(uintptr_t pc)
{
    char moduleName[MAX_PATH] = {};
    if (!TryGetModuleNameForPc(pc, moduleName, sizeof(moduleName))) {
        return false;
    }

    return _stricmp(moduleName, "ntdll.dll") == 0
        || _stricmp(moduleName, "kernelbase.dll") == 0
        || _stricmp(moduleName, "kernel32.dll") == 0;
}

static bool CaptureThreadAttributionPc(State& s, HANDLE threadHandle, const CONTEXT& threadContext, uintptr_t leafPc, uintptr_t* pc)
{
    if (pc == nullptr) {
        return false;
    }

    *pc = leafPc;
    if (leafPc == 0 || !IsWindowsWaitHelperPc(leafPc) || !EnsureSymbolsInitialized(s)) {
        return *pc != 0;
    }

    CONTEXT unwindContext = threadContext;
    STACKFRAME64 frame = {};
#if defined(_M_X64)
    constexpr DWORD machineType = IMAGE_FILE_MACHINE_AMD64;
    frame.AddrPC.Offset = static_cast<DWORD64>(unwindContext.Rip);
    frame.AddrFrame.Offset = static_cast<DWORD64>(unwindContext.Rbp);
    frame.AddrStack.Offset = static_cast<DWORD64>(unwindContext.Rsp);
#else
    constexpr DWORD machineType = IMAGE_FILE_MACHINE_I386;
    frame.AddrPC.Offset = static_cast<DWORD64>(unwindContext.Eip);
    frame.AddrFrame.Offset = static_cast<DWORD64>(unwindContext.Ebp);
    frame.AddrStack.Offset = static_cast<DWORD64>(unwindContext.Esp);
#endif
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Mode = AddrModeFlat;

    // When a thread is parked in host wait helpers, attribute the sample to the
    // first non-Windows helper caller frame so the report points at owning code.
    for (int depth = 0; depth < 4; depth++) {
        if (!StackWalk64(
                machineType,
                GetCurrentProcess(),
                threadHandle,
                &frame,
                &unwindContext,
                nullptr,
                SymFunctionTableAccess64,
                SymGetModuleBase64,
                nullptr)) {
            break;
        }

        uintptr_t candidatePc = static_cast<uintptr_t>(frame.AddrPC.Offset);
        if (candidatePc == 0 || candidatePc == leafPc) {
            continue;
        }
        if (IsWindowsWaitHelperPc(candidatePc)) {
            continue;
        }

        *pc = candidatePc;
        break;
    }

    return *pc != 0;
}

static bool CaptureThreadPc(State& s, HANDLE threadHandle, uintptr_t* pc)
{
    if (threadHandle == nullptr || pc == nullptr) {
        return false;
    }

    CONTEXT context = {};
    context.ContextFlags = CONTEXT_FULL;

    DWORD suspendCount = SuspendThread(threadHandle);
    if (suspendCount == static_cast<DWORD>(-1)) {
        return false;
    }

    BOOL gotContext = GetThreadContext(threadHandle, &context);
    bool captured = false;
    if (!gotContext) {
        ResumeThread(threadHandle);
        return false;
    }

    uintptr_t leafPc = 0;
#if defined(_M_X64)
    leafPc = static_cast<uintptr_t>(context.Rip);
#else
    leafPc = static_cast<uintptr_t>(context.Eip);
#endif
    if (leafPc != 0) {
        captured = CaptureThreadAttributionPc(s, threadHandle, context, leafPc, pc);
    }

    ResumeThread(threadHandle);
    return captured;
}

static void RecordProcessPcSample(State& s, uintptr_t pc, ULONGLONG cycles)
{
    if (pc == 0) {
        return;
    }

    s.processPcSampleTotal++;
    s.processPcSampleTotalCycles += cycles;
    for (int i = 0; i < PERF_MAX_PC_SAMPLES; i++) {
        if (s.processPcSamples[i].pc == pc) {
            s.processPcSamples[i].hits++;
            s.processPcSamples[i].cycles += cycles;
            return;
        }
    }

    for (int i = 0; i < PERF_MAX_PC_SAMPLES; i++) {
        if (s.processPcSamples[i].pc == 0) {
            s.processPcSamples[i].pc = pc;
            s.processPcSamples[i].hits = 1;
            s.processPcSamples[i].cycles = cycles;
            return;
        }
    }
}

static void RecordThreadPcSample(SampledThread& sampledThread, uintptr_t pc, ULONGLONG cycles)
{
    if (pc == 0) {
        return;
    }

    for (int i = 0; i < PERF_MAX_THREAD_PC_SAMPLES; i++) {
        if (sampledThread.pcSamples[i].pc == pc) {
            sampledThread.pcSamples[i].hits++;
            sampledThread.pcSamples[i].cycles += cycles;
            return;
        }
    }

    for (int i = 0; i < PERF_MAX_THREAD_PC_SAMPLES; i++) {
        if (sampledThread.pcSamples[i].pc == 0) {
            sampledThread.pcSamples[i].pc = pc;
            sampledThread.pcSamples[i].hits = 1;
            sampledThread.pcSamples[i].cycles = cycles;
            return;
        }
    }
}

static void RecordXboxThreadPcSample(State& s, int xboxThreadIdx, uintptr_t pc, ULONGLONG cycles)
{
    if (xboxThreadIdx < 0 || xboxThreadIdx >= PERF_MAX_XBOX_THREADS || pc == 0) {
        return;
    }

    s.xboxThreadSampleHits[xboxThreadIdx]++;
    s.xboxThreadSampleCycles[xboxThreadIdx] += cycles;
    auto* samples = s.xboxThreadPcSamples[xboxThreadIdx];
    for (int i = 0; i < PERF_MAX_THREAD_PC_SAMPLES; i++) {
        if (samples[i].pc == pc) {
            samples[i].hits++;
            samples[i].cycles += cycles;
            return;
        }
    }

    for (int i = 0; i < PERF_MAX_THREAD_PC_SAMPLES; i++) {
        if (samples[i].pc == 0) {
            samples[i].pc = pc;
            samples[i].hits = 1;
            samples[i].cycles = cycles;
            return;
        }
    }
}

static std::string ResolveSamplePc(State& s, uintptr_t pc)
{
    char buffer[512] = {};
    HMODULE moduleHandle = nullptr;

    if (GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(pc),
            &moduleHandle)
        && moduleHandle != nullptr) {
        char modulePath[MAX_PATH] = {};
        GetModuleFileNameA(moduleHandle, modulePath, sizeof(modulePath));
        const char* slash = strrchr(modulePath, '\\');
        const char* moduleName = slash ? slash + 1 : modulePath;
        DWORD64 moduleOffset = static_cast<DWORD64>(pc) - reinterpret_cast<DWORD64>(moduleHandle);

        if (EnsureSymbolsInitialized(s)) {
            BYTE symBuffer[sizeof(SYMBOL_INFO) + 256] = {};
            PSYMBOL_INFO symbol = reinterpret_cast<PSYMBOL_INFO>(symBuffer);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 255;
            DWORD64 symbolDisplacement = 0;

            if (SymFromAddr(GetCurrentProcess(), static_cast<DWORD64>(pc), &symbolDisplacement, symbol) && symbol->Name[0] != '\0') {
                _snprintf_s(
                    buffer,
                    sizeof(buffer),
                    _TRUNCATE,
                    "%s!%s+0x%llX",
                    moduleName,
                    symbol->Name,
                    static_cast<unsigned long long>(symbolDisplacement));
                return buffer;
            }
        }

        _snprintf_s(
            buffer,
            sizeof(buffer),
            _TRUNCATE,
            "%s+0x%llX",
            moduleName,
            static_cast<unsigned long long>(moduleOffset));
        return buffer;
    }

    _snprintf_s(buffer, sizeof(buffer), _TRUNCATE, "0x%p", reinterpret_cast<void*>(pc));
    return buffer;
}

static void FindTopSampledThreadIndices(const SampledThread* threads, int topIdx[PERF_REPORT_TOP_SAMPLED_THREADS])
{
    for (int i = 0; i < PERF_REPORT_TOP_SAMPLED_THREADS; i++) {
        topIdx[i] = -1;
    }

    for (int idx = 0; idx < PERF_MAX_SAMPLED_THREADS; idx++) {
        if (threads[idx].handle == nullptr || threads[idx].cycles == 0) {
            continue;
        }

        for (int slot = 0; slot < PERF_REPORT_TOP_SAMPLED_THREADS; slot++) {
            if (topIdx[slot] < 0 || threads[idx].cycles > threads[topIdx[slot]].cycles) {
                for (int move = PERF_REPORT_TOP_SAMPLED_THREADS - 1; move > slot; move--) {
                    topIdx[move] = topIdx[move - 1];
                }
                topIdx[slot] = idx;
                break;
            }
        }
    }
}

static void FindTopPcSampleIndices(const PcSample* samples, int topIdx[PERF_REPORT_TOP_PC_SAMPLES])
{
    for (int i = 0; i < PERF_REPORT_TOP_PC_SAMPLES; i++) {
        topIdx[i] = -1;
    }

    for (int idx = 0; idx < PERF_MAX_PC_SAMPLES; idx++) {
        if (samples[idx].pc == 0 || samples[idx].cycles == 0) {
            continue;
        }

        for (int slot = 0; slot < PERF_REPORT_TOP_PC_SAMPLES; slot++) {
            if (topIdx[slot] < 0 || samples[idx].cycles > samples[topIdx[slot]].cycles) {
                for (int move = PERF_REPORT_TOP_PC_SAMPLES - 1; move > slot; move--) {
                    topIdx[move] = topIdx[move - 1];
                }
                topIdx[slot] = idx;
                break;
            }
        }
    }
}

static void FindTopThreadPcSampleIndices(const SampledThread::ThreadPcSample* samples, int topIdx[PERF_REPORT_TOP_THREAD_PC_SAMPLES])
{
    for (int i = 0; i < PERF_REPORT_TOP_THREAD_PC_SAMPLES; i++) {
        topIdx[i] = -1;
    }

    for (int idx = 0; idx < PERF_MAX_THREAD_PC_SAMPLES; idx++) {
        if (samples[idx].pc == 0 || samples[idx].cycles == 0) {
            continue;
        }

        for (int slot = 0; slot < PERF_REPORT_TOP_THREAD_PC_SAMPLES; slot++) {
            if (topIdx[slot] < 0 || samples[idx].cycles > samples[topIdx[slot]].cycles) {
                for (int move = PERF_REPORT_TOP_THREAD_PC_SAMPLES - 1; move > slot; move--) {
                    topIdx[move] = topIdx[move - 1];
                }
                topIdx[slot] = idx;
                break;
            }
        }
    }
}

static DWORD WINAPI SampleProfilerThreadProc(LPVOID)
{
    auto& s = g_state;

    while (true) {
        Sleep(PERF_SAMPLER_INTERVAL_MS);
        if (!g_PerfTraceEnabled || !s.initialized || !s.logFile || !s.sampleLockInitialized) {
            continue;
        }

        EnterCriticalSection(&s.sampleLock);

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (s.sampleRefreshTick == 0 || (now.QuadPart - s.sampleRefreshTick) >= s.freq) {
            RefreshSampledThreads(s);
            s.sampleRefreshTick = now.QuadPart;
        }

        for (int i = 0; i < PERF_MAX_SAMPLED_THREADS; i++) {
            auto& sampledThread = s.sampledThreads[i];
            if (sampledThread.handle == nullptr) {
                continue;
            }

            ULONG64 cycles = 0;
            if (!QueryThreadCycleTime(sampledThread.handle, &cycles)) {
                ResetSampledThread(sampledThread);
                continue;
            }

            ULONG64 delta = cycles - sampledThread.cycleStart;
            sampledThread.cycleStart = cycles;
            if (delta == 0) {
                continue;
            }

            uintptr_t pc = 0;
            if (CaptureThreadPc(s, sampledThread.handle, &pc)) {
                sampledThread.hits++;
                sampledThread.cycles += delta;
                RecordThreadPcSample(sampledThread, pc, delta);
                RecordProcessPcSample(s, pc, delta);
            }
        }

        for (int i = 0; i < s.xboxThreadCount; i++) {
            HANDLE xboxThread = s.xboxThreads[i];
            if (xboxThread == nullptr) {
                continue;
            }

            ULONG64 cycles = 0;
            if (!QueryThreadCycleTime(xboxThread, &cycles)) {
                continue;
            }

            ULONG64 delta = cycles - s.xboxThreadSampleCycleStart[i];
            s.xboxThreadSampleCycleStart[i] = cycles;
            if (delta == 0) {
                continue;
            }

            uintptr_t pc = 0;
            if (CaptureThreadPc(s, xboxThread, &pc)) {
                RecordXboxThreadPcSample(s, i, pc, delta);
            }
        }

        LeaveCriticalSection(&s.sampleLock);
    }
}

static void FindTopXboxThreadIndices(const double* values, int count, int topIdx[PERF_REPORT_TOP_XBOX_THREADS])
{
    for (int i = 0; i < PERF_REPORT_TOP_XBOX_THREADS; i++) {
        topIdx[i] = -1;
    }

    for (int idx = 0; idx < count; idx++) {
        double value = values[idx];
        if (value <= 0.0) {
            continue;
        }

        for (int slot = 0; slot < PERF_REPORT_TOP_XBOX_THREADS; slot++) {
            if (topIdx[slot] < 0 || value > values[topIdx[slot]]) {
                for (int move = PERF_REPORT_TOP_XBOX_THREADS - 1; move > slot; move--) {
                    topIdx[move] = topIdx[move - 1];
                }
                topIdx[slot] = idx;
                break;
            }
        }
    }
}

static void FindTopXboxThreadTickIndices(const long long* values, int count, int topIdx[PERF_REPORT_TOP_XBOX_THREADS])
{
    for (int i = 0; i < PERF_REPORT_TOP_XBOX_THREADS; i++) {
        topIdx[i] = -1;
    }

    for (int idx = 0; idx < count; idx++) {
        long long value = values[idx];
        if (value <= 0) {
            continue;
        }

        for (int slot = 0; slot < PERF_REPORT_TOP_XBOX_THREADS; slot++) {
            if (topIdx[slot] < 0 || value > values[topIdx[slot]]) {
                for (int move = PERF_REPORT_TOP_XBOX_THREADS - 1; move > slot; move--) {
                    topIdx[move] = topIdx[move - 1];
                }
                topIdx[slot] = idx;
                break;
            }
        }
    }
}

static void FindTopXboxThreadCountIndices(const uint32_t* values, int count, int topIdx[PERF_REPORT_TOP_XBOX_THREADS])
{
    for (int i = 0; i < PERF_REPORT_TOP_XBOX_THREADS; i++) {
        topIdx[i] = -1;
    }

    for (int idx = 0; idx < count; idx++) {
        uint32_t value = values[idx];
        if (value == 0) {
            continue;
        }

        for (int slot = 0; slot < PERF_REPORT_TOP_XBOX_THREADS; slot++) {
            if (topIdx[slot] < 0 || value > values[topIdx[slot]]) {
                for (int move = PERF_REPORT_TOP_XBOX_THREADS - 1; move > slot; move--) {
                    topIdx[move] = topIdx[move - 1];
                }
                topIdx[slot] = idx;
                break;
            }
        }
    }
}

static void FindTopKickDrainIndices(const long long* values, int count, int topIdx[PERF_REPORT_TOP_VS_KICKDRAIN_KEYS])
{
    for (int i = 0; i < PERF_REPORT_TOP_VS_KICKDRAIN_KEYS; i++) {
        topIdx[i] = -1;
    }

    for (int idx = 0; idx < count; idx++) {
        long long value = values[idx];
        if (value <= 0) {
            continue;
        }

        for (int slot = 0; slot < PERF_REPORT_TOP_VS_KICKDRAIN_KEYS; slot++) {
            if (topIdx[slot] < 0 || value > values[topIdx[slot]]) {
                for (int move = PERF_REPORT_TOP_VS_KICKDRAIN_KEYS - 1; move > slot; move--) {
                    topIdx[move] = topIdx[move - 1];
                }
                topIdx[slot] = idx;
                break;
            }
        }
    }
}

static void FindTopKickDrainMethodIndices(const uint32_t* values, int count, int topIdx[PERF_REPORT_TOP_VS_KICKDRAIN_METHODS])
{
    for (int i = 0; i < PERF_REPORT_TOP_VS_KICKDRAIN_METHODS; i++) {
        topIdx[i] = -1;
    }

    for (int idx = 0; idx < count; idx++) {
        uint32_t value = values[idx];
        if (value == 0) {
            continue;
        }

        for (int slot = 0; slot < PERF_REPORT_TOP_VS_KICKDRAIN_METHODS; slot++) {
            if (topIdx[slot] < 0 || value > values[topIdx[slot]]) {
                for (int move = PERF_REPORT_TOP_VS_KICKDRAIN_METHODS - 1; move > slot; move--) {
                    topIdx[move] = topIdx[move - 1];
                }
                topIdx[slot] = idx;
                break;
            }
        }
    }
}

static int FindOrCreateKickDrainSlot(State& s, uint64_t shaderKey, uint32_t shaderHandle, uint64_t shaderCacheHash)
{
    if (shaderKey == 0 && shaderHandle == 0) {
        return -1;
    }

    for (int i = 0; i < PERF_MAX_VS_KICKDRAIN_KEYS; i++) {
        if (s.vsKickDrainKeys[i] == shaderKey && s.vsKickDrainHandles[i] == shaderHandle) {
			if (s.vsKickDrainCacheHashes[i] == 0 && shaderCacheHash != 0) {
				s.vsKickDrainCacheHashes[i] = shaderCacheHash;
			}
            return i;
        }
    }

    for (int i = 0; i < PERF_MAX_VS_KICKDRAIN_KEYS; i++) {
        if (s.vsKickDrainKeys[i] == 0 && s.vsKickDrainHandles[i] == 0
            && s.vsKickDrainWindowTicks[i] == 0 && s.vsKickDrainFrameTicks[i] == 0) {
            s.vsKickDrainKeys[i] = shaderKey;
            s.vsKickDrainHandles[i] = shaderHandle;
			s.vsKickDrainCacheHashes[i] = shaderCacheHash;
            return i;
        }
    }

    return -1;
}

static int FindOrCreateKickDrainMethodSlot(State& s, uint32_t method)
{
    if (method == 0xFFFFFFFFu) {
        return -1;
    }

    for (int i = 0; i < PERF_MAX_VS_KICKDRAIN_METHODS; i++) {
        if (s.vsKickDrainBlockerMethods[i] == method) {
            return i;
        }
    }

    for (int i = 0; i < PERF_MAX_VS_KICKDRAIN_METHODS; i++) {
        if (s.vsKickDrainBlockerMethods[i] == 0 && s.vsKickDrainBlockerMethodCounts[i] == 0) {
            s.vsKickDrainBlockerMethods[i] = method;
            return i;
        }
    }

    return -1;
}

const char* g_catNames[PERF_CAT_COUNT] = {
    "Swap            ",
    "UpdateNative    ",
    "UpdateTextures  ",
    "UpdateVtxShader ",
    "UpdatePxlShader ",
    "Draw            ",
    "CreateResource  ",
    "VtxConvert      ",
    "ShaderCompile   ",
    "LockUnlock      ",
    "Present         ",
    "Blit            ",
    "RenderStates    ",
    "TextureStates   ",
    "VtxDecl+Const   ",
    "VS_Decl         ",
    "VS_Const        ",
    "Viewport        ",
    "GetBackbuffer   ",
    "FrameSleep      ",
    "EndScene        ",
    "EmuX86VEH       ",
    "KeWait          ",
    "KeDelay         ",
    "VS_KickDrain    ",
    "VS_Upload       ",
    "XcpuRun         ",
    "VS_BoneSync     ",
    "VS_BoneUpload   ",
};

void RegisterXboxThread(HANDLE h, DWORD xboxThreadId, DWORD nativeThreadId, const void* systemRoutine, const void* startRoutine)
{
    auto& s = g_state;
    // Reserve a slot atomically — thread-safe, up to PERF_MAX_XBOX_THREADS threads.
    LONG idx = InterlockedIncrement((LONG*)&s.xboxThreadCount) - 1;
    if (idx >= PERF_MAX_XBOX_THREADS) {
        // Undo overshoot so the count stays accurate.
        InterlockedDecrement((LONG*)&s.xboxThreadCount);
        return;
    }

    // Duplicate the handle so PerfTrace has its own reference independent of the
    // Xbox handle table lifecycle.  The dup is intentionally never closed (it
    // leaks at emulator exit, which is acceptable).
    HANDLE dup = INVALID_HANDLE_VALUE;
    DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &dup,
                    THREAD_QUERY_INFORMATION | THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, 0);
    if (dup == INVALID_HANDLE_VALUE) {
        InterlockedDecrement((LONG*)&s.xboxThreadCount);
        return;
    }

    // Snapshot the initial cumulative TSC cycle count so the first frame's delta
    // starts from registration, not thread birth.
    ULONG64 cycles = 0;
    QueryThreadCycleTime(dup, &cycles);

    s.xboxThreads[idx] = dup;
    s.xboxThreadCycleStart[idx] = cycles;
    s.xboxThreadIds[idx] = xboxThreadId;
    s.xboxNativeThreadIds[idx] = nativeThreadId;
    s.xboxThreadSystemRoutine[idx] = systemRoutine;
    s.xboxThreadStartRoutine[idx] = startRoutine;
    s.xboxThreadSampleCycleStart[idx] = cycles;
    std::string startLabel = FormatThreadRoutineLabel(s, startRoutine);
    std::string systemLabel = FormatThreadRoutineLabel(s, systemRoutine);
    _snprintf_s(
        s.xboxThreadLabels[idx],
        PERF_MAX_XBOX_THREAD_LABEL,
        _TRUNCATE,
        "xb=%04X nt=%04X sr=%s sys=%s",
        (unsigned int)(xboxThreadId & 0xFFFFu),
        (unsigned int)(nativeThreadId & 0xFFFFu),
        startLabel.c_str(),
        systemLabel.c_str());
}

void RecordVSKickDrain(uint64_t shaderKey, uint32_t shaderHandle, uint64_t shaderCacheHash, long long elapsedTicks, uint32_t reasonMask)
{
    auto& s = g_state;
    int slot = FindOrCreateKickDrainSlot(s, shaderKey, shaderHandle, shaderCacheHash);
    if (slot < 0) {
        return;
    }

    s.vsKickDrainFrameTicks[slot] += elapsedTicks;
    s.vsKickDrainWindowTicks[slot] += elapsedTicks;
    if (reasonMask & PERF_VS_KICKDRAIN_REASON_UNRELIABLE_PARSE) {
        s.vsKickDrainUnreliableCount[slot]++;
    }
    if (reasonMask & PERF_VS_KICKDRAIN_REASON_MORE_PENDING) {
        s.vsKickDrainMorePendingCount[slot]++;
    }
    if (reasonMask & PERF_VS_KICKDRAIN_REASON_NON_CONSTANT) {
        s.vsKickDrainNonConstantCount[slot]++;
    }
    if (reasonMask & PERF_VS_KICKDRAIN_REASON_PARSED_CONSTANTS) {
        s.vsKickDrainParsedConstantsCount[slot]++;
    }
}

void RecordVSKickDrainBlockerMethod(uint32_t method)
{
    auto& s = g_state;
    int slot = FindOrCreateKickDrainMethodSlot(s, method);
    if (slot < 0) {
        return;
    }

    s.vsKickDrainBlockerMethodCounts[slot]++;
}

void RecordPFIFOPending(uint32_t currentDepth, uint32_t highWater, uint32_t overflowCount)
{
    auto& s = g_state;
    s.pfifoPendingDepth = currentDepth;

    if (highWater > s.pfifoPendingHighWater) {
        s.pfifoPendingHighWater = highWater;
    }

    if (overflowCount >= s.pfifoPendingOverflowLastSeen) {
        s.pfifoPendingOverflowWindowCount += (overflowCount - s.pfifoPendingOverflowLastSeen);
    }
    else {
        s.pfifoPendingOverflowWindowCount += overflowCount;
    }

    s.pfifoPendingOverflowLastSeen = overflowCount;
}

static void RecordThreadPoll(uint32_t* frameCounts, uint32_t* windowCounts)
{
    auto& s = g_state;
    DWORD currentThreadId = GetCurrentThreadId();

    for (int i = 0; i < s.xboxThreadCount; i++) {
        if (s.xboxNativeThreadIds[i] != currentThreadId) {
            continue;
        }

        frameCounts[i]++;
        windowCounts[i]++;
        break;
    }
}

void RecordDelayPoll()
{
    auto& s = g_state;
    RecordThreadPoll(s.xboxThreadDelayPollFrameCounts, s.xboxThreadDelayPollWindowCounts);
}

void RecordWaitPoll()
{
    auto& s = g_state;
    RecordThreadPoll(s.xboxThreadWaitPollFrameCounts, s.xboxThreadWaitPollWindowCounts);
}

void Init(const char* logPath)
{
    auto& s = g_state;
    if (s.initialized) return;

    // System-thread proxies can register before PerfTrace_Init runs. Preserve
    // that registration data so xcpu accounting survives initialization.
    HANDLE preservedXboxThreads[PERF_MAX_XBOX_THREADS] = {};
    ULONGLONG preservedXboxThreadCycleStart[PERF_MAX_XBOX_THREADS] = {};
    DWORD preservedXboxThreadIds[PERF_MAX_XBOX_THREADS] = {};
    DWORD preservedXboxNativeThreadIds[PERF_MAX_XBOX_THREADS] = {};
    const void* preservedXboxThreadSystemRoutine[PERF_MAX_XBOX_THREADS] = {};
    const void* preservedXboxThreadStartRoutine[PERF_MAX_XBOX_THREADS] = {};
    char preservedXboxThreadLabels[PERF_MAX_XBOX_THREADS][PERF_MAX_XBOX_THREAD_LABEL] = {};
    ULONGLONG preservedXboxThreadSampleCycleStart[PERF_MAX_XBOX_THREADS] = {};
    int preservedXboxThreadCount = s.xboxThreadCount;
    if (preservedXboxThreadCount < 0) {
        preservedXboxThreadCount = 0;
    }
    if (preservedXboxThreadCount > PERF_MAX_XBOX_THREADS) {
        preservedXboxThreadCount = PERF_MAX_XBOX_THREADS;
    }

    memcpy(preservedXboxThreads, s.xboxThreads, sizeof(preservedXboxThreads));
    memcpy(preservedXboxThreadCycleStart, s.xboxThreadCycleStart, sizeof(preservedXboxThreadCycleStart));
    memcpy(preservedXboxThreadIds, s.xboxThreadIds, sizeof(preservedXboxThreadIds));
    memcpy(preservedXboxNativeThreadIds, s.xboxNativeThreadIds, sizeof(preservedXboxNativeThreadIds));
    memcpy(preservedXboxThreadSystemRoutine, s.xboxThreadSystemRoutine, sizeof(preservedXboxThreadSystemRoutine));
    memcpy(preservedXboxThreadStartRoutine, s.xboxThreadStartRoutine, sizeof(preservedXboxThreadStartRoutine));
    memcpy(preservedXboxThreadLabels, s.xboxThreadLabels, sizeof(preservedXboxThreadLabels));
    memcpy(preservedXboxThreadSampleCycleStart, s.xboxThreadSampleCycleStart, sizeof(preservedXboxThreadSampleCycleStart));

    memset(&s, 0, sizeof(s));
    s.xboxThreadCount = preservedXboxThreadCount;
    memcpy(s.xboxThreads, preservedXboxThreads, sizeof(s.xboxThreads));
    memcpy(s.xboxThreadCycleStart, preservedXboxThreadCycleStart, sizeof(s.xboxThreadCycleStart));
    memcpy(s.xboxThreadIds, preservedXboxThreadIds, sizeof(s.xboxThreadIds));
    memcpy(s.xboxNativeThreadIds, preservedXboxNativeThreadIds, sizeof(s.xboxNativeThreadIds));
    memcpy(s.xboxThreadSystemRoutine, preservedXboxThreadSystemRoutine, sizeof(s.xboxThreadSystemRoutine));
    memcpy(s.xboxThreadStartRoutine, preservedXboxThreadStartRoutine, sizeof(s.xboxThreadStartRoutine));
    memcpy(s.xboxThreadLabels, preservedXboxThreadLabels, sizeof(s.xboxThreadLabels));
    memcpy(s.xboxThreadSampleCycleStart, preservedXboxThreadSampleCycleStart, sizeof(s.xboxThreadSampleCycleStart));

    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    s.freq = freq.QuadPart;
    s.windowStartTick = now.QuadPart;
    s.swapStartTick = 0;

    s.logFile = fopen(logPath, "w");
    if (s.logFile) {
        fprintf(s.logFile,
            "# Cxbx-Reloaded PerfTrace — report every %.0f s\n"
            "# columns: frame | ftime(ms) | fps(instant) | fps(~smooth) | xcpu(ms,xN threads) | swap | blit | pres | end | sleep | gbuf | native(xN) | tex | ps | vs | draw | vtx(hits/misses) | rsrc(xN) | cmp | rs | ts | vd[dc co bs kd bu up vp] | kewait kedelay | rkw(render-thread kewait) rkd(render-thread kedelay)\n"
            "# suspect frames may emit xthr lines with top Xbox CPU threads: xb=<guest id> nt=<native id> sr=<start routine> sys=<system routine>\n"
            "# window summaries also emit SampleThread, SampleThreadPc, and PcHotspot lines from a %d ms process-wide PC sampler\n"
            "#\n",
            (double)PERF_REPORT_INTERVAL_S,
            PERF_SAMPLER_INTERVAL_MS);
        fflush(s.logFile);
        fprintf(stdout, "[PerfTrace] logging to %s\n", logPath);
    } else {
        fprintf(stderr, "[PerfTrace] ERROR: cannot open %s\n", logPath);
    }

    if (s.logFile) {
        InitializeCriticalSection(&s.sampleLock);
        s.sampleLockInitialized = true;
        EnsureSymbolsInitialized(s);
        s.samplerThread = CreateThread(nullptr, 0, SampleProfilerThreadProc, nullptr, 0, &s.samplerThreadId);
    }

    // Calibrate CPU frequency for TSC cycle -> ms conversion.
    // Measure how many TSC cycles elapse over a short QPC interval.
    {
        ULONG64 cyc0 = 0, cyc1 = 0;
        LARGE_INTEGER t0, t1;
        QueryThreadCycleTime(GetCurrentThread(), &cyc0);
        QueryPerformanceCounter(&t0);
        // Spin for ~5ms to get a stable sample without Sleep() (which yields the thread).
        long long spinTarget = t0.QuadPart + s.freq / 200; // 1/200 s = 5ms
        do { QueryPerformanceCounter(&t1); } while (t1.QuadPart < spinTarget);
        QueryThreadCycleTime(GetCurrentThread(), &cyc1);
        double elapsedSec = (double)(t1.QuadPart - t0.QuadPart) / (double)s.freq;
        s.cpuGhz = (double)(cyc1 - cyc0) / elapsedSec / 1e9;
        if (s.cpuGhz < 0.5 || s.cpuGhz > 10.0) s.cpuGhz = 3.0; // fallback guard
    }

    s.initialized = true;
}

void OnSwapBegin()
{
    auto& s = g_state;
    if (!s.logFile) return;

    // Record the render thread ID — the host thread that calls D3DDevice_Swap IS the render thread.
    s.renderThreadId = GetCurrentThreadId();

    // Capture current tick for frame-time measurement before any other work.
    LARGE_INTEGER nowLI;
    QueryPerformanceCounter(&nowLI);
    long long nowTick = nowLI.QuadPart;

    // Compute frame-to-frame time and FPS using the tick from the previous OnSwapBegin.
    double frameDeltaMs = 0.0;
    double instantFps = 0.0;
    if (s.lastSwapBeginTick != 0 && s.frameCount > 0) {
        frameDeltaMs = (double)(nowTick - s.lastSwapBeginTick) / (double)s.freq * 1000.0;
        if (frameDeltaMs > 0.0) {
            instantFps = 1000.0 / frameDeltaMs;
            s.smoothFps = (s.smoothFps == 0.0) ? instantFps : (0.9 * s.smoothFps + 0.1 * instantFps);
        }
    }
    s.lastSwapBeginTick = nowTick;

    // Sample Xbox thread CPU time using QueryThreadCycleTime (TSC-based, nanosecond resolution).
    // Unlike GetThreadTimes, this has no 15.625ms quantization — reads the hardware TSC directly.
    // Divide by cpuGhz (cycles/ns) to get milliseconds.
    double xcpuMs = 0.0;
    ULONGLONG xcpuCycles = 0;
    int nthr = s.xboxThreadCount; // read snapshot; newly-added threads missed this frame only
    memset(s.xcpuThreadMs, 0, sizeof(s.xcpuThreadMs));
    for (int i = 0; i < nthr; i++) {
        ULONG64 cur = 0;
        if (QueryThreadCycleTime(s.xboxThreads[i], &cur)) {
            ULONG64 delta = cur - s.xboxThreadCycleStart[i];
            double deltaMs = (double)delta / (s.cpuGhz * 1e6); // cycles / (GHz * 1e6) = ms
            s.xcpuThreadMs[i] = deltaMs;
            s.xcpuThreadWindowMs[i] += deltaMs;
            if (deltaMs > s.xcpuThreadMaxFrameMs[i]) s.xcpuThreadMaxFrameMs[i] = deltaMs;
            xcpuMs += deltaMs;
            xcpuCycles += delta;
            s.xboxThreadCycleStart[i] = cur;
        }
    }
    s.xcpuMs = xcpuMs;
    s.xcpuCyclesPerFrame = xcpuCycles;

    if (xcpuCycles > 0) {
        long long xcpuTicks = (long long)((double)xcpuCycles / (s.cpuGhz * 1e6) * (double)s.freq / 1000.0);
        if (xcpuTicks > 0) {
            AccumulateTicks(s, PERF_CAT_XCPU_RUN, xcpuTicks);
        }
    }

    // Print the PREVIOUS frame's accumulated data before resetting.
    // draw/native/tex/etc all happen before D3DDevice_Swap is called,
    // so we must print first, then clear.
    if (s.frameCount > 0) {
        double swapMs  = (double)s.frame[PERF_CAT_SWAP].totalTicks            / (double)s.freq * 1000.0;
        double blitMs  = (double)s.frame[PERF_CAT_BLIT].totalTicks             / (double)s.freq * 1000.0;
        double presMs  = (double)s.frame[PERF_CAT_PRESENT].totalTicks          / (double)s.freq * 1000.0;
        double nativeMs= (double)s.frame[PERF_CAT_UPDATE_NATIVE].totalTicks   / (double)s.freq * 1000.0;
        double texMs   = (double)s.frame[PERF_CAT_UPDATE_TEXTURES].totalTicks / (double)s.freq * 1000.0;
        double psMs    = (double)s.frame[PERF_CAT_UPDATE_PS].totalTicks       / (double)s.freq * 1000.0;
        double vsMs    = (double)s.frame[PERF_CAT_UPDATE_VS].totalTicks       / (double)s.freq * 1000.0;
        double drawMs  = (double)s.frame[PERF_CAT_DRAW].totalTicks            / (double)s.freq * 1000.0;
        double vtxMs   = (double)s.frame[PERF_CAT_VTX_CONVERT].totalTicks    / (double)s.freq * 1000.0;
        double rsrcMs  = (double)s.frame[PERF_CAT_CREATE_RESOURCE].totalTicks / (double)s.freq * 1000.0;
        double cmpMs   = (double)s.frame[PERF_CAT_SHADER_COMPILE].totalTicks  / (double)s.freq * 1000.0;
        double gbMs    = (double)s.frame[PERF_CAT_GET_BACKBUFFER].totalTicks  / (double)s.freq * 1000.0;
        double sleepMs = (double)s.frame[PERF_CAT_FRAME_SLEEP].totalTicks     / (double)s.freq * 1000.0;
        double endMs   = (double)s.frame[PERF_CAT_ENDSCENE].totalTicks        / (double)s.freq * 1000.0;
        long long nativeCalls = s.frame[PERF_CAT_UPDATE_NATIVE].calls;
        long long rsrcCalls   = s.frame[PERF_CAT_CREATE_RESOURCE].calls;
        ULONG vtxHits = g_VtxCacheHits;
        ULONG vtxMisses = g_VtxCacheMisses;
        int topIdx[PERF_REPORT_TOP_XBOX_THREADS];
        int topWaitIdx[PERF_REPORT_TOP_XBOX_THREADS];
        int topDelayIdx[PERF_REPORT_TOP_XBOX_THREADS];
        int topKickIdx[PERF_REPORT_TOP_VS_KICKDRAIN_KEYS];
        double keWaitMs = (double)s.frame[PERF_CAT_KE_WAIT].totalTicks / (double)s.freq * 1000.0;
        double keDelayMs = (double)s.frame[PERF_CAT_KE_DELAY].totalTicks / (double)s.freq * 1000.0;

        for (int i = 0; i < PERF_MAX_VS_KICKDRAIN_KEYS; i++) {
            if (s.vsKickDrainFrameTicks[i] > s.vsKickDrainWindowMaxFrameTicks[i]) {
                s.vsKickDrainWindowMaxFrameTicks[i] = s.vsKickDrainFrameTicks[i];
            }
        }

        for (int i = 0; i < nthr; i++) {
            if (s.xboxThreadKeWaitFrameTicks[i] > s.xboxThreadKeWaitWindowMaxFrameTicks[i]) {
                s.xboxThreadKeWaitWindowMaxFrameTicks[i] = s.xboxThreadKeWaitFrameTicks[i];
            }
            if (s.xboxThreadKeDelayFrameTicks[i] > s.xboxThreadKeDelayWindowMaxFrameTicks[i]) {
                s.xboxThreadKeDelayWindowMaxFrameTicks[i] = s.xboxThreadKeDelayFrameTicks[i];
            }
            if (s.xboxThreadDelayPollFrameCounts[i] > s.xboxThreadDelayPollWindowMaxFrameCounts[i]) {
                s.xboxThreadDelayPollWindowMaxFrameCounts[i] = s.xboxThreadDelayPollFrameCounts[i];
            }
            if (s.xboxThreadWaitPollFrameCounts[i] > s.xboxThreadWaitPollWindowMaxFrameCounts[i]) {
                s.xboxThreadWaitPollWindowMaxFrameCounts[i] = s.xboxThreadWaitPollFrameCounts[i];
            }
        }

        FindTopXboxThreadIndices(s.xcpuThreadMs, nthr, topIdx);
        FindTopXboxThreadTickIndices(s.xboxThreadKeWaitFrameTicks, nthr, topWaitIdx);
        FindTopXboxThreadTickIndices(s.xboxThreadKeDelayFrameTicks, nthr, topDelayIdx);
        FindTopKickDrainIndices(s.vsKickDrainFrameTicks, PERF_MAX_VS_KICKDRAIN_KEYS, topKickIdx);

        fprintf(s.logFile,
            "F%06llu  ftime=%7.3f  fps=%5.1f(~%5.1f)  xcpu=%7.3f(x%d)  swap=%6.2f  blit=%6.2f  pres=%6.2f  end=%6.2f  sleep=%6.2f  gbuf=%6.2f  native=%6.2f(x%lld)  tex=%6.2f  ps=%6.2f  vs=%6.2f  draw=%6.2f  vtx=%6.2f(%lu/%lu)  rsrc=%6.2f(x%lld)  cmp=%6.2f  rs=%6.2f  ts=%6.2f  vd=%6.2f[dc=%5.2f co=%5.2f bs=%5.2f kd=%5.2f bu=%5.2f up=%5.2f vp=%5.2f]  kewait=%6.2f(x%lld)  kedelay=%6.2f(x%lld)  rkw=%6.2f(x%lld)  rkd=%6.2f(x%lld) ms\n",
            (unsigned long long)s.frameCount,
            frameDeltaMs, instantFps, s.smoothFps,
            s.xcpuMs, nthr,
            swapMs, blitMs, presMs, endMs, sleepMs, gbMs, nativeMs, nativeCalls,
            texMs, psMs, vsMs, drawMs, vtxMs, vtxHits, vtxMisses, rsrcMs, rsrcCalls, cmpMs,
            (double)s.frame[PERF_CAT_RENDER_STATES].totalTicks / (double)s.freq * 1000.0,
            (double)s.frame[PERF_CAT_TEX_STATES].totalTicks / (double)s.freq * 1000.0,
            (double)s.frame[PERF_CAT_VTX_DECL].totalTicks / (double)s.freq * 1000.0,
            (double)s.frame[PERF_CAT_VS_DECL].totalTicks / (double)s.freq * 1000.0,
            (double)s.frame[PERF_CAT_VS_CONST].totalTicks / (double)s.freq * 1000.0,
            (double)s.frame[PERF_CAT_VS_BONE_SYNC].totalTicks / (double)s.freq * 1000.0,
            (double)s.frame[PERF_CAT_VS_KICKDRAIN].totalTicks / (double)s.freq * 1000.0,
            (double)s.frame[PERF_CAT_VS_BONE_UPLOAD].totalTicks / (double)s.freq * 1000.0,
            (double)s.frame[PERF_CAT_VS_UPLOAD].totalTicks / (double)s.freq * 1000.0,
            (double)s.frame[PERF_CAT_VIEWPORT].totalTicks / (double)s.freq * 1000.0,
            (double)s.frame[PERF_CAT_KE_WAIT].totalTicks / (double)s.freq * 1000.0,
            s.frame[PERF_CAT_KE_WAIT].calls,
            (double)s.frame[PERF_CAT_KE_DELAY].totalTicks / (double)s.freq * 1000.0,
            s.frame[PERF_CAT_KE_DELAY].calls,
            (double)s.frameRt[0].totalTicks / (double)s.freq * 1000.0,
            s.frameRt[0].calls,
            (double)s.frameRt[1].totalTicks / (double)s.freq * 1000.0,
            s.frameRt[1].calls);

        if ((frameDeltaMs >= 20.0 || s.xcpuMs >= 10.0) && topIdx[0] >= 0) {
            fprintf(
                s.logFile,
                "         xthr0=%5.2f[%s]",
                s.xcpuThreadMs[topIdx[0]],
                s.xboxThreadLabels[topIdx[0]]);

            if (topIdx[1] >= 0) {
                fprintf(
                    s.logFile,
                    "  xthr1=%5.2f[%s]",
                    s.xcpuThreadMs[topIdx[1]],
                    s.xboxThreadLabels[topIdx[1]]);
            }
            if (topIdx[2] >= 0) {
                fprintf(
                    s.logFile,
                    "  xthr2=%5.2f[%s]",
                    s.xcpuThreadMs[topIdx[2]],
                    s.xboxThreadLabels[topIdx[2]]);
            }
            fprintf(s.logFile, "\n");
        }

        if ((frameDeltaMs >= 20.0 || s.xcpuMs >= 10.0) && topKickIdx[0] >= 0) {
            fprintf(
                s.logFile,
                "         xkd0=%5.2f[key=%016llX h=%08X c=%016llX]",
                (double)s.vsKickDrainFrameTicks[topKickIdx[0]] / (double)s.freq * 1000.0,
                (unsigned long long)s.vsKickDrainKeys[topKickIdx[0]],
                (unsigned int)s.vsKickDrainHandles[topKickIdx[0]],
				(unsigned long long)s.vsKickDrainCacheHashes[topKickIdx[0]]);

            if (topKickIdx[1] >= 0) {
                fprintf(
                    s.logFile,
                    "  xkd1=%5.2f[key=%016llX h=%08X c=%016llX]",
                    (double)s.vsKickDrainFrameTicks[topKickIdx[1]] / (double)s.freq * 1000.0,
                    (unsigned long long)s.vsKickDrainKeys[topKickIdx[1]],
                    (unsigned int)s.vsKickDrainHandles[topKickIdx[1]],
					(unsigned long long)s.vsKickDrainCacheHashes[topKickIdx[1]]);
            }
            if (topKickIdx[2] >= 0) {
                fprintf(
                    s.logFile,
                    "  xkd2=%5.2f[key=%016llX h=%08X c=%016llX]",
                    (double)s.vsKickDrainFrameTicks[topKickIdx[2]] / (double)s.freq * 1000.0,
                    (unsigned long long)s.vsKickDrainKeys[topKickIdx[2]],
                    (unsigned int)s.vsKickDrainHandles[topKickIdx[2]],
					(unsigned long long)s.vsKickDrainCacheHashes[topKickIdx[2]]);
            }
            fprintf(s.logFile, "\n");
        }

        if ((frameDeltaMs >= 20.0 || keWaitMs >= 40.0) && topWaitIdx[0] >= 0) {
            fprintf(
                s.logFile,
                "         kwt0=%5.2f[%s]",
                (double)s.xboxThreadKeWaitFrameTicks[topWaitIdx[0]] / (double)s.freq * 1000.0,
                s.xboxThreadLabels[topWaitIdx[0]]);

            if (topWaitIdx[1] >= 0) {
                fprintf(
                    s.logFile,
                    "  kwt1=%5.2f[%s]",
                    (double)s.xboxThreadKeWaitFrameTicks[topWaitIdx[1]] / (double)s.freq * 1000.0,
                    s.xboxThreadLabels[topWaitIdx[1]]);
            }
            if (topWaitIdx[2] >= 0) {
                fprintf(
                    s.logFile,
                    "  kwt2=%5.2f[%s]",
                    (double)s.xboxThreadKeWaitFrameTicks[topWaitIdx[2]] / (double)s.freq * 1000.0,
                    s.xboxThreadLabels[topWaitIdx[2]]);
            }
            fprintf(s.logFile, "\n");
        }

        if ((frameDeltaMs >= 20.0 || keDelayMs >= 20.0) && topDelayIdx[0] >= 0) {
            fprintf(
                s.logFile,
                "         kdl0=%5.2f[%s]",
                (double)s.xboxThreadKeDelayFrameTicks[topDelayIdx[0]] / (double)s.freq * 1000.0,
                s.xboxThreadLabels[topDelayIdx[0]]);

            if (topDelayIdx[1] >= 0) {
                fprintf(
                    s.logFile,
                    "  kdl1=%5.2f[%s]",
                    (double)s.xboxThreadKeDelayFrameTicks[topDelayIdx[1]] / (double)s.freq * 1000.0,
                    s.xboxThreadLabels[topDelayIdx[1]]);
            }
            if (topDelayIdx[2] >= 0) {
                fprintf(
                    s.logFile,
                    "  kdl2=%5.2f[%s]",
                    (double)s.xboxThreadKeDelayFrameTicks[topDelayIdx[2]] / (double)s.freq * 1000.0,
                    s.xboxThreadLabels[topDelayIdx[2]]);
            }
            fprintf(s.logFile, "\n");
        }
    }

    // Reset per-frame accumulators for the new frame.
    memset(s.frame, 0, sizeof(s.frame));
    memset(s.frameRt, 0, sizeof(s.frameRt));
    memset(s.vsKickDrainFrameTicks, 0, sizeof(s.vsKickDrainFrameTicks));
    memset(s.xboxThreadKeWaitFrameTicks, 0, sizeof(s.xboxThreadKeWaitFrameTicks));
    memset(s.xboxThreadKeDelayFrameTicks, 0, sizeof(s.xboxThreadKeDelayFrameTicks));
    memset(s.xboxThreadDelayPollFrameCounts, 0, sizeof(s.xboxThreadDelayPollFrameCounts));
    memset(s.xboxThreadWaitPollFrameCounts, 0, sizeof(s.xboxThreadWaitPollFrameCounts));
    g_VtxCacheHits = 0;
    g_VtxCacheMisses = 0;

    // Record the swap start time so OnSwapEnd can measure total swap duration.
    // Reuse nowTick already captured above to avoid an extra QPC call.
    s.swapStartTick = nowTick;
}

void Report()
{
    auto& s = g_state;
    if (!s.logFile) return;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double windowSec = (double)(now.QuadPart - s.windowStartTick) / (double)s.freq;
    double framesInWindow = (double)s.windowFrameCount;

    fprintf(s.logFile, "# === frame %-6llu  window=%.3fs  (%.1f fps avg) ===\n",
        (unsigned long long)s.frameCount,
        windowSec,
        framesInWindow / windowSec);

    for (int i = 0; i < PERF_CAT_COUNT; i++) {
        auto& c = s.cats[i];
        if (c.calls == 0) continue;
        double totalMs  = (double)c.totalTicks / (double)s.freq * 1000.0;
        double avgUs    = (double)c.totalTicks / (double)c.calls / (double)s.freq * 1000000.0;
        double maxMs    = (double)c.maxTicks   / (double)s.freq * 1000.0;
        double callsPerFrame = (double)c.calls / framesInWindow;
        fprintf(s.logFile,
            "  %s  calls/frame=%6.1f  total=%8.2f ms  avg=%7.2f us  max=%7.3f ms\n",
            g_catNames[i], callsPerFrame, totalMs, avgUs, maxMs);
    }

    int topIdx[PERF_REPORT_TOP_XBOX_THREADS];
    int nthr = s.xboxThreadCount;
    FindTopXboxThreadIndices(s.xcpuThreadWindowMs, nthr, topIdx);
    for (int slot = 0; slot < PERF_REPORT_TOP_XBOX_THREADS; slot++) {
        int idx = topIdx[slot];
        if (idx < 0) {
            continue;
        }

        fprintf(
            s.logFile,
            "  XcpuThread[%d]    avg/frame=%6.2f ms  max/frame=%6.2f ms  total=%8.2f ms  %s\n",
            slot,
            s.xcpuThreadWindowMs[idx] / framesInWindow,
            s.xcpuThreadMaxFrameMs[idx],
            s.xcpuThreadWindowMs[idx],
            s.xboxThreadLabels[idx]);
    }

    if (s.sampleLockInitialized) {
        EnterCriticalSection(&s.sampleLock);

        for (int slot = 0; slot < PERF_REPORT_TOP_XBOX_THREADS; slot++) {
            int idx = topIdx[slot];
            if (idx < 0 || s.xboxThreadSampleCycles[idx] == 0) {
                continue;
            }

            int topXboxPcIdx[PERF_REPORT_TOP_THREAD_PC_SAMPLES];
            FindTopThreadPcSampleIndices(s.xboxThreadPcSamples[idx], topXboxPcIdx);
            for (int pcSlot = 0; pcSlot < PERF_REPORT_TOP_THREAD_PC_SAMPLES; pcSlot++) {
                int pcIdx = topXboxPcIdx[pcSlot];
                if (pcIdx < 0) {
                    continue;
                }

                const auto& pcSample = s.xboxThreadPcSamples[idx][pcIdx];
                std::string resolvedPc = ResolveSamplePc(s, pcSample.pc);
                fprintf(
                    s.logFile,
                    "    XcpuThreadPc[%d.%d] cpu=%6.2f ms  pct=%5.1f%%  hits=%6u  %s\n",
                    slot,
                    pcSlot,
                    (double)pcSample.cycles / (s.cpuGhz * 1e6),
                    (double)pcSample.cycles * 100.0 / (double)s.xboxThreadSampleCycles[idx],
                    pcSample.hits,
                    resolvedPc.c_str());
            }
        }

        int topSampleThreadIdx[PERF_REPORT_TOP_SAMPLED_THREADS];
        FindTopSampledThreadIndices(s.sampledThreads, topSampleThreadIdx);
        for (int slot = 0; slot < PERF_REPORT_TOP_SAMPLED_THREADS; slot++) {
            int idx = topSampleThreadIdx[slot];
            if (idx < 0) {
                continue;
            }

            fprintf(
                s.logFile,
                "  SampleThread[%d] cpu=%6.2f ms  hits=%6u  %s\n",
                slot,
                (double)s.sampledThreads[idx].cycles / (s.cpuGhz * 1e6),
                s.sampledThreads[idx].hits,
                s.sampledThreads[idx].label);

            int topThreadPcIdx[PERF_REPORT_TOP_THREAD_PC_SAMPLES];
            FindTopThreadPcSampleIndices(s.sampledThreads[idx].pcSamples, topThreadPcIdx);
            for (int pcSlot = 0; pcSlot < PERF_REPORT_TOP_THREAD_PC_SAMPLES; pcSlot++) {
                int pcIdx = topThreadPcIdx[pcSlot];
                if (pcIdx < 0) {
                    continue;
                }

                const auto& pcSample = s.sampledThreads[idx].pcSamples[pcIdx];
                std::string resolvedPc = ResolveSamplePc(s, pcSample.pc);
                fprintf(
                    s.logFile,
                    "    SampleThreadPc[%d.%d] cpu=%6.2f ms  pct=%5.1f%%  hits=%6u  %s\n",
                    slot,
                    pcSlot,
                    (double)pcSample.cycles / (s.cpuGhz * 1e6),
                    (double)pcSample.cycles * 100.0 / (double)s.sampledThreads[idx].cycles,
                    pcSample.hits,
                    resolvedPc.c_str());
            }
        }

        if (s.processPcSampleTotalCycles > 0) {
            int topPcIdx[PERF_REPORT_TOP_PC_SAMPLES];
            FindTopPcSampleIndices(s.processPcSamples, topPcIdx);
            for (int slot = 0; slot < PERF_REPORT_TOP_PC_SAMPLES; slot++) {
                int idx = topPcIdx[slot];
                if (idx < 0) {
                    continue;
                }

                std::string resolvedPc = ResolveSamplePc(s, s.processPcSamples[idx].pc);
                fprintf(
                    s.logFile,
                    "  PcHotspot[%d]   cpu=%6.2f ms  pct=%5.1f%%  hits=%6u  %s\n",
                    slot,
                    (double)s.processPcSamples[idx].cycles / (s.cpuGhz * 1e6),
                    (double)s.processPcSamples[idx].cycles * 100.0 / (double)s.processPcSampleTotalCycles,
                    s.processPcSamples[idx].hits,
                    resolvedPc.c_str());
            }
        }

        for (int i = 0; i < PERF_MAX_SAMPLED_THREADS; i++) {
            s.sampledThreads[i].hits = 0;
            s.sampledThreads[i].cycles = 0;
            memset(s.sampledThreads[i].pcSamples, 0, sizeof(s.sampledThreads[i].pcSamples));
        }
        memset(s.xboxThreadSampleHits, 0, sizeof(s.xboxThreadSampleHits));
        memset(s.xboxThreadSampleCycles, 0, sizeof(s.xboxThreadSampleCycles));
        memset(s.xboxThreadPcSamples, 0, sizeof(s.xboxThreadPcSamples));
        memset(s.processPcSamples, 0, sizeof(s.processPcSamples));
        s.processPcSampleTotal = 0;
        s.processPcSampleTotalCycles = 0;

        LeaveCriticalSection(&s.sampleLock);
    }

    int topWaitIdx[PERF_REPORT_TOP_XBOX_THREADS];
    FindTopXboxThreadTickIndices(s.xboxThreadKeWaitWindowTicks, nthr, topWaitIdx);
    for (int slot = 0; slot < PERF_REPORT_TOP_XBOX_THREADS; slot++) {
        int idx = topWaitIdx[slot];
        if (idx < 0) {
            continue;
        }

        fprintf(
            s.logFile,
            "  KeWaitThread[%d]  avg/frame=%6.2f ms  max/frame=%6.2f ms  total=%8.2f ms  %s\n",
            slot,
            (double)s.xboxThreadKeWaitWindowTicks[idx] / (double)s.freq * 1000.0 / framesInWindow,
            (double)s.xboxThreadKeWaitWindowMaxFrameTicks[idx] / (double)s.freq * 1000.0,
            (double)s.xboxThreadKeWaitWindowTicks[idx] / (double)s.freq * 1000.0,
            s.xboxThreadLabels[idx]);
    }

    int topDelayIdx[PERF_REPORT_TOP_XBOX_THREADS];
    FindTopXboxThreadTickIndices(s.xboxThreadKeDelayWindowTicks, nthr, topDelayIdx);
    for (int slot = 0; slot < PERF_REPORT_TOP_XBOX_THREADS; slot++) {
        int idx = topDelayIdx[slot];
        if (idx < 0) {
            continue;
        }

        fprintf(
            s.logFile,
            "  KeDelayThread[%d] avg/frame=%6.2f ms  max/frame=%6.2f ms  total=%8.2f ms  %s\n",
            slot,
            (double)s.xboxThreadKeDelayWindowTicks[idx] / (double)s.freq * 1000.0 / framesInWindow,
            (double)s.xboxThreadKeDelayWindowMaxFrameTicks[idx] / (double)s.freq * 1000.0,
            (double)s.xboxThreadKeDelayWindowTicks[idx] / (double)s.freq * 1000.0,
            s.xboxThreadLabels[idx]);
    }

    int topDelayPollIdx[PERF_REPORT_TOP_XBOX_THREADS];
    FindTopXboxThreadCountIndices(s.xboxThreadDelayPollWindowCounts, nthr, topDelayPollIdx);
    for (int slot = 0; slot < PERF_REPORT_TOP_XBOX_THREADS; slot++) {
        int idx = topDelayPollIdx[slot];
        if (idx < 0) {
            continue;
        }

        fprintf(
            s.logFile,
            "  DelayPollThread[%d] avg/frame=%6.1f  max/frame=%6u  total=%8u  %s\n",
            slot,
            (double)s.xboxThreadDelayPollWindowCounts[idx] / framesInWindow,
            s.xboxThreadDelayPollWindowMaxFrameCounts[idx],
            s.xboxThreadDelayPollWindowCounts[idx],
            s.xboxThreadLabels[idx]);
    }

    int topWaitPollIdx[PERF_REPORT_TOP_XBOX_THREADS];
    FindTopXboxThreadCountIndices(s.xboxThreadWaitPollWindowCounts, nthr, topWaitPollIdx);
    for (int slot = 0; slot < PERF_REPORT_TOP_XBOX_THREADS; slot++) {
        int idx = topWaitPollIdx[slot];
        if (idx < 0) {
            continue;
        }

        fprintf(
            s.logFile,
            "  WaitPollThread[%d]  avg/frame=%6.1f  max/frame=%6u  total=%8u  %s\n",
            slot,
            (double)s.xboxThreadWaitPollWindowCounts[idx] / framesInWindow,
            s.xboxThreadWaitPollWindowMaxFrameCounts[idx],
            s.xboxThreadWaitPollWindowCounts[idx],
            s.xboxThreadLabels[idx]);
    }

    int topKickIdx[PERF_REPORT_TOP_VS_KICKDRAIN_KEYS];
    FindTopKickDrainIndices(s.vsKickDrainWindowTicks, PERF_MAX_VS_KICKDRAIN_KEYS, topKickIdx);
    for (int slot = 0; slot < PERF_REPORT_TOP_VS_KICKDRAIN_KEYS; slot++) {
        int idx = topKickIdx[slot];
        if (idx < 0) {
            continue;
        }

        fprintf(
            s.logFile,
            "  KickDrain[%d]    avg/frame=%6.2f ms  max/frame=%6.2f ms  total=%8.2f ms  key=%016llX h=%08X c=%016llX  reasons[ur=%u mp=%u nc=%u pc=%u]\n",
            slot,
            (double)s.vsKickDrainWindowTicks[idx] / (double)s.freq * 1000.0 / framesInWindow,
            (double)s.vsKickDrainWindowMaxFrameTicks[idx] / (double)s.freq * 1000.0,
            (double)s.vsKickDrainWindowTicks[idx] / (double)s.freq * 1000.0,
            (unsigned long long)s.vsKickDrainKeys[idx],
            (unsigned int)s.vsKickDrainHandles[idx],
			(unsigned long long)s.vsKickDrainCacheHashes[idx],
            s.vsKickDrainUnreliableCount[idx],
            s.vsKickDrainMorePendingCount[idx],
            s.vsKickDrainNonConstantCount[idx],
            s.vsKickDrainParsedConstantsCount[idx]);
    }

    int topMethodIdx[PERF_REPORT_TOP_VS_KICKDRAIN_METHODS];
    FindTopKickDrainMethodIndices(s.vsKickDrainBlockerMethodCounts, PERF_MAX_VS_KICKDRAIN_METHODS, topMethodIdx);
    for (int slot = 0; slot < PERF_REPORT_TOP_VS_KICKDRAIN_METHODS; slot++) {
        int idx = topMethodIdx[slot];
        if (idx < 0) {
            continue;
        }

        fprintf(
            s.logFile,
            "  KickDrainBlocker[%d]  count=%6u  method=0x%04X\n",
            slot,
            s.vsKickDrainBlockerMethodCounts[idx],
            s.vsKickDrainBlockerMethods[idx]);
    }

    if (s.pfifoPendingDepth > 0 || s.pfifoPendingHighWater > 0 || s.pfifoPendingOverflowWindowCount > 0) {
        fprintf(
            s.logFile,
            "  PFIFOPending    depth=%4u  max=%4u  overflow=%u\n",
            s.pfifoPendingDepth,
            s.pfifoPendingHighWater,
            s.pfifoPendingOverflowWindowCount);
    }

    fprintf(s.logFile, "\n");
    fflush(s.logFile);

    // Reset window accumulators
    memset(s.cats, 0, sizeof(s.cats));
    memset(s.xcpuThreadWindowMs, 0, sizeof(s.xcpuThreadWindowMs));
    memset(s.xcpuThreadMaxFrameMs, 0, sizeof(s.xcpuThreadMaxFrameMs));
    memset(s.xboxThreadKeWaitWindowTicks, 0, sizeof(s.xboxThreadKeWaitWindowTicks));
    memset(s.xboxThreadKeWaitWindowMaxFrameTicks, 0, sizeof(s.xboxThreadKeWaitWindowMaxFrameTicks));
    memset(s.xboxThreadKeDelayWindowTicks, 0, sizeof(s.xboxThreadKeDelayWindowTicks));
    memset(s.xboxThreadKeDelayWindowMaxFrameTicks, 0, sizeof(s.xboxThreadKeDelayWindowMaxFrameTicks));
    memset(s.xboxThreadDelayPollWindowCounts, 0, sizeof(s.xboxThreadDelayPollWindowCounts));
    memset(s.xboxThreadDelayPollWindowMaxFrameCounts, 0, sizeof(s.xboxThreadDelayPollWindowMaxFrameCounts));
    memset(s.xboxThreadWaitPollWindowCounts, 0, sizeof(s.xboxThreadWaitPollWindowCounts));
    memset(s.xboxThreadWaitPollWindowMaxFrameCounts, 0, sizeof(s.xboxThreadWaitPollWindowMaxFrameCounts));
    memset(s.vsKickDrainKeys, 0, sizeof(s.vsKickDrainKeys));
    memset(s.vsKickDrainHandles, 0, sizeof(s.vsKickDrainHandles));
	memset(s.vsKickDrainCacheHashes, 0, sizeof(s.vsKickDrainCacheHashes));
    memset(s.vsKickDrainWindowTicks, 0, sizeof(s.vsKickDrainWindowTicks));
    memset(s.vsKickDrainWindowMaxFrameTicks, 0, sizeof(s.vsKickDrainWindowMaxFrameTicks));
    memset(s.vsKickDrainUnreliableCount, 0, sizeof(s.vsKickDrainUnreliableCount));
    memset(s.vsKickDrainMorePendingCount, 0, sizeof(s.vsKickDrainMorePendingCount));
    memset(s.vsKickDrainNonConstantCount, 0, sizeof(s.vsKickDrainNonConstantCount));
    memset(s.vsKickDrainParsedConstantsCount, 0, sizeof(s.vsKickDrainParsedConstantsCount));
    memset(s.vsKickDrainBlockerMethods, 0, sizeof(s.vsKickDrainBlockerMethods));
    memset(s.vsKickDrainBlockerMethodCounts, 0, sizeof(s.vsKickDrainBlockerMethodCounts));
    s.pfifoPendingHighWater = s.pfifoPendingDepth;
    s.pfifoPendingOverflowWindowCount = 0;
    s.windowStartTick = now.QuadPart;
    s.windowFrameCount = 0;
}

void OnSwapEnd()
{
    auto& s = g_state;
    if (!s.logFile) return;

    // Record total swap time (StretchRect + Present + all swap work).
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (s.swapStartTick != 0) {
        long long elapsed = now.QuadPart - s.swapStartTick;
        AccumulateTicks(s, PERF_CAT_SWAP, elapsed);
    }

    s.frameCount++;
    s.windowFrameCount++;

    double elapsed_s = (double)(now.QuadPart - s.windowStartTick) / (double)s.freq;
    if (elapsed_s >= PERF_REPORT_INTERVAL_S) {
        Report();
    }
}

} // namespace PerfTraceInternal
