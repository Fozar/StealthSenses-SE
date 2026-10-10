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

#include "Detection.h"

#include "Config.h"
#include "ThreadProbe.h"

namespace StealthSenses::Detection {
    namespace {
        // The call to Actor::CalculateDetection inside DoDetectionJob: SE 41659+0x526 → 36758,
        // AE 42742+0x67B → 37774 [PE: powerof3_PapyrusExtenderSSE/src/Game/Manager.cpp; checked by
        // disassembly on SE 1.5.97 and AE 1.7.104 — open_questions #15]
        constexpr REL::RelocationID kDetectionJob{ 41659, 42742 };
        constexpr REL::RelocationID kCalculateDetection{ 36758, 37774 };

        ThreadProbe::Counter       g_calls;  // flagged: the target is the player
        std::atomic<std::int32_t>  g_playerMin{ INT32_MAX };  // detectionValue range on the player
        std::atomic<std::int32_t>  g_playerMax{ INT32_MIN };

        void Widen(std::atomic<std::int32_t>& a_bound, std::int32_t a_value, bool a_min) {
            auto cur = a_bound.load(std::memory_order_relaxed);
            while ((a_min ? a_value < cur : a_value > cur) &&
                   !a_bound.compare_exchange_weak(cur, a_value, std::memory_order_relaxed)) {}
        }

        // Signature as hooked by PE (source detects target); the meaning of the u8/u32/float
        // outputs and pos is open_questions #1. The return value is unused by the caller.
        struct CalculateDetection {
            static std::uintptr_t thunk(RE::Actor* a_source, RE::Actor* a_target, std::int32_t& a_detectionValue,
                std::uint8_t& a_unk04, std::uint8_t& a_unk05, std::uint32_t& a_unk06, RE::NiPoint3& a_pos,
                float& a_unk08, float& a_unk09, float& a_unk10) {
                const auto result = func(a_source, a_target, a_detectionValue, a_unk04, a_unk05, a_unk06, a_pos,
                    a_unk08, a_unk09, a_unk10);

                const bool onPlayer = a_target && a_target == RE::PlayerCharacter::GetSingleton();
                g_calls.Hit(onPlayer);
                if (onPlayer) {
                    Widen(g_playerMin, a_detectionValue, true);
                    Widen(g_playerMax, a_detectionValue, false);
                }
                return result;
            }

            static inline REL::Relocation<decltype(thunk)> func;
        };

        std::uintptr_t g_site    = 0;  // the patched call, 0 = not installed
        std::uintptr_t g_ourStub = 0;  // where our patch made it jump

        std::uintptr_t CallTarget(std::uintptr_t a_site) {
            std::int32_t rel = 0;
            std::memcpy(&rel, reinterpret_cast<const void*>(a_site + 1), sizeof(rel));
            return a_site + 5 + rel;
        }
    }

    void Install() {
        if (!ThreadProbe::Enabled()) {
            return;
        }
        const REL::Relocation<std::uintptr_t> site{ kDetectionJob, REL::Relocate(0x526, 0x67B) };
        const auto* code = reinterpret_cast<const std::uint8_t*>(site.address());
        if (code[0] != 0xE8) {
            SKSE::log::warn("Detection probe: no call at DoDetectionJob+0x{:X} (byte {:02X}), not installed",
                site.offset() - REL::Relocation<std::uintptr_t>{ kDetectionJob }.offset(), code[0]);
            return;
        }
        // Another plugin (Papyrus Extender) may have patched the call already: then the target is
        // its thunk, and ours calls it in turn
        const bool vanilla = CallTarget(site.address()) == REL::Relocation<std::uintptr_t>{ kCalculateDetection }.address();

        CalculateDetection::func = SKSE::GetTrampoline().write_call<5>(site.address(), CalculateDetection::thunk);
        g_site                   = site.address();
        g_ourStub                = CallTarget(g_site);
        SKSE::log::info("Detection probe installed (call target: {})",
            vanilla ? "vanilla CalculateDetection" : "already patched by another plugin, chained");
    }

    void Verify() {
        if (g_site == 0) {
            return;
        }
        // A plugin patching the same call after us (at its own load or later) chains to our thunk
        // through its func, so both still run; this only tells which one runs first
        const bool ours = CallTarget(g_site) == g_ourStub;
        SKSE::log::info("Detection probe check: call {}", ours ? "still ours" : "patched again by another plugin after us");
    }

    void Report() {
        if (!ThreadProbe::Enabled()) {
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

        std::uint32_t total = 0, onPlayer = 0;
        const auto    threads = g_calls.Drain(total, onPlayer);
        const auto    lo      = g_playerMin.exchange(INT32_MAX);
        const auto    hi      = g_playerMax.exchange(INT32_MIN);
        SKSE::log::info("CalculateDetection, {:.1f} s: {} calls, {} on the player{}; threads:{}", span, total, onPlayer,
            onPlayer ? std::format(" (detectionValue {}..{})", lo, hi) : "", threads);
    }
}
