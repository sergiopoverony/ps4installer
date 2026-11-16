// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SergioPoverony
#pragma once

#include <string>
#include <vector>

// Журнал службы: /data/ps4installer/log.txt, каждая строка сразу сбрасывается на диск.
// log_open() должен быть самым первым вызовом в main(): если папки /data/ps4installer
// после запуска payload'а нет, программа не дошла до main() (проблема загрузки).
void log_open(const char* dir);
// Записать в журнал номер сигнала при аварии (SIGSEGV и др.) и завершиться.
void log_install_crash_handler();
// Что делала служба в момент аварии (последний запрос): печатается вместе с номером сигнала.
void log_set_context(const char* what);
void log_printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
// Последние строки журнала (для /status).
std::vector<std::string> log_tail(size_t max_lines);
