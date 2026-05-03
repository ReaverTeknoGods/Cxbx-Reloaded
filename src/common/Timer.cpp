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
// *  (c) 2018      ergo720
// *
// *  All rights reserved
// *
// ******************************************************************

#include <core\kernel\exports\xboxkrnl.h>

#include <windows.h>
#include <chrono>
#include <thread>
#include <vector>
#include <mutex>
#include <array>
#include "Timer.h"
#include "common\util\CxbxUtil.h"
#include "core\kernel\support\EmuFS.h"
#include "core\kernel\exports\EmuKrnlPs.hpp"
#include "core\kernel\exports\EmuKrnl.h"
#include "common/BetaConfig.h"
#include "common/win32/Threads.h"
#include "devices\Xbox.h"
#include "devices\usb\OHCI.h"
#include "core\hle\DSOUND\DirectSound\DirectSoundGlobal.hpp"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

static std::atomic_uint64_t last_qpc; // last time when QPC was called
static std::atomic_uint64_t exec_time; // total execution time in us since the emulation started
static std::atomic_flag get_now_lock = ATOMIC_FLAG_INIT;
static uint64_t pit_last; // last time when the pit time was updated
static uint64_t pit_last_qpc; // last QPC time of the pit
// The frequency of the high resolution clock of the host, and the start time
int64_t HostQPCFrequency, HostQPCStartTime;

static HANDLE get_precise_sleep_timer()
{
	thread_local HANDLE precise_sleep_timer = []() -> HANDLE {
		HANDLE timer = CreateWaitableTimerExW(
			nullptr,
			nullptr,
			CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
			TIMER_MODIFY_STATE | SYNCHRONIZE);
		if (timer == nullptr) {
			timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
		}
		return timer;
	}();

	return precise_sleep_timer;
}

void SleepPassive(std::chrono::microseconds duration)
{
	if (duration <= std::chrono::microseconds::zero()) {
		return;
	}

	auto timer = get_precise_sleep_timer();
	if (timer != nullptr) {
		auto sleep100ns = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count() / 100;
		if (sleep100ns > 0) {
			LARGE_INTEGER dueTime;
			dueTime.QuadPart = -sleep100ns;
			if (SetWaitableTimer(timer, &dueTime, 0, nullptr, nullptr, FALSE)) {
				WaitForSingleObject(timer, INFINITE);
				return;
			}
		}
	}

	auto sleepMs = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
	if (sleepMs > 0) {
		Sleep((DWORD)sleepMs);
	}
	else {
		std::this_thread::yield();
	}
}


void timer_init()
{
	QueryPerformanceFrequency(reinterpret_cast<LARGE_INTEGER *>(&HostQPCFrequency));
	QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER *>(&HostQPCStartTime));
	pit_last_qpc = last_qpc = HostQPCStartTime;
	pit_last = get_now();

	// Synchronize xbox system time with host time
	LARGE_INTEGER HostSystemTime;
	GetSystemTimeAsFileTime((LPFILETIME)&HostSystemTime);
	xbox::KeSystemTime.High2Time = HostSystemTime.u.HighPart;
	xbox::KeSystemTime.LowPart = HostSystemTime.u.LowPart;
	xbox::KeSystemTime.High1Time = HostSystemTime.u.HighPart;
}

// More precise sleep, but with increased CPU usage
void SleepPrecise(std::chrono::steady_clock::time_point targetTime)
{
	using namespace std::chrono;
	auto now = steady_clock::now();
	if (now >= targetTime) {
		return;
	}

	// Sleep with a waitable timer first, then only busy-wait for a short tail.
	// This keeps frame pacing and KeDelayExecutionThread accurate without burning
	// a fixed 2 ms of CPU every time we "sleep".
	constexpr auto spinThreshold = 500us;
	if (g_BetaConfig.precise_sleep_timer) {
		auto timer = get_precise_sleep_timer();
		if (timer != nullptr) {
			auto sleepFor = targetTime - now - spinThreshold;
			if (sleepFor > steady_clock::duration::zero()) {
				auto sleep100ns = duration_cast<nanoseconds>(sleepFor).count() / 100;
				if (sleep100ns > 0) {
					LARGE_INTEGER dueTime;
					dueTime.QuadPart = -sleep100ns;
					if (SetWaitableTimer(timer, &dueTime, 0, nullptr, nullptr, FALSE)) {
						WaitForSingleObject(timer, INFINITE);
					}
				}
			}
		} else {
			auto sleepFor = (targetTime - spinThreshold) - now;
			auto sleepMs = duration_cast<milliseconds>(sleepFor).count();
			if (sleepMs > 0) {
				Sleep((DWORD)sleepMs);
			}
		}
	} else {
		// Legacy path: plain Sleep
		auto sleepFor = (targetTime - spinThreshold) - now;
		auto sleepMs = duration_cast<milliseconds>(sleepFor).count();
		if (sleepMs > 0) {
			Sleep((DWORD)sleepMs);
		}
	}

	while (steady_clock::now() < targetTime) {
		YieldProcessor();
	}
}

