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

#include "Config.h"

#include <nlohmann/json.hpp>

namespace StealthSenses::Config {
    namespace {
        Settings g_settings;

        // Missing keys and wrong value types are ignored: the default stays.
        template <class T>
        void Read(const nlohmann::json& a_obj, const char* a_key, T& a_out) {
            const auto it = a_obj.find(a_key);
            if (it == a_obj.end()) {
                return;
            }
            try {
                a_out = it->get<T>();
            } catch (const nlohmann::json::exception&) {
                SKSE::log::warn("Config: key '{}' has wrong type, default kept", a_key);
            }
        }

        const nlohmann::json& Section(const nlohmann::json& a_root, const char* a_name) {
            static const nlohmann::json empty = nlohmann::json::object();
            const auto it = a_root.find(a_name);
            return it != a_root.end() && it->is_object() ? *it : empty;
        }
    }

    const Settings& Get() {
        return g_settings;
    }

    void Load() {
        const std::filesystem::path path = "Data/SKSE/Plugins/StealthSensesConfig.json";
        std::ifstream file(path);
        if (!file) {
            SKSE::log::info("Config: {} not found, using defaults", path.string());
            return;
        }

        nlohmann::json root;
        try {
            root = nlohmann::json::parse(file, nullptr, true, true);
        } catch (const nlohmann::json::exception& e) {
            SKSE::log::error("Config: parse error, using defaults: {}", e.what());
            return;
        }

        auto& t = g_settings.trail;
        const auto& jt = Section(root, "trail");
        Read(jt, "enabled", t.enabled);
        Read(jt, "min_spacing", t.min_spacing);
        Read(jt, "max_footprints", t.max_footprints);
        Read(jt, "poll_interval_ms", t.poll_interval_ms);
        Read(jt, "halflife_hours", t.halflife_hours);
        Read(jt, "weather_halflife_mult", t.weather_halflife_mult);
        Read(jt, "soft_visibility", t.soft_visibility);
        Read(jt, "gravel_visibility", t.gravel_visibility);
        Read(jt, "hard_visibility", t.hard_visibility);
        Read(jt, "unknown_visibility", t.unknown_visibility);
        Read(jt, "blood_steps", t.blood_steps);
        Read(jt, "blood_visibility_bonus", t.blood_visibility_bonus);

        auto& k = g_settings.tracker;
        const auto& jk = Section(root, "tracker");
        Read(jk, "enabled", k.enabled);
        Read(jk, "interval_ms", k.interval_ms);
        Read(jk, "notice_radius", k.notice_radius);
        Read(jk, "notice_fov", k.notice_fov);
        Read(jk, "lead_distance", k.lead_distance);
        Read(jk, "gap_distance", k.gap_distance);
        Read(jk, "arrive_radius", k.arrive_radius);
        Read(jk, "retarget_seconds", k.retarget_seconds);
        Read(jk, "lost_seconds", k.lost_seconds);
        Read(jk, "examine_seconds", k.examine_seconds);
        Read(jk, "examine_every", k.examine_every);
        Read(jk, "search_radius", k.search_radius);
        Read(jk, "hop_timeout", k.hop_timeout);
        Read(jk, "look_seconds", k.look_seconds);
        Read(jk, "draw_weapon", k.draw_weapon);
        Read(jk, "set_alert", k.set_alert);
        Read(jk, "stuck_seconds", k.stuck_seconds);
        Read(jk, "stuck_give_up", k.stuck_give_up);
        Read(jk, "repickup_seconds", k.repickup_seconds);
        Read(jk, "min_visibility", k.min_visibility);
        Read(jk, "faint_visibility", k.faint_visibility);
        Read(jk, "close_read_radius", k.close_read_radius);
        Read(jk, "caution_level", k.caution_level);
        Read(jk, "after_combat_seconds", k.after_combat_seconds);
        Read(jk, "require_hostile", k.require_hostile);
        Read(jk, "humanoids_only", k.humanoids_only);
        Read(jk, "include_combat", k.include_combat);
        Read(jk, "max_trackers", k.max_trackers);
#ifdef STEALTHSENSES_DEV
        Read(jk, "debug_all_npcs", k.debug_all_npcs);
#endif

        auto& d = g_settings.debug;
        const auto& jd = Section(root, "debug");
        Read(jd, "log_level", d.log_level);
        Read(jd, "telemetry", d.telemetry);
        Read(jd, "telemetry_max_mb", d.telemetry_max_mb);
#ifdef STEALTHSENSES_DEV
        Read(jd, "console", d.console);
        Read(jd, "log_footstep_tags", d.log_footstep_tags);
#endif
        Read(jd, "mark_key", d.mark_key);

        // Clamp values that would break the loops below
        t.min_spacing      = std::max(t.min_spacing, 8.0f);
        t.max_footprints   = std::clamp(t.max_footprints, 16, 65536);
        t.poll_interval_ms = std::clamp(t.poll_interval_ms, 50, 5000);
        t.halflife_hours   = std::max(t.halflife_hours, 0.01f);
        k.interval_ms      = std::clamp(k.interval_ms, t.poll_interval_ms, 10000);
        k.max_trackers     = std::max(k.max_trackers, 1);

        SKSE::log::info("Config: loaded {}", path.string());
    }
}
