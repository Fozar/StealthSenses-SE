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
        constexpr std::int32_t      kNoCheck = -1000;

        ThreadProbe::Counter       g_calls;  // flagged: the target is the player
        std::atomic<std::int32_t>  g_playerMin{ INT32_MAX };  // detectionValue range on the player
        std::atomic<std::int32_t>  g_playerMax{ INT32_MIN };

        // The last CalculateDetection result per observer, on the player as target (open_questions #1:
        // what the outputs mean). Written by the detection workers, read by the tracker tick: a
        // seqlock per slot, a writer that finds a slot busy just skips this sample.
        struct Slot {
            std::atomic<RE::FormID>    source{ 0 };
            std::atomic<std::uint32_t> seq{ 0 };  // odd while being written
            std::atomic<std::int32_t>  value{ 0 };
            std::atomic<std::uint32_t> flags{ 0 };  // unk04 | unk05 << 8
            std::atomic<std::uint32_t> word{ 0 };
            std::array<std::atomic<float>, 6> floats{};  // pos.x, pos.y, pos.z, unk08, unk09, unk10
            std::atomic<std::uint64_t> ms{ 0 };
        };
        std::array<Slot, 256> g_samples;

        // Detection bonuses by observer, sorted by FormID: published whole by the tracker tick,
        // read by the detection workers (open_questions #3: six threads in parallel)
        using Bonuses = std::vector<std::pair<RE::FormID, std::int32_t>>;
        std::atomic<std::shared_ptr<const Bonuses>> g_bonuses;
        std::atomic<std::uint32_t>                  g_bonused{ 0 };  // calls raised since the last report

        std::int32_t BonusFor(RE::FormID a_source) {
            const auto bonuses = g_bonuses.load(std::memory_order_acquire);
            if (!bonuses) {
                return 0;
            }
            const auto it = std::ranges::lower_bound(*bonuses, a_source, {}, &Bonuses::value_type::first);
            return it != bonuses->end() && it->first == a_source ? it->second : 0;
        }

        Slot* FindSlot(RE::FormID a_source, bool a_claim) {
            const std::size_t start = (a_source * 2654435761u) % g_samples.size();
            for (std::size_t i = 0; i < 16; ++i) {
                auto&      slot = g_samples[(start + i) % g_samples.size()];
                RE::FormID cur  = slot.source.load();
                if (cur == a_source) {
                    return &slot;
                }
                if (cur == 0) {
                    if (!a_claim) {
                        return nullptr;
                    }
                    if (slot.source.compare_exchange_strong(cur, a_source) || cur == a_source) {
                        return &slot;
                    }
                }
            }
            return nullptr;
        }

        void Record(RE::FormID a_source, std::int32_t a_value, std::uint8_t a_unk04, std::uint8_t a_unk05,
            std::uint32_t a_unk06, const RE::NiPoint3& a_pos, float a_unk08, float a_unk09, float a_unk10) {
            auto* slot = FindSlot(a_source, true);
            if (!slot) {
                return;
            }
            auto seq = slot->seq.load(std::memory_order_relaxed);
            if ((seq & 1) || !slot->seq.compare_exchange_strong(seq, seq + 1, std::memory_order_acquire)) {
                return;
            }
            constexpr auto relaxed = std::memory_order_relaxed;
            slot->value.store(a_value, relaxed);
            slot->flags.store(a_unk04 | (static_cast<std::uint32_t>(a_unk05) << 8), relaxed);
            slot->word.store(a_unk06, relaxed);
            const float floats[] = { a_pos.x, a_pos.y, a_pos.z, a_unk08, a_unk09, a_unk10 };
            for (std::size_t i = 0; i < 6; ++i) {
                slot->floats[i].store(floats[i], relaxed);
            }
            slot->ms.store(GetTickCount64(), relaxed);
            slot->seq.store(seq + 2, std::memory_order_release);
        }

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

                const bool onPlayer = a_target && a_source && a_target == RE::PlayerCharacter::GetSingleton();
                if constexpr (kDevBuild) {
                    g_calls.Hit(onPlayer);
                    if (onPlayer && Config::Get().debug.detection_probe) {
                        // The engine's own result, before our bonus
                        Widen(g_playerMin, a_detectionValue, true);
                        Widen(g_playerMax, a_detectionValue, false);
                        Record(a_source->GetFormID(), a_detectionValue, a_unk04, a_unk05, a_unk06, a_pos, a_unk08,
                            a_unk09, a_unk10);
                    }
                }
                // The value is the detection level: meter = value + 100, detected from 0; -1000 = no
                // check this time (open_questions #1), left alone. The caller passes it on to
                // DetectionState::Set (SE 41675 / AE 42760, disassembly).
                if (onPlayer && a_detectionValue != kNoCheck) {
                    if (const auto bonus = BonusFor(a_source->GetFormID()); bonus > 0) {
                        a_detectionValue += bonus;
                        g_bonused.fetch_add(1, std::memory_order_relaxed);
                    }
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
        if (!Config::Get().detection.enabled && !Config::Get().debug.detection_probe) {
            return;
        }
        const REL::Relocation<std::uintptr_t> site{ kDetectionJob, REL::Relocate(0x526, 0x67B) };
        const auto* code = reinterpret_cast<const std::uint8_t*>(site.address());
        if (code[0] != 0xE8) {
            SKSE::log::warn("Detection hook: no call at DoDetectionJob+0x{:X} (byte {:02X}), not installed",
                site.offset() - REL::Relocation<std::uintptr_t>{ kDetectionJob }.offset(), code[0]);
            return;
        }
        // Another plugin (Papyrus Extender) may have patched the call already: then the target is
        // its thunk, and ours calls it in turn
        const bool vanilla = CallTarget(site.address()) == REL::Relocation<std::uintptr_t>{ kCalculateDetection }.address();

        CalculateDetection::func = SKSE::GetTrampoline().write_call<5>(site.address(), CalculateDetection::thunk);
        g_site                   = site.address();
        g_ourStub                = CallTarget(g_site);
        SKSE::log::info("Detection hook installed (call target: {})",
            vanilla ? "vanilla CalculateDetection" : "already patched by another plugin, chained");
    }

    void Verify() {
        if (g_site == 0) {
            return;
        }
        // A plugin patching the same call after us (at its own load or later) chains to our thunk
        // through its func, so both still run; this only tells which one runs first
        const bool ours = CallTarget(g_site) == g_ourStub;
        SKSE::log::info("Detection hook check: call {}", ours ? "still ours" : "patched again by another plugin after us");
    }

    std::optional<PlayerSample> LastOnPlayer(RE::FormID a_source) {
        auto* slot = g_site ? FindSlot(a_source, false) : nullptr;
        if (!slot) {
            return std::nullopt;
        }
        constexpr auto relaxed = std::memory_order_relaxed;
        for (int attempt = 0; attempt < 4; ++attempt) {
            const auto before = slot->seq.load(std::memory_order_acquire);
            if (before == 0 || (before & 1)) {
                continue;
            }
            PlayerSample s;
            s.value      = slot->value.load(relaxed);
            const auto f = slot->flags.load(relaxed);
            s.unk04      = static_cast<std::uint8_t>(f & 0xFF);
            s.unk05      = static_cast<std::uint8_t>(f >> 8);
            s.unk06      = slot->word.load(relaxed);
            s.pos        = { slot->floats[0].load(relaxed), slot->floats[1].load(relaxed), slot->floats[2].load(relaxed) };
            s.unk08      = slot->floats[3].load(relaxed);
            s.unk09      = slot->floats[4].load(relaxed);
            s.unk10      = slot->floats[5].load(relaxed);
            const auto ms = slot->ms.load(relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (slot->seq.load(relaxed) == before) {
                s.ageMs = static_cast<std::uint32_t>(GetTickCount64() - ms);
                return s;
            }
        }
        return std::nullopt;
    }

    void PublishBonuses(std::vector<std::pair<RE::FormID, std::int32_t>> a_bonuses) {
        std::ranges::sort(a_bonuses);
        g_bonuses.store(std::make_shared<const Bonuses>(std::move(a_bonuses)), std::memory_order_release);
    }

    void Report() {
        if (g_site == 0 || !Config::Get().debug.detection_probe) {
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
        SKSE::log::info("CalculateDetection, {:.1f} s: {} calls, {} on the player{}, {} raised by a bonus; threads:{}", span,
            total, onPlayer, onPlayer ? std::format(" (detectionValue {}..{})", lo, hi) : "", g_bonused.exchange(0), threads);
    }
}
