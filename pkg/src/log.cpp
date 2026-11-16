// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SergioPoverony
#include "log.h"
#include <stdio.h>
#include <stdarg.h>
#include <sys/stat.h>

#define LOG_DIR "/data/ps4installer_app"
static const char* LOGPATH = LOG_DIR "/log.txt";

void log_reset() {
    mkdir(LOG_DIR, 0777);
    FILE* f = fopen(LOGPATH, "w");
    if (f) fclose(f);
}

void logf_(const char* fmt, ...) {
    FILE* f = fopen(LOGPATH, "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

const char* log_path() { return LOGPATH; }
