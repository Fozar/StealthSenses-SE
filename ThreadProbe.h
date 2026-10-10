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

#pragma once

// Dev-build thread probe (debug.thread_probe), open_questions #3 and #21: which engine threads
// run the detection math and our own tasks, and whether two pieces of our code ever touch the
// plugin state (Trail, Trackers) at once. The state has no locks: that holds only if they never do.
// Release build: everything here is a no-op.

#include <Windows.h>

namespace StealthSenses::ThreadProbe {
    bool Enabled();

    // At plugin load: the game's main thread (it also runs save/load; kDataLoaded is not on it)
    void  SetMainThread();
    DWORD MainThread();

    // Calls per thread since the last Drain; safe from any thread (atomics only)
    class Counter {
    public:
        void Hit(bool a_flagged = false);
        // " 1234=10 5678(main)=3" for the threads that had calls, then zeroes the counts
        std::string Drain(std::uint32_t& a_total, std::uint32_t& a_flagged);

    private:
        struct Slot {
            std::atomic<DWORD>         thread{ 0 };
            std::atomic<std::uint32_t> calls{ 0 };
            std::atomic<std::uint32_t> flagged{ 0 };
        };
        Slot* Claim(DWORD a_thread);

        std::array<Slot, 32>       _slots;
        std::atomic<std::uint32_t> _unslotted{ 0 };
    };

    // Wraps code that touches the plugin state: every task, the co-save callbacks, game load.
    // Another section entering from a different thread before this one ends is logged as an
    // overlap — the proof that the "no locks" model is wrong.
    class Section {
    public:
        explicit Section(const char* a_name);
        ~Section();
        Section(const Section&)            = delete;
        Section& operator=(const Section&) = delete;

    private:
        bool _owner = false;
    };

    // Game state code: every 5 s logs where our sections ran and the overlaps seen
    void Report();
}
