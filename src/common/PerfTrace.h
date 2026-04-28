// ******************************************************************
// * PerfTrace.h - Lightweight per-frame performance tracing
// *
// * Usage:
// *   - Wrap a scope with  PERF_SCOPE(PERF_CAT_xxx)  — RAII, zero cost when off.
// *   - At frame end call  PerfTrace_OnSwap()  from EMUPATCH(D3DDevice_Swap).
// *   - Output goes to  <ShaderCacheDir>/../perf_trace.log
// *   - Toggle at runtime: global bool g_PerfTraceEnabled (default: false)
// *     or pass command-line flag  --perf-trace  (wired up in Emu.cpp).
// *
// * The log prints one line per category every PERF_REPORT_INTERVAL_S seconds:
// *   [frame N | Δt ms]  CAT_NAME  calls=N  total=X.XXms  avg=X.XXms  max=X.XXms
// ******************************************************************
#pragma once
#ifndef PERFTRACE_H
#define PERFTRACE_H

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <cstdint>

// ── tunables ──────────────────────────────────────────────────────────────────
// Seconds between log lines per category (reduce for more granular data)
#ifndef PERF_REPORT_INTERVAL_S
#define PERF_REPORT_INTERVAL_S 5.0
#endif
// Maximum number of Xbox threads to track for CPU time accounting
#define PERF_MAX_XBOX_THREADS 32
// Fixed-size label buffer for Xbox thread attribution in perf_trace.log
#define PERF_MAX_XBOX_THREAD_LABEL 96
// Number of hottest Xbox threads to print in suspect-frame and window summaries
#define PERF_REPORT_TOP_XBOX_THREADS 3
// Number of unique vertex shader keys to track for VS_KickDrain attribution
#define PERF_MAX_VS_KICKDRAIN_KEYS 32
// Number of hottest VS_KickDrain shader keys to print in suspect-frame and window summaries
#define PERF_REPORT_TOP_VS_KICKDRAIN_KEYS 3
// Number of PFIFO non-constant methods to track as kick-drain blockers
#define PERF_MAX_VS_KICKDRAIN_METHODS 16
// Number of hottest PFIFO non-constant blocker methods to print in window summaries
#define PERF_REPORT_TOP_VS_KICKDRAIN_METHODS 4
// Number of process threads tracked by the sampling profiler
#define PERF_MAX_SAMPLED_THREADS 64
// Number of hottest sampled threads to print in window summaries
#define PERF_REPORT_TOP_SAMPLED_THREADS 5
// Number of unique sampled PCs to retain per report window
#define PERF_MAX_PC_SAMPLES 64
// Number of hottest sampled PCs to print in window summaries
#define PERF_REPORT_TOP_PC_SAMPLES 5
// Number of unique sampled PCs to retain per sampled thread in a report window
#define PERF_MAX_THREAD_PC_SAMPLES 16
// Number of hottest sampled PCs to print for each top sampled thread
#define PERF_REPORT_TOP_THREAD_PC_SAMPLES 3
// Sampling interval for the process-wide PC sampler
#define PERF_SAMPLER_INTERVAL_MS 2
// ─────────────────────────────────────────────────────────────────────────────

