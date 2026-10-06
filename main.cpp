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

// RE headers must precede <Windows.h>: its macros (MAX_PATH, …) break CommonLib declarations
#include "Config.h"
#include "Serialization.h"
#include "Telemetry.h"
#include "Trackers.h"
#include "Trail.h"

#include <spdlog/sinks/basic_file_sink.h>
#include <Windows.h>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace {
    using namespace StealthSenses;

    std::atomic<DWORD>         g_mainThreadId{ 0 };
    std::atomic<std::uint32_t> g_playerHandle{ 0 };
    std::atomic<bool>          g_tickPending{ false };

    void InitializeLogging() {
        auto logPath = SKSE::log::log_directory().value_or(
            []() {
                wchar_t buf[MAX_PATH] = {};
                GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), buf, MAX_PATH);
                return std::filesystem::path(buf).parent_path();
            }()) / "StealthSenses.log";

        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logPath.string(), true);
        auto log  = std::make_shared<spdlog::logger>("global", std::move(sink));
        log->set_level(spdlog::level::info);
        log->flush_on(spdlog::level::info);
        log->set_pattern("[%H:%M:%S.%e] [%l] %v");
        spdlog::set_default_logger(std::move(log));
    }

    void ApplyLogLevel() {
        const auto level = spdlog::level::from_str(Config::Get().debug.log_level);
        spdlog::default_logger()->set_level(level);
        spdlog::default_logger()->flush_on(level);
    }

    // Open question #10: which tags BGSFootstepEvent carries, for whom, and on which thread.
    class FootstepSink final : public RE::BSTEventSink<RE::BGSFootstepEvent> {
    public:
        RE::BSEventNotifyControl ProcessEvent(const RE::BGSFootstepEvent* a_event,
            RE::BSTEventSource<RE::BGSFootstepEvent>*) override {
            if (!a_event) {
                return RE::BSEventNotifyControl::kContinue;
            }
            const bool isPlayer = a_event->actor.native_handle() == g_playerHandle.load();
            LogTag(a_event, isPlayer);

            // The manager also carries JumpUp/JumpDown and creature breathing tags; only
            // Foot* (FootLeft, FootSprintRight, FootFront, …) are steps
            if (isPlayer && a_event->tag.c_str() && std::string_view(a_event->tag.c_str()).starts_with("Foot")) {
                SKSE::GetTaskInterface()->AddTask([]() {
                    Trail::NoteFootstepEvent();
                    Trail::TryRecord(RE::PlayerCharacter::GetSingleton(), "footstep");
                });
            }
            return RE::BSEventNotifyControl::kContinue;
        }

    private:
        void LogTag(const RE::BGSFootstepEvent* a_event, bool a_isPlayer) {
            if (!Config::Get().debug.log_footstep_tags) {
                return;
            }
            const std::string tag = a_event->tag.c_str() ? a_event->tag.c_str() : "";
            std::scoped_lock lock(_mutex);
            if (_seen.size() < 64 && _seen.emplace(tag, a_isPlayer).second) {
                SKSE::log::info("Footstep tag '{}' (player: {}) on thread {} (main {})",
                    tag, a_isPlayer, GetCurrentThreadId(), g_mainThreadId.load());
            }
        }

        std::mutex                                 _mutex;
        std::set<std::pair<std::string, bool>>     _seen;
    };

    class HitSink final : public RE::BSTEventSink<RE::TESHitEvent> {
    public:
        RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent* a_event,
            RE::BSTEventSource<RE::TESHitEvent>*) override {
            // Any hit on the player counts as bleeding: the event carries no damage amount
            if (a_event && a_event->target && a_event->target->IsPlayerRef()) {
                SKSE::GetTaskInterface()->AddTask([]() { Trail::MarkPlayerHit(); });
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    // Mark key: the tester flags a moment ("at mark 3 he turned back"); see Telemetry::Mark
    class InputSink final : public RE::BSTEventSink<RE::InputEvent*> {
    public:
        RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_events,
            RE::BSTEventSource<RE::InputEvent*>*) override {
            const auto key = static_cast<std::uint32_t>(Config::Get().debug.mark_key);
            if (!a_events || key == 0) {
                return RE::BSEventNotifyControl::kContinue;
            }
            for (auto* event = *a_events; event; event = event->next) {
                const auto* button = event->AsButtonEvent();
                if (button && button->GetDevice() == RE::INPUT_DEVICE::kKeyboard &&
                    button->GetIDCode() == key && button->IsDown()) {
                    SKSE::GetTaskInterface()->AddTask([]() { Telemetry::Mark(); });
                }
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    // Game-thread tick: poll sampler + tracker update. Time spent paused is not counted.
    void Tick() {
        using Clock = std::chrono::steady_clock;
        static Clock::time_point last   = Clock::now();
        static float             backlog = 0.0f;

        g_tickPending = false;

        const auto now = Clock::now();
        const float dt = std::min(std::chrono::duration<float>(now - last).count(), 1.0f);
        last           = now;

        auto* ui       = RE::UI::GetSingleton();
        auto* player   = RE::PlayerCharacter::GetSingleton();
        if (!ui || ui->GameIsPaused() || !player || !player->Is3DLoaded()) {
            return;
        }

        if (!Trail::FootstepEventsActive()) {
            Trail::TryRecord(player, "poll");
        }

        backlog += dt;
        if (backlog * 1000.0f >= static_cast<float>(Config::Get().tracker.interval_ms)) {
            const auto start = std::chrono::steady_clock::now();
            Trackers::Update(backlog);
            backlog = 0.0f;
            if (Telemetry::Enabled()) {
                // Main-thread cost of a tracker tick: trail scan, NPCs, sight rays
                const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start);
                Telemetry::Write({ { "type", "perf" }, { "us", us.count() }, { "footprints", Trail::Footprints().size() } });
            }
        }
    }

    void StartTicker() {
        std::thread([]() {
            const auto interval = std::chrono::milliseconds(Config::Get().trail.poll_interval_ms);
            for (;;) {
                std::this_thread::sleep_for(interval);
                if (!g_tickPending.exchange(true)) {
                    SKSE::GetTaskInterface()->AddTask(Tick);
                }
            }
        }).detach();
    }

    void OnDataLoaded() {
        g_mainThreadId = GetCurrentThreadId();

        if (auto* footsteps = RE::BGSFootstepManager::GetSingleton()) {
            static FootstepSink sink;
            footsteps->AddEventSink(&sink);
            SKSE::log::info("Footstep sink registered");
        } else {
            SKSE::log::warn("BGSFootstepManager not available, poll sampler only");
        }

        if (auto* events = RE::ScriptEventSourceHolder::GetSingleton()) {
            static HitSink sink;
            events->AddEventSink<RE::TESHitEvent>(&sink);
        }

        if (auto* input = RE::BSInputDeviceManager::GetSingleton()) {
            static InputSink sink;
            input->AddEventSink(&sink);
        }

        if (!Trackers::Init()) {
            SKSE::log::error("Trackers: vanilla package/keyword/XMarker not found, tracking disabled");
        }
        Telemetry::Open();
        StartTicker();
    }

    void OnGameLoaded() {
        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
            g_playerHandle = player->GetHandle().native_handle();
        }
        Trackers::Clear();
        Trackers::ReleaseStale();
        SKSE::log::info("Game loaded: {} footprints in trail", Trail::Footprints().size());

        // A load starts a new timeline in the trace: the viewer splits sessions on it, and the
        // restored trail is dumped so footprints from before this run are on the map too
        Telemetry::Write({ { "type", "load" }, { "footprints", Trail::Footprints().size() } });
        for (const auto& fp : Trail::Footprints()) {
            Telemetry::Write({ { "type", "footprint" }, { "seq", fp.seq }, { "pos", Telemetry::Vec(fp.pos) },
                { "space", Telemetry::Hex(fp.space) }, { "mat", Telemetry::Hex(static_cast<std::uint32_t>(fp.material)) },
                { "base", Trail::Visibility(fp, fp.gameHours, false) }, { "blood", (fp.flags & Trail::kBlood) != 0 },
                { "src", "save" } });
        }
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* skse) {
    SKSE::Init(skse);
    InitializeLogging();
    const auto* plugin = SKSE::PluginDeclaration::GetSingleton();
    const auto& ver    = plugin->GetVersion();
    SKSE::log::info("StealthSenses {}.{}.{} ({} build) — plugin loaded", ver.major(), ver.minor(), ver.patch(),
        kDevBuild ? "dev" : "release");

    Config::Load();
    ApplyLogLevel();
    Serialization::Register();

    SKSE::GetMessagingInterface()->RegisterListener([](SKSE::MessagingInterface::Message* msg) {
        using MI = SKSE::MessagingInterface;
        switch (msg->type) {
        case MI::kDataLoaded:
            OnDataLoaded();
            break;
        case MI::kNewGame:
        case MI::kPostLoadGame:
            OnGameLoaded();
            break;
        }
    });

    return true;
}
