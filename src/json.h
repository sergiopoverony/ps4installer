// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SergioPoverony
#pragma once

#include <string>
#include <vector>

// Минимальные помощники для JSON: экранирование строк и чтение массива строк по ключу.
std::string json_escape(const std::string& s);

// Из тела вида {"paths":["a","b"]} достаёт массив строк по ключу. false, если ключа нет или формат неверный.
bool json_get_string_array(const std::string& body, const std::string& key, std::vector<std::string>* out);
