// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SergioPoverony
// Тексты интерфейса по языкам: файлы assets/misc/lang_<код>.txt, строки вида «ключ=текст».
// Сначала грузится en.txt (запасной), поверх него - файл языка системы. Нет ключа - показывается сам ключ.
#pragma once
#include <string>

// dir - папка с файлами языков (на консоли /app0/assets/misc), lang - код языка ("ru", "en", ...).
void i18n_load(const std::string& dir, const std::string& lang);

// Текст по ключу.
const std::string& tr(const char* key);

// Текст по ключу с подстановкой {1} и {2}.
std::string trf(const char* key, const std::string& a, const std::string& b = "");

// Код языка по номеру языка системы PS4 (ORBIS_SYSTEM_PARAM_LANG_*). Неподдерживаемые языки -> "en".
std::string i18n_code_for_system_lang(int sysLang);