// Category IDs — add new ones here and mirror in PerfTrace.cpp's g_catNames[]
enum PerfCat : int {
    PERF_CAT_SWAP            = 0,  // Full D3DDevice_Swap incl. StretchRect + Present
    PERF_CAT_UPDATE_NATIVE   = 1,  // CxbxUpdateNativeD3DResources (per draw call)
    PERF_CAT_UPDATE_TEXTURES = 2,  // CxbxUpdateHostTextures
    PERF_CAT_UPDATE_VS       = 3,  // CxbxUpdateHostVertexShader + constants
    PERF_CAT_UPDATE_PS       = 4,  // DxbxUpdateActivePixelShader
    PERF_CAT_DRAW            = 5,  // Host DrawIndexedPrimitive / DrawPrimitive
    PERF_CAT_CREATE_RESOURCE = 6,  // CreateHostResource (texture/surface upload)
    PERF_CAT_VTX_CONVERT     = 7,  // Vertex buffer conversion (VertexBufferConverter.Apply)
    PERF_CAT_SHADER_COMPILE  = 8,  // EmuCompileShader (sync miss only)
    PERF_CAT_LOCK_UNLOCK     = 9,  // Lock/Unlock surface or texture
    PERF_CAT_PRESENT         = 10, // Just the IDirect3DDevice9::Present call
    PERF_CAT_BLIT            = 11, // StretchRect blit (Xbox BB -> Host BB)
    PERF_CAT_RENDER_STATES   = 12, // XboxRenderStates.Apply()
    PERF_CAT_TEX_STATES      = 13, // XboxTextureStates.Apply()
    PERF_CAT_VTX_DECL        = 14, // CxbxUpdateHostVertexDeclaration + constants + viewport
    PERF_CAT_VS_DECL         = 15, // CxbxUpdateHostVertexDeclaration only
    PERF_CAT_VS_CONST        = 16, // CxbxUpdateHostVertexShaderConstants only
    PERF_CAT_VIEWPORT        = 17, // CxbxUpdateHostViewport only
    PERF_CAT_GET_BACKBUFFER  = 18, // Host GetBackBuffer call in swap path
    PERF_CAT_FRAME_SLEEP     = 19, // SleepPrecise frame limiter inside Swap
    PERF_CAT_ENDSCENE        = 20, // EndScene + GPU flush before Present
    PERF_CAT_EMU_X86         = 21, // EmuX86_DecodeException VEH handler (privileged instrs)
    PERF_CAT_KE_WAIT         = 22, // KeWaitForSingleObject / KeWaitForMultipleObjects
    PERF_CAT_KE_DELAY        = 23, // KeDelayExecutionThread
    PERF_CAT_VS_KICKDRAIN    = 24, // KickOff + PFIFO drain inside VS_CONST
    PERF_CAT_VS_UPLOAD       = 25, // Snapshot + compare + SetVertexShaderConstantF
    PERF_CAT_XCPU_RUN        = 26, // Xbox CPU emulation (EmuX86_Opcode handlers)
    PERF_CAT_VS_BONE_SYNC    = 27, // Bone-only PFIFO parse + constant overlay inside VS_CONST
    PERF_CAT_VS_BONE_UPLOAD  = 28, // Bone-only constant snapshot + compare + upload work
    PERF_CAT_COUNT           = 29,
};

enum PerfVSKickDrainReason : uint32_t {
    PERF_VS_KICKDRAIN_REASON_UNRELIABLE_PARSE = 1u << 0,
    PERF_VS_KICKDRAIN_REASON_MORE_PENDING     = 1u << 1,
    PERF_VS_KICKDRAIN_REASON_NON_CONSTANT     = 1u << 2,
    PERF_VS_KICKDRAIN_REASON_PARSED_CONSTANTS = 1u << 3,
};

// ── internal state (defined in PerfTrace.h to keep it header-only) ───────────
extern bool g_PerfTraceEnabled;

// Per-frame vtx cache statistics (incremented by ConvertStream, reset each swap)
extern ULONG g_VtxCacheHits;
extern ULONG g_VtxCacheMisses;

