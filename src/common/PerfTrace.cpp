// ******************************************************************
// * PerfTrace.cpp - Implementation for lightweight perf tracing
// ******************************************************************
#include "PerfTrace.h"
#include <cstdarg>
#include <cstring>

bool g_PerfTraceEnabled = true;

ULONG g_VtxCacheHits = 0;
ULONG g_VtxCacheMisses = 0;

namespace PerfTraceInternal {

State g_state = {};

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
                    THREAD_QUERY_INFORMATION, FALSE, 0);
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
    _snprintf_s(
        s.xboxThreadLabels[idx],
        PERF_MAX_XBOX_THREAD_LABEL,
        _TRUNCATE,
        "xb=%04X nt=%04X sr=%p sys=%p",
        (unsigned int)(xboxThreadId & 0xFFFFu),
        (unsigned int)(nativeThreadId & 0xFFFFu),
        startRoutine,
        systemRoutine);
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

void Init(const char* logPath)
{
    auto& s = g_state;
    if (s.initialized) return;
    memset(&s, 0, sizeof(s));

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
            "#\n",
            (double)PERF_REPORT_INTERVAL_S);
        fflush(s.logFile);
        fprintf(stdout, "[PerfTrace] logging to %s\n", logPath);
    } else {
        fprintf(stderr, "[PerfTrace] ERROR: cannot open %s\n", logPath);
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
