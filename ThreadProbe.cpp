// Stealth Senses — SKSE plugin for Skyrim SE/AE
// Copyright (C) 2026 fozar
//
// This program is free software: you can redistribute it and/or modify it under the terms of
// the GNU General Public License as published by the Free Software Foundation, either version 3
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
// without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
// See the GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along with this program.
// If not, see <https://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ThreadProbe.h"

#include "Config.h"

namespace StealthSenses::ThreadProbe {
    namespace {
        std::atomic<DWORD>         g_mainThread{ 0 };
        std::atomic<const char*>   g_owner{ nullptr };  // section running now
        std::atomic<DWORD>         g_ownerThread{ 0 };
        std::atomic<std::uint32_t> g_overlaps{ 0 };
        Counter                    g_sections;
    }

    bool Enabled() {
        return Config::Get().debug.thread_probe;
    }

    void SetMainThread() {
        g_mainThread = GetCurrentThreadId();
    }

    DWORD MainThread() {
        return g_mainThread.load();
    }

    Counter::Slot* Counter::Claim(DWORD a_thread) {
        for (auto& slot : _slots) {
            DWORD owner = slot.thread.load();
            if (owner == a_thread || (owner == 0 && slot.thread.compare_exchange_strong(owner, a_thread))) {
                return &slot;
            }
        }
        return nullptr;
    }

    void Counter::Hit(bool a_flagged) {
        // One slot lookup per thread and counter; the counters are statics, so the cache can be too
        thread_local std::unordered_map<const Counter*, Slot*> cache;
        auto [it, fresh] = cache.try_emplace(this, nullptr);
        if (fresh) {
            it->second = Claim(GetCurrentThreadId());
        }
        if (!it->second) {
            _unslotted.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        it->second->calls.fetch_add(1, std::memory_order_relaxed);
        if (a_flagged) {
            it->second->flagged.fetch_add(1, std::memory_order_relaxed);
        }
    }

    std::string Counter::Drain(std::uint32_t& a_total, std::uint32_t& a_flagged) {
        a_total   = 0;
        a_flagged = 0;
        std::string threads;
        const DWORD main = MainThread();
        for (auto& slot : _slots) {
            const DWORD id = slot.thread.load();
            if (id == 0) {
                break;  // slots are claimed in order
            }
            const auto calls   = slot.calls.exchange(0);
            const auto flagged = slot.flagged.exchange(0);
            if (calls == 0) {
                continue;
            }
            a_total += calls;
            a_flagged += flagged;
            threads += std::format(" {}{}={}", id, id == main ? "(main)" : "", calls);
        }
        if (const auto unslotted = _unslotted.exchange(0)) {
            threads += std::format(" +{} unslotted", unslotted);
        }
        return threads.empty() ? " none" : threads;
    }

    Section::Section(const char* a_name) {
        if (!Enabled()) {
            return;
        }
        const DWORD self = GetCurrentThreadId();
        g_sections.Hit();
        const char* idle = nullptr;
        if (g_owner.compare_exchange_strong(idle, a_name)) {
            g_ownerThread = self;
            _owner        = true;
        } else if (g_ownerThread.load() != self) {  // same thread = nested call, not a race
            g_overlaps.fetch_add(1, std::memory_order_relaxed);
            SKSE::log::warn("Thread probe: '{}' on thread {} entered while '{}' runs on thread {}", a_name, self,
                idle ? idle : "?", g_ownerThread.load());
        }
    }

    Section::~Section() {
        if (_owner) {
            g_ownerThread = 0;
            g_owner       = nullptr;
        }
    }

    void Report() {
        if (!Enabled()) {
            return;
        }
        using Clock = std::chrono::steady_clock;
        static auto last = Clock::now();
        const auto  now  = Clock::now();
        const auto  span = std::chrono::duration<float>(now - last).count();
        if (span < 5.0f) {
            return;
        }
        last = now;

        std::uint32_t total = 0, unused = 0;
        const auto    threads = g_sections.Drain(total, unused);
        SKSE::log::info("Thread probe, {:.1f} s: {} sections, {} overlaps; threads:{}", span, total,
            g_overlaps.exchange(0), threads);
    }
}