namespace PerfTraceInternal {

struct CatStats {
    long long totalTicks;
    long long maxTicks;
    long long calls;
};

struct SampledThread {
    HANDLE handle;
    DWORD nativeThreadId;
    ULONGLONG cycleStart;
    uint32_t hits;
    ULONGLONG cycles;
    char label[PERF_MAX_XBOX_THREAD_LABEL];
    struct ThreadPcSample {
        uintptr_t pc;
        uint32_t hits;
        ULONGLONG cycles;
    } pcSamples[PERF_MAX_THREAD_PC_SAMPLES];
};

struct PcSample {
    uintptr_t pc;
    uint32_t hits;
    ULONGLONG cycles;
};

struct State {
    CatStats cats[PERF_CAT_COUNT];
    long long windowStartTick;
    long long swapStartTick;  // set in OnSwapBegin, read in OnSwapEnd
    long long lastSwapBeginTick; // for computing frame-to-frame time
    long long freq;
    unsigned long long frameCount;
    unsigned long long windowFrameCount;
    double smoothFps;  // EMA-smoothed FPS (alpha=0.1)
    FILE* logFile;
    bool initialized;
    // simple per-frame accumulators reset each Swap
    CatStats frame[PERF_CAT_COUNT];
    // Xbox thread CPU time tracking (QueryThreadCycleTime, TSC-based)
    HANDLE xboxThreads[PERF_MAX_XBOX_THREADS];        // duplicated native Win32 handles
    ULONGLONG xboxThreadCycleStart[PERF_MAX_XBOX_THREADS]; // cumulative TSC cycles at last sample
    DWORD xboxThreadIds[PERF_MAX_XBOX_THREADS];       // Xbox-visible thread IDs / handles
    DWORD xboxNativeThreadIds[PERF_MAX_XBOX_THREADS]; // native Win32 thread IDs
    const void* xboxThreadSystemRoutine[PERF_MAX_XBOX_THREADS];
    const void* xboxThreadStartRoutine[PERF_MAX_XBOX_THREADS];
    char xboxThreadLabels[PERF_MAX_XBOX_THREADS][PERF_MAX_XBOX_THREAD_LABEL];
    ULONGLONG xboxThreadSampleCycleStart[PERF_MAX_XBOX_THREADS];
    uint32_t xboxThreadSampleHits[PERF_MAX_XBOX_THREADS];
    ULONGLONG xboxThreadSampleCycles[PERF_MAX_XBOX_THREADS];
    SampledThread::ThreadPcSample xboxThreadPcSamples[PERF_MAX_XBOX_THREADS][PERF_MAX_THREAD_PC_SAMPLES];
    double xcpuThreadMs[PERF_MAX_XBOX_THREADS];       // current frame CPU time per Xbox thread
    double xcpuThreadWindowMs[PERF_MAX_XBOX_THREADS]; // accumulated CPU time in current report window
    double xcpuThreadMaxFrameMs[PERF_MAX_XBOX_THREADS]; // hottest single-frame CPU time in window
    long long xboxThreadKeWaitFrameTicks[PERF_MAX_XBOX_THREADS];
    long long xboxThreadKeWaitWindowTicks[PERF_MAX_XBOX_THREADS];
    long long xboxThreadKeWaitWindowMaxFrameTicks[PERF_MAX_XBOX_THREADS];
    long long xboxThreadKeDelayFrameTicks[PERF_MAX_XBOX_THREADS];
    long long xboxThreadKeDelayWindowTicks[PERF_MAX_XBOX_THREADS];
    long long xboxThreadKeDelayWindowMaxFrameTicks[PERF_MAX_XBOX_THREADS];
    uint32_t xboxThreadDelayPollFrameCounts[PERF_MAX_XBOX_THREADS];
    uint32_t xboxThreadDelayPollWindowCounts[PERF_MAX_XBOX_THREADS];
    uint32_t xboxThreadDelayPollWindowMaxFrameCounts[PERF_MAX_XBOX_THREADS];
    uint32_t xboxThreadWaitPollFrameCounts[PERF_MAX_XBOX_THREADS];
    uint32_t xboxThreadWaitPollWindowCounts[PERF_MAX_XBOX_THREADS];
    uint32_t xboxThreadWaitPollWindowMaxFrameCounts[PERF_MAX_XBOX_THREADS];
    ULONGLONG xcpuCyclesPerFrame;  // last frame: total Xbox-thread TSC cycles (converted to ms using cpuGhz)
    double cpuGhz;        // measured CPU frequency (GHz) used to convert TSC cycles -> ms
    int xboxThreadCount;  // number of registered Xbox threads (written atomically)
    double xcpuMs;        // last frame: total Xbox-thread CPU time in ms
    uint64_t vsKickDrainKeys[PERF_MAX_VS_KICKDRAIN_KEYS];
    uint32_t vsKickDrainHandles[PERF_MAX_VS_KICKDRAIN_KEYS];
    uint64_t vsKickDrainCacheHashes[PERF_MAX_VS_KICKDRAIN_KEYS];
    long long vsKickDrainFrameTicks[PERF_MAX_VS_KICKDRAIN_KEYS];
    long long vsKickDrainWindowTicks[PERF_MAX_VS_KICKDRAIN_KEYS];
    long long vsKickDrainWindowMaxFrameTicks[PERF_MAX_VS_KICKDRAIN_KEYS];
    uint32_t vsKickDrainUnreliableCount[PERF_MAX_VS_KICKDRAIN_KEYS];
    uint32_t vsKickDrainMorePendingCount[PERF_MAX_VS_KICKDRAIN_KEYS];
    uint32_t vsKickDrainNonConstantCount[PERF_MAX_VS_KICKDRAIN_KEYS];
    uint32_t vsKickDrainParsedConstantsCount[PERF_MAX_VS_KICKDRAIN_KEYS];
    uint32_t vsKickDrainBlockerMethods[PERF_MAX_VS_KICKDRAIN_METHODS];
    uint32_t vsKickDrainBlockerMethodCounts[PERF_MAX_VS_KICKDRAIN_METHODS];
    uint32_t pfifoPendingDepth;
    uint32_t pfifoPendingHighWater;
    uint32_t pfifoPendingOverflowLastSeen;
    uint32_t pfifoPendingOverflowWindowCount;
    // Render thread tracking: host thread ID of the thread that calls D3DDevice_Swap
    DWORD renderThreadId; // set each frame in OnSwapBegin from GetCurrentThreadId()
    // Per-frame render-thread exclusive stats for KE_WAIT and KE_DELAY
    CatStats frameRt[2]; // [0]=KE_WAIT, [1]=KE_DELAY, render thread only
    SampledThread sampledThreads[PERF_MAX_SAMPLED_THREADS];
    PcSample processPcSamples[PERF_MAX_PC_SAMPLES];
    uint32_t processPcSampleTotal;
    ULONGLONG processPcSampleTotalCycles;
    HANDLE samplerThread;
    DWORD samplerThreadId;
    long long sampleRefreshTick;
    CRITICAL_SECTION sampleLock;
    bool sampleLockInitialized;
    bool symbolsReady;
};

inline void AccumulateTicks(State& s, PerfCat cat, long long elapsed)
{
    s.frame[cat].totalTicks += elapsed;
    s.frame[cat].calls++;
    if (elapsed > s.frame[cat].maxTicks) s.frame[cat].maxTicks = elapsed;

    s.cats[cat].totalTicks += elapsed;
    s.cats[cat].calls++;
    if (elapsed > s.cats[cat].maxTicks) s.cats[cat].maxTicks = elapsed;
}

extern State g_state;
extern const char* g_catNames[PERF_CAT_COUNT];

void Init(const char* logPath);
void RegisterXboxThread(HANDLE h, DWORD xboxThreadId, DWORD nativeThreadId, const void* systemRoutine, const void* startRoutine); // duplicate h and track its CPU time per-frame
void RecordVSKickDrain(uint64_t shaderKey, uint32_t shaderHandle, uint64_t shaderCacheHash, long long elapsedTicks, uint32_t reasonMask);
void RecordVSKickDrainBlockerMethod(uint32_t method);
void RecordPFIFOPending(uint32_t currentDepth, uint32_t highWater, uint32_t overflowCount);
void RecordDelayPoll();
void RecordWaitPoll();
void Report();          // called by PerfTrace_OnSwap every N seconds
void OnSwapBegin();     // resets per-frame accumulators, bumps frame counter
void OnSwapEnd();       // accumulates frame totals into window totals, maybe Report

} // namespace PerfTraceInternal

