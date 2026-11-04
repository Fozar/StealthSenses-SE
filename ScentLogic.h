// Stealth Senses — scent rules, independent of the engine.
// Copyright (C) 2026 fozar
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Config.h"
#include "Trail.h"
#include "ProfileLogic.h"
#include <algorithm>
#include <cmath>
#include <span>

namespace StealthSenses::Scent {
    inline float WeatherFactor(bool exterior, bool rain, bool snow, const Config::ScentSettings& cfg) {
        return !exterior ? 1.0f : rain ? cfg.rain_mult : snow ? cfg.snow_mult : 1.0f;
    }
    inline float BodyFactor(bool torso, bool hands, bool feet, bool head, bool sprint, const Config::ScentSettings& cfg) {
        const float covered = (torso ? cfg.body_torso_reduction : 0.0f) +
            (static_cast<int>(hands) + static_cast<int>(feet) + static_cast<int>(head)) * cfg.body_limb_reduction;
        return std::max(0.0f, 1.0f - covered) * (sprint ? cfg.body_sprint_mult : 1.0f);
    }
    template <class Clear>
    const Trail::Footprint* FirstReachable(std::span<const Trail::Footprint* const> a_ordered, Clear a_clear, std::size_t a_budget) {
        for (const auto* fp : a_ordered) {
            if (a_budget == 0) break;
            --a_budget;
            if (a_clear(*fp)) return fp;
        }
        return nullptr;
    }
    // Runtime IDs are local to this trail. Saved IDs are remapped by run, so arbitrary saved
    // integers cannot exhaust the counter; a load always starts a fresh live run afterwards.
    class Chain {
    public:
        std::uint32_t Current() const { return current_; }
        void Break() { current_ = 0; restored_ = 0; }
        std::uint32_t Stamp(RE::FormID a_space) {
            if (a_space == 0) return 0;
            if (current_ == 0 || space_ != a_space) { current_ = ++next_; space_ = a_space; }
            return current_;
        }
        std::uint32_t Restore(std::uint32_t a_saved, RE::FormID a_space) {
            if (a_saved == 0) { Break(); return 0; }
            if (restored_ != a_saved || space_ != a_space) Break();
            restored_ = a_saved;
            return Stamp(a_space);
        }
    private:
        std::uint32_t next_ = 0, current_ = 0, restored_ = 0;
        RE::FormID space_ = 0;
    };

    struct Sample {
        float peak = 0.0f;
        float radius = 0.0f; // reference spread radius, before the reader's range multiplier
    };

    // A bounded ground approximation, not an air simulation. The area grows by g^2, so the
    // central concentration divides by g^2; the integral of the linear radial cone still decays
    // by the original half-life. Recomputed from born, including after loading a save.
    inline Sample SampleAt(const Trail::Footprint& a_fp, float a_nowHours, const Config::ScentSettings& a_cfg) {
        if (!a_cfg.enabled || a_fp.scentSegment == 0 || !std::isfinite(a_fp.gameHours) ||
            !std::isfinite(a_nowHours) || !std::isfinite(a_cfg.halflife_hours) || a_cfg.halflife_hours <= 0.0f ||
            !std::isfinite(a_cfg.radius) || a_cfg.radius <= 0 || !std::isfinite(a_cfg.spread_max_mult) ||
            a_cfg.spread_max_mult < 1 || a_cfg.spread_max_mult > 2 || !std::isfinite(a_cfg.spread_hours) || a_cfg.spread_hours <= 0) {
            return {};
        }
        const float base = a_cfg.base_strength * a_fp.scentBody + ((a_fp.flags & Trail::kBlood) ? a_cfg.blood_bonus : 0.0f);
        if (!std::isfinite(base) || base <= 0.0f) {
            return {};
        }
        const float age = std::max(a_nowHours - a_fp.gameHours, 0.0f);
        const float growth = 1.0f + (a_cfg.spread_max_mult - 1.0f) * -std::expm1(-age / a_cfg.spread_hours);
        return { base * std::exp2(-age / a_cfg.halflife_hours) / (growth * growth), a_cfg.radius * growth };
    }

    inline float Strength(const Trail::Footprint& a_fp, float a_nowHours, const Config::ScentSettings& a_cfg) {
        return SampleAt(a_fp, a_nowHours, a_cfg).peak;
    }

    // Potential concentration in open space. The NPC reader applies its profile and barrier check.
    inline float AtDistance(const Sample& a_source, float a_distance, const Config::ScentSettings& a_cfg) {
        if (!a_cfg.enabled || !std::isfinite(a_source.peak) || !std::isfinite(a_distance) ||
            a_source.peak <= 0.0f || !std::isfinite(a_source.radius) || a_source.radius <= 0.0f) {
            return 0.0f;
        }
        return a_source.peak * std::clamp(1.0f - std::max(a_distance, 0.0f) / a_source.radius, 0.0f, 1.0f);
    }

    // Reference isoline at min_strength: may grow briefly, then shrinks to zero. Not a barrier test.
    inline float Radius(const Sample& a_source, const Config::ScentSettings& a_cfg) {
        if (!a_cfg.enabled || !std::isfinite(a_source.peak) || a_source.peak <= a_cfg.min_strength ||
            !std::isfinite(a_source.radius) || a_source.radius <= 0.0f || !std::isfinite(a_cfg.min_strength) || a_cfg.min_strength <= 0.0f) {
            return 0.0f;
        }
        return a_source.radius * (1.0f - a_cfg.min_strength / a_source.peak);
    }

    inline bool Connected(const Trail::Footprint& a_from, const Trail::Footprint& a_to) {
        return a_from.space == a_to.space && a_from.scentSegment != 0 && a_from.scentSegment == a_to.scentSegment;
    }

    inline float Reach(bool a_blood, const Profiles::Profile& a_profile) {
        const float reach = a_profile.smell * (a_blood ? a_profile.smellBlood : 1.0f);
        return std::isfinite(reach) ? std::clamp(reach, 0.0f, Profiles::kMaxSmell) : 0.0f;
    }

    // Sensitivity extends range, not lifetime. Weather affects perception only.
    inline float Perceived(const Sample& a_source, float a_distance, bool a_blood,
        const Profiles::Profile& a_profile, const Config::ScentSettings& a_cfg, float a_weather = 1.0f) {
        const float reach = Reach(a_blood, a_profile);
        return reach > 0 ? AtDistance(a_source, a_distance / reach, a_cfg) * a_weather : 0.0f;
    }

    // Segment zero is only allowed as the reader's initial (not-yet-acquired) segment.
    inline bool Ahead(const Trail::Footprint& a_fp, std::uint32_t a_seq, std::uint32_t a_segment) {
        return a_fp.seq > a_seq && a_fp.scentSegment != 0 &&
            (a_segment == 0 || a_fp.scentSegment == a_segment);
    }
}
