#include "Telemetry.h"

#include "Config.h"
#include "Trail.h"

namespace StealthSenses::Telemetry {
    namespace {
        std::ofstream                         g_file;
        std::filesystem::path                 g_path;
        std::uintmax_t                        g_bytes = 0;
        std::chrono::steady_clock::time_point g_start;
        int                                   g_marks = 0;

        // Release users may leave telemetry on for hours (~7 MB/h near NPCs): keep the newest
        // part in the file and the previous one in .1
        void RotateIfNeeded() {
            const auto limit = static_cast<std::uintmax_t>(std::max(Config::Get().debug.telemetry_max_mb, 1)) << 20;
            if (g_bytes < limit) {
                return;
            }
            g_file.close();
            auto            old = g_path;
            std::error_code ec;
            old += ".1";
            std::filesystem::remove(old, ec);
            std::filesystem::rename(g_path, old, ec);
            g_file.open(g_path, std::ios::out | std::ios::trunc | std::ios::binary);
            g_bytes = 0;
        }
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
        g_path = *dir / "StealthSenses.trace.jsonl";
        // The previous game run survives as .prev: a quick restart used to wipe the trace of the
        // run that was being reported
        {
            auto            prev = g_path;
            std::error_code ec;
            prev += ".prev";
            if (std::filesystem::exists(g_path, ec)) {
                std::filesystem::remove(prev, ec);
                std::filesystem::rename(g_path, prev, ec);
            }
        }
        g_file.open(g_path, std::ios::out | std::ios::trunc | std::ios::binary);
        g_start = std::chrono::steady_clock::now();
        SKSE::log::info("Telemetry: writing {}", g_path.string());
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
        const auto line = a_record.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
        g_file << line << '\n';
        g_file.flush();
        g_bytes += line.size() + 1;
        RotateIfNeeded();
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
        if (Config::Get().debug.console) {
            if (auto* console = RE::ConsoleLog::GetSingleton()) {
                console->Print("[StealthSenses] %s", text.c_str());
            }
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