// ── RAII scope guard ──────────────────────────────────────────────────────────
struct PerfScopeGuard {
    LARGE_INTEGER start;
    PerfCat cat;

    inline PerfScopeGuard(PerfCat c) : cat(c) {
        if (g_PerfTraceEnabled)
            QueryPerformanceCounter(&start);
    }
    inline ~PerfScopeGuard() {
        if (!g_PerfTraceEnabled) return;
        LARGE_INTEGER end;
        QueryPerformanceCounter(&end);
        long long elapsed = end.QuadPart - start.QuadPart;

        auto& s = PerfTraceInternal::g_state;
        PerfTraceInternal::AccumulateTicks(s, cat, elapsed);
        if (cat == PERF_CAT_KE_WAIT || cat == PERF_CAT_KE_DELAY) {
            DWORD currentThreadId = GetCurrentThreadId();

            for (int i = 0; i < s.xboxThreadCount; i++) {
                if (s.xboxNativeThreadIds[i] != currentThreadId) {
                    continue;
                }

                if (cat == PERF_CAT_KE_WAIT) {
                    s.xboxThreadKeWaitFrameTicks[i] += elapsed;
                    s.xboxThreadKeWaitWindowTicks[i] += elapsed;
                } else {
                    s.xboxThreadKeDelayFrameTicks[i] += elapsed;
                    s.xboxThreadKeDelayWindowTicks[i] += elapsed;
                }
                break;
            }

            // render-thread exclusive tracking for kewait/kedelay
            if (s.renderThreadId != 0 && currentThreadId == s.renderThreadId) {
                int rtIdx = (cat == PERF_CAT_KE_WAIT) ? 0 : 1;
                s.frameRt[rtIdx].totalTicks += elapsed;
                s.frameRt[rtIdx].calls++;
            }
        }
    }
};

