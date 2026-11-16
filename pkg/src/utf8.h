// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SergioPoverony
#pragma once
#include <string>

// Разбор UTF-8 без зависимостей от консоли (проверяется тестами на ПК).
// Кириллица занимает 2 байта: любой подсчёт ширины и перенос строк - только по символам.
int decode_utf8(const unsigned char*& p);   // следующий символ, p сдвигается; '?' при ошибке
int count_cp(const std::string& s);         // число символов
