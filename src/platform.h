// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SergioPoverony
#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Всё, что зависит от консоли. На консоли (payload SDK) вызывает системные функции,
// при сборке с -DHOST_TEST заменено заглушками, чтобы логику можно было проверять на ПК.
namespace platform {

void notify(const std::string& msg);  // всплывающее уведомление на экране консоли
// Уведомление с иконкой (icon_uri: путь к картинке или адрес вида cxml://...; пусто - без иконки).
void notify_icon(const std::string& msg, const std::string& icon_uri);
// Title ID установленных игр (папки в /user/app). На ПК: переменная FAKE_INSTALLED="A,B".
std::vector<std::string> installed_title_ids();
// Установленные игры с названиями (из param.sfo установленной игры; пусто, если прочитать не удалось).
// На ПК: FAKE_INSTALLED="ID:Название,ID2".
struct InstalledGame {
    std::string title_id, title;
    std::string loc;   // где стоит: "HDD" (внутренний диск) или "EXT" (расширенное хранилище /mnt/ext*)
};
std::vector<InstalledGame> installed_games();
// Путь к icon0.png установленной игры или пустая строка. На ПК: папка FAKE_APPMETA/<ID>/icon0.png.
std::string app_icon_path(const std::string& title_id);
// app.pkg установленной игры (на EXT вместо sce_sys лежит он: название и иконка читаются из него). Пусто, если нет.
std::string app_pkg_path(const std::string& title_id);
// Запуск установленной игры. Не проверено на консоли. На ПК: пишет Title ID в файл FAKE_LAUNCH_LOG.
bool launch_app(const std::string& title_id, std::string* err);
// Удаление установленной игры вместе с данными. Не проверено на консоли. На ПК: пишет Title ID в FAKE_UNINSTALL_LOG.
bool uninstall_app(const std::string& title_id, std::string* err);
void power_tick();                    // не давать консоли уйти в сон на время установки
bool free_bytes(const char* path, uint64_t* out);
// Полный размер диска, на котором лежит path (для полоски занятого места). На ПК: FAKE_TOTAL_BYTES.
bool total_bytes(const char* path, uint64_t* out);

// Версия прошивки консоли: возвращает основную часть (13 для 13.02) или -1, если узнать не удалось.
// В *text - строка версии для журнала и /health.
int firmware_major(std::string* text);

// Системная установка (BGFT). Библиотека подгружается во время работы, а не при запуске
// payload'а: так список зависимостей ELF остаётся минимальным.
bool bgft_load(std::string* err);  // идемпотентно
std::string bgft_info();           // какие функции найдены (для /health и журнала)

// Регистрирует установку локального файла. path - в виде для системы (/user/data/... или /mnt/usbN/...).
bool bgft_register_local(const std::string& path, const std::string& content_id, int* task_id, uint32_t* err);
bool bgft_start(int task_id, uint32_t* err);

struct Progress {
    uint32_t bits = 0;
    int32_t error_result = 0;
    uint64_t length = 0;
    uint64_t transferred = 0;
};
bool bgft_progress(int task_id, Progress* p, uint32_t* err);

constexpr uint32_t BGFT_ERR_ALREADY_INSTALLED = 0x80990088;
// Система ещё занята прежней задачей с тем же content id (бывает сразу после предыдущей установки;
// причина предположительная, на консоли не подтверждена): регистрацию стоит повторить позже.
constexpr uint32_t BGFT_ERR_BUSY = 0x80990015;

}  // namespace platform
