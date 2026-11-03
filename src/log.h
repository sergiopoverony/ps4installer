#pragma once

#include <string>
#include <vector>

// Журнал службы: /data/ps4installer/log.txt, каждая строка сразу сбрасывается на диск.
// log_open() должен быть самым первым вызовом в main(): если папки /data/ps4installer
// после запуска payload'а нет, программа не дошла до main() (проблема загрузки).
void log_open(const char* dir);
void log_printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
// Последние строки журнала (для /status).
std::vector<std::string> log_tail(size_t max_lines);
