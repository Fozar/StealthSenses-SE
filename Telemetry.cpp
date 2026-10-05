#include "Telemetry.h"

#include "Config.h"
#include "Trail.h"

namespace StealthSenses::Telemetry {
    namespace {
        std::ofstream                         g_file;
        std::chrono::steady_clock::time_point g_start;
        int                                   g_marks = 0;
    }

    void Open() {
        if (!Config::Get().debug.telemetry || g_file.is_open()) {
            return;
        }
        const auto dir = SKSE::log::log_directory();
        if (!dir) {
            SKSE::log::warn("Telemetry: no log directory, disabled");
            return;
        }
        const auto path = *dir / "StealthSenses.trace.jsonl";
        g_file.open(path, std::ios::out | std::ios::trunc | std::ios::binary);
        g_start = std::chrono::steady_clock::now();
        SKSE::log::info("Telemetry: writing {}", path.string());
    }

    bool Enabled() {
        return g_file.is_open();
    }

    void Write(nlohmann::json a_record) {
        if (!g_file.is_open()) {
            return;
        }
        a_record["t"] = std::round(std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count() * 100.0) / 100.0;
        if (const auto* calendar = RE::Calendar::GetSingleton()) {
            a_record["gh"] = calendar->GetHoursPassed();
        }
        // Game strings may be invalid UTF-8; replace instead of throwing
        g_file << a_record.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) << '\n';
        g_file.flush();
    }

    void Log(std::string_view a_text) {
        Write({ { "type", "log" }, { "text", a_text } });
    }

    void Mark() {
        ++g_marks;
        auto* player = RE::PlayerCharacter::GetSingleton();
        nlohmann::json record{ { "type", "mark" }, { "n", g_marks } };
        if (player) {
            record["pos"] = Vec(player->GetPosition());
            record["space"] = Hex(Trail::SpaceOf(player));
        }
        Write(std::move(record));

        const auto text = std::format("Mark {}", g_marks);
        SKSE::log::info("=== {} ===", text);
        if (auto* console = RE::ConsoleLog::GetSingleton()) {
            console->Print("[StealthSenses] %s", text.c_str());
        }
        RE::SendHUDMessage::ShowHUDMessage(std::format("StealthSenses: {}", text).c_str());
    }

    nlohmann::json Vec(const RE::NiPoint3& a_pos) {
        return { std::round(a_pos.x), std::round(a_pos.y), std::round(a_pos.z) };
    }

    std::string Hex(std::uint32_t a_value) {
        return std::format("{:08X}", a_value);
    }
}
