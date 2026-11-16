// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SergioPoverony
#pragma once
void log_reset();
void logf_(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
const char* log_path();
