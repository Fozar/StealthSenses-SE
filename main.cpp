// RE headers must precede <Windows.h>: its macros (MAX_PATH, …) break CommonLib declarations
#include "Config.h"
#include "Serialization.h"
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
            Trackers::Update(backlog);
            backlog = 0.0f;
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

        Trackers::LogSoundLevels();
        StartTicker();
    }

    void OnGameLoaded() {
        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
            g_playerHandle = player->GetHandle().native_handle();
        }
        Trackers::Clear();
        SKSE::log::info("Game loaded: {} footprints in trail", Trail::Footprints().size());
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* skse) {
    SKSE::Init(skse);
    InitializeLogging();
    const auto* plugin = SKSE::PluginDeclaration::GetSingleton();
    const auto& ver    = plugin->GetVersion();
    SKSE::log::info("StealthSenses {}.{}.{} — plugin loaded", ver.major(), ver.minor(), ver.patch());

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
