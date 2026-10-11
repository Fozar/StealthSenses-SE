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
        const std::string text{ std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
        try {
            g_settings = Parse(text);
        } catch (const nlohmann::json::exception& e) {
            SKSE::log::error("Config: parse error, using defaults: {}", e.what());
            return;
        }
        SKSE::log::info("Config: loaded {}", path.string());
    }

    std::optional<std::pair<std::string, RE::FormID>> ParseFormRef(std::string_view a_text) {
        const auto bar = a_text.find('|');
        if (bar == std::string_view::npos || bar == 0 || bar + 1 == a_text.size()) {
            return std::nullopt;
        }
        auto hex = a_text.substr(bar + 1);
        if (hex.starts_with("0x") || hex.starts_with("0X")) {
            hex.remove_prefix(2);
        }
        RE::FormID id = 0;
        const auto [end, ec] = std::from_chars(hex.data(), hex.data() + hex.size(), id, 16);
        if (ec != std::errc{} || end != hex.data() + hex.size() || id > 0xFFFFFF) {
            return std::nullopt;  // the load order byte is not part of it
        }
        return std::pair{ std::string(a_text.substr(0, bar)), id };
    }

    Settings Parse(std::string_view a_text) {
        // Comments allowed (the last true): users annotate their config
        const auto root = nlohmann::json::parse(a_text, nullptr, true, true);
        Settings   settings;

        auto& t = settings.trail;
        const auto& jt = Section(root, "trail");
        Read(jt, "enabled", t.enabled);
        Read(jt, "min_spacing", t.min_spacing);
        Read(jt, "max_footprints", t.max_footprints);
        Read(jt, "thin_spacing", t.thin_spacing);
        Read(jt, "poll_interval_ms", t.poll_interval_ms);
        Read(jt, "halflife_hours", t.halflife_hours);
        Read(jt, "weather_halflife_mult", t.weather_halflife_mult);
        Read(jt, "soft_visibility", t.soft_visibility);
        Read(jt, "gravel_visibility", t.gravel_visibility);
        Read(jt, "hard_visibility", t.hard_visibility);
        Read(jt, "unknown_visibility", t.unknown_visibility);
        Read(jt, "blood_steps", t.blood_steps);
        Read(jt, "blood_visibility_bonus", t.blood_visibility_bonus);

        auto& s = settings.step;
        const auto& js = Section(root, "step");
        Read(js, "enabled", s.enabled);
        Read(js, "skill_floor", s.skill_floor);
        Read(js, "sneak_mult", s.sneak_mult);
        Read(js, "walk_mult", s.walk_mult);
        Read(js, "sprint_mult", s.sprint_mult);
        Read(js, "heavy_boots_mult", s.heavy_boots_mult);
        Read(js, "heavy_armor_mult", s.heavy_armor_mult);
        Read(js, "overencumbered_mult", s.overencumbered_mult);
        Read(js, "light_foot_mult", s.light_foot_mult);
        Read(js, "khajiit_mult", s.khajiit_mult);
        Read(js, "bosmer_mult", s.bosmer_mult);
        Read(js, "argonian_mult", s.argonian_mult);
        Read(js, "beast_form_mult", s.beast_form_mult);
        Read(js, "min_mult", s.min_mult);
        Read(js, "max_mult", s.max_mult);
        Read(js, "khajiit_races", s.khajiit_races);
        Read(js, "argonian_races", s.argonian_races);
        Read(js, "bosmer_races", s.bosmer_races);

        auto& k = settings.tracker;
        const auto& jk = Section(root, "tracker");
        Read(jk, "enabled", k.enabled);
        Read(jk, "interval_ms", k.interval_ms);
        Read(jk, "notice_radius", k.notice_radius);
        Read(jk, "notice_fov", k.notice_fov);
        Read(jk, "notice_line_of_sight", k.notice_line_of_sight);
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
        Read(jk, "max_height_diff", k.max_height_diff);
        Read(jk, "caution_level", k.caution_level);
        Read(jk, "after_combat_seconds", k.after_combat_seconds);
        Read(jk, "body_found_seconds", k.body_found_seconds);
        Read(jk, "body_notice_radius", k.body_notice_radius);
        Read(jk, "body_know_radius", k.body_know_radius);
        Read(jk, "body_max_age_hours", k.body_max_age_hours);
        Read(jk, "give_up_seconds", k.give_up_seconds);
        Read(jk, "leash_distance", k.leash_distance);
        Read(jk, "tired_cooldown", k.tired_cooldown);
        Read(jk, "fight_pause_seconds", k.fight_pause_seconds);
        Read(jk, "require_hostile", k.require_hostile);
        Read(jk, "humanoids_only", k.humanoids_only);
        Read(jk, "include_combat", k.include_combat);
        Read(jk, "max_trackers", k.max_trackers);
#ifdef STEALTHSENSES_DEV
        Read(jk, "debug_all_npcs", k.debug_all_npcs);
#endif

        auto& e = settings.detection;
        const auto& je = Section(root, "detection");
        Read(je, "enabled", e.enabled);
        Read(je, "tracker_bonus", e.tracker_bonus);
        Read(je, "after_fight_bonus", e.after_fight_bonus);
        Read(je, "found_body_bonus", e.found_body_bonus);

        auto& d = settings.debug;
        const auto& jd = Section(root, "debug");
        Read(jd, "log_level", d.log_level);
        Read(jd, "telemetry", d.telemetry);
        Read(jd, "telemetry_max_mb", d.telemetry_max_mb);
#ifdef STEALTHSENSES_DEV
        Read(jd, "console", d.console);
        Read(jd, "log_footstep_tags", d.log_footstep_tags);
        Read(jd, "detection_probe", d.detection_probe);
        Read(jd, "thread_probe", d.thread_probe);
        Read(jd, "key_track", d.key_track);
        Read(jd, "key_body", d.key_body);
        Read(jd, "key_paint", d.key_paint);
        Read(jd, "key_reset", d.key_reset);
        Read(jd, "key_overlay", d.key_overlay);
#endif
        Read(jd, "mark_key", d.mark_key);

        // Clamp values that would break the loops below
        t.min_spacing      = std::max(t.min_spacing, 8.0f);
        t.max_footprints   = std::clamp(t.max_footprints, 16, 65536);
        t.poll_interval_ms = std::clamp(t.poll_interval_ms, 50, 5000);
        t.halflife_hours   = std::max(t.halflife_hours, 0.01f);
        t.thin_spacing     = std::max(t.thin_spacing, t.min_spacing);
        k.interval_ms      = std::clamp(k.interval_ms, t.poll_interval_ms, 10000);
        k.max_trackers     = std::max(k.max_trackers, 1);
        k.max_height_diff  = std::max(k.max_height_diff, 32.0f);  // 0 would rule out any uneven ground
        // A multiplier below 0 would make visibility negative; the bounds keep every player trackable
        for (float* mult : { &s.sneak_mult, &s.walk_mult, &s.sprint_mult, &s.heavy_boots_mult, &s.heavy_armor_mult,
                 &s.overencumbered_mult, &s.light_foot_mult, &s.khajiit_mult, &s.bosmer_mult, &s.argonian_mult,
                 &s.beast_form_mult }) {
            *mult = std::max(*mult, 0.0f);
        }
        s.skill_floor = std::clamp(s.skill_floor, 0.0f, 1.0f);
        s.min_mult    = std::max(s.min_mult, 0.0f);
        s.max_mult    = std::max(s.max_mult, s.min_mult);
        // A bonus only makes an NPC better at spotting the player, never worse
        e.tracker_bonus     = std::clamp(e.tracker_bonus, 0, 100);
        e.after_fight_bonus = std::clamp(e.after_fight_bonus, 0, 100);
        e.found_body_bonus  = std::clamp(e.found_body_bonus, 0, 100);

        return settings;
    }
}