// PERF_SCOPE(cat) — use at the top of a scope/function.
// Compiles to nothing when tracing is disabled (the inline guard checks the bool).
#define PERF_SCOPE(cat) PerfScopeGuard _perf_guard_##cat(cat)

// ── public API ────────────────────────────────────────────────────────────────

// Call once during emulator init (before any draw calls).
// logPath = full path for the output file.
inline void PerfTrace_Init(const char* logPath) {
    PerfTraceInternal::Init(logPath);
}

// Call at the START of EMUPATCH(D3DDevice_Swap) (before PERF_SCOPE(PERF_CAT_SWAP)).
inline void PerfTrace_OnSwapBegin() {
    if (g_PerfTraceEnabled) PerfTraceInternal::OnSwapBegin();
}

// Call at the END of EMUPATCH(D3DDevice_Swap).
inline void PerfTrace_OnSwapEnd() {
    if (g_PerfTraceEnabled) PerfTraceInternal::OnSwapEnd();
}

// Register a native Win32 thread handle to have its CPU time tracked per-frame.
// Safe to call from any thread; a duplicate of h is stored internally.
// Typically called once per Xbox thread from PsCreateSystemThreadEx.
inline void PerfTrace_RegisterXboxThread(HANDLE h, DWORD xboxThreadId, DWORD nativeThreadId, const void* systemRoutine, const void* startRoutine) {
    if (g_PerfTraceEnabled) PerfTraceInternal::RegisterXboxThread(h, xboxThreadId, nativeThreadId, systemRoutine, startRoutine);
}

inline void PerfTrace_RecordVSKickDrain(uint64_t shaderKey, uint32_t shaderHandle, uint64_t shaderCacheHash, long long elapsedTicks, uint32_t reasonMask) {
    if (g_PerfTraceEnabled) PerfTraceInternal::RecordVSKickDrain(shaderKey, shaderHandle, shaderCacheHash, elapsedTicks, reasonMask);
}

inline void PerfTrace_RecordVSKickDrainBlockerMethod(uint32_t method) {
    if (g_PerfTraceEnabled) PerfTraceInternal::RecordVSKickDrainBlockerMethod(method);
}

inline void PerfTrace_RecordPFIFOPending(uint32_t currentDepth, uint32_t highWater, uint32_t overflowCount) {
    if (g_PerfTraceEnabled) PerfTraceInternal::RecordPFIFOPending(currentDepth, highWater, overflowCount);
}

inline void PerfTrace_RecordDelayPoll() {
    if (g_PerfTraceEnabled) PerfTraceInternal::RecordDelayPoll();
}

inline void PerfTrace_RecordWaitPoll() {
    if (g_PerfTraceEnabled) PerfTraceInternal::RecordWaitPoll();
}

#endif // PERFTRACE_H
