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

// Detection hook: the call to Actor::CalculateDetection inside DoDetectionJob.
// For now a dev-build probe only (debug.thread_probe): which threads run the detection
// math and how often — open_questions #3, decides how 0.9 may feed it suspicion data.

namespace StealthSenses::Detection {
    // At plugin load: patches the call site (dev build with the probe on), else does nothing
    void Install();

    // kDataLoaded and each game load: logs whether another plugin patched the call after Install
    void Verify();

    // Game thread: every 5 s logs the calls per thread since the previous report
    void Report();
}