// NOTE: the pit device is not implemented right now, so we put this here
static uint64_t pit_next(uint64_t now)
{
	constexpr uint64_t pit_period = 1000;
	uint64_t next = pit_last + pit_period;

	if (now >= next) {
		xbox::KiClockIsr(now - pit_last);
		pit_last = get_now();
		return pit_period;
	}

	return pit_last + pit_period - now; // time remaining until next clock interrupt
}

static void update_non_periodic_events()
{
	// update dsound
	dsound_worker();

	// Periodically assert IRQ10 for Chihiro media board communication.
	if (g_BetaConfig.periodic_irq10 && EmuInterruptList[10] && EmuInterruptList[10]->Connected) {
		static uint64_t irq10_last = 0;
		uint64_t now_qpc;
		QueryPerformanceCounter((LARGE_INTEGER*)&now_qpc);
		if (irq10_last == 0 || (now_qpc - irq10_last) * 1000 / HostQPCFrequency >= 16) {
			irq10_last = now_qpc;
			HalSystemInterrupts[10].Assert(false);
			HalSystemInterrupts[10].Assert(true);
		}
	}

	// check for hw interrupts
	for (int i = 0; i < MAX_BUS_INTERRUPT_LEVEL; i++) {
		// If the interrupt is pending and connected, process it
		if (g_bEnableAllInterrupts && HalSystemInterrupts[i].IsPending() && EmuInterruptList[i] && EmuInterruptList[i]->Connected) {
			HalSystemInterrupts[i].Trigger(EmuInterruptList[i]);
		}
	}
}

uint64_t get_now()
{
	if (g_BetaConfig.get_now_lock) {
		// Spinlock to prevent concurrent callers from double-counting elapsed time.
		// Almost always uncontended (system_events is the primary caller).
		while (get_now_lock.test_and_set(std::memory_order_acquire)) {
			YieldProcessor();
		}

		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);

		uint64_t prev = last_qpc.load(std::memory_order_relaxed);
		last_qpc.store(now.QuadPart, std::memory_order_relaxed);
		uint64_t elapsed_us = now.QuadPart - prev;
		elapsed_us *= 1000000;
		elapsed_us /= HostQPCFrequency;
		uint64_t result = exec_time.fetch_add(elapsed_us, std::memory_order_relaxed) + elapsed_us;

		get_now_lock.clear(std::memory_order_release);
		return result;
	} else {
		// Original unlocked path
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		uint64_t elapsed_us = now.QuadPart - last_qpc;
		last_qpc = now.QuadPart;
		elapsed_us *= 1000000;
		elapsed_us /= HostQPCFrequency;
		exec_time += elapsed_us;
		return exec_time;
	}
}

static uint64_t get_next(uint64_t now)
{
	std::array<uint64_t, 5> next = {
		pit_next(now),
		g_NV2A->vblank_next(now),
		g_NV2A->ptimer_next(now),
		g_USB0->m_HostController->OHCI_next(now),
		dsound_next(now)
	};
	return *std::min_element(next.begin(), next.end());
}

xbox::void_xt NTAPI system_events(xbox::PVOID arg)
{
	constexpr uint64_t system_event_sleep_quantum_us = 500;
	constexpr uint64_t system_event_spin_window_us = 250;

	// Optionally move system_events off the Xbox core to reduce contention
	if (g_BetaConfig.system_events_other_affinity && g_AffinityPolicy) {
		g_AffinityPolicy->SetAffinityOther();
	}

	// Testing shows that, if this thread has the same priority of the other xbox threads, it can take tens, even hundreds of ms to complete a single loop.
	// So we increase its priority to above normal, so that it scheduled more often
	if (g_BetaConfig.system_events_normal_priority) {
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
	} else {
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
	}

	// Always run this thread at dpc level to prevent it from ever executing APCs/DPCs
	xbox::KeRaiseIrqlToDpcLevel();

	while (true) {
		const uint64_t last_time = get_now();
		const uint64_t nearest_next = get_next(last_time);

		while (true) {
			update_non_periodic_events();
			uint64_t elapsed_us = get_now() - last_time;
			if (elapsed_us >= nearest_next) {
				break;
			}

			uint64_t remaining_us = nearest_next - elapsed_us;
			if (remaining_us > system_event_spin_window_us) {
				const uint64_t sleep_us = std::min(remaining_us - system_event_spin_window_us, system_event_sleep_quantum_us);
				SleepPassive(std::chrono::microseconds(sleep_us));
				continue;
			}

			std::this_thread::yield();
		}
	}
}

int64_t Timer_GetScaledPerformanceCounter(int64_t Period)
{
	LARGE_INTEGER currentQPC;
	QueryPerformanceCounter(&currentQPC);

	// Scale frequency with overflow avoidance, like in std::chrono
	// https://github.com/microsoft/STL/blob/6d2f8b0ed88ea6cba26cc2151f47f678442c1663/stl/inc/chrono#L703
	const int64_t currentTime = currentQPC.QuadPart - HostQPCStartTime;
	const int64_t whole = (currentTime / HostQPCFrequency) * Period;
	const int64_t part  = (currentTime % HostQPCFrequency) * Period / HostQPCFrequency;

	return whole + part;
}

