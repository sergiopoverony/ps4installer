// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SergioPoverony
#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Разбор заголовка PKG и работа с путями. Чистый C++, без вызовов консоли
// (проверяется тестами на ПК). Смещения полей заголовка взяты из открытых
// установщиков (flatz/ps4_remote_pkg_installer, cy33hc/ps4-webdav-client).

enum PkgContentType : uint32_t {
    PKG_TYPE_GD = 0x1A,  // игра, патч, ремастер
    PKG_TYPE_AC = 0x1B,  // дополнение с данными
    PKG_TYPE_AL = 0x1C,  // дополнение без данных
    PKG_TYPE_DP = 0x1E,  // дельта-патч
};

struct PkgInfo {
    std::string path;        // путь, как его видит служба (/data/pkg/... или /mnt/usbN/...)
    uint64_t file_size = 0;  // размер файла на диске
    uint64_t pkg_size = 0;   // размер из заголовка
    std::string content_id;  // 36 символов, например UP0000-CUSA00000_00-EXAMPLEGAME00000
    std::string title_id;    // CUSA00000
    uint32_t content_type = 0;
    uint32_t flags = 0;
    std::string kind;        // game | patch | dlc | dlc_nodata | delta_patch
    bool is_patch = false;
    // Файл меньше, чем записано в заголовке: докачан не до конца (сравнение не проверено на всех PKG).
    // Торрент-клиент создаёт файл сразу полного размера, а непришедшие куски остаются нулями.
    // Поэтому ещё берём пробы из разных мест файла (pkg_probe_zeros); нулевых проб много - файл не докачан.
    int zero_blocks = 0;   // сколько проб из набора оказалось сплошь из нулей
    bool incomplete() const { return (pkg_size > 0 && file_size < pkg_size) || zero_blocks >= 6; }   // у целого PKG бывают 1-5 нулевых блоков (выравнивание); у недокачанного торрента нулей много
    // из param.sfo внутри PKG (необязательно: пусто, если прочитать не удалось)
    std::string title;       // TITLE
    std::string app_ver;     // APP_VER, например "01.04"
    int ver_num = -1;        // 01.04 -> 104, -1 если неизвестно
    // признаки по имени файла и папки (см. name_tags)
    bool backport = false;   // Backport / Back Port / BP
    bool rus = false;        // RUS / RU / Russian / русификатор
    bool dlc_name = false;   // слово DLC в имени
};

// TITLE из обычного файла param.sfo (у установленной игры). false, если файла нет или он не разобран.
bool pkg_sfo_title_from_file(const std::string& path, std::string* title);

// Ищет признаки в имени файла и имени его папки (по целым словам, регистр не важен).
void pkg_apply_name_tags(PkgInfo* p);

// Один пакет, который не будет установлен, и причина (для ответа службы).
struct SkippedPkg {
    PkgInfo pkg;
    std::string reason;
};

// Решает, что и в каком порядке ставить:
//  - backport пропускается, если прошивка выше 9 (или неизвестна, fw_major < 0);
//  - из нескольких обычных патчей одной игры остаётся самый новый (по APP_VER, если он известен у всех);
//  - порядок: самая большая игра первой; у каждой игры сразу за ней патчи, потом DLC, потом
//    русификаторы (по имени); внутри группы от большего файла к меньшему.
std::vector<PkgInfo> plan_install(const std::vector<PkgInfo>& in, int fw_major, std::vector<SkippedPkg>* skipped);

// Читает и проверяет заголовок. При ошибке возвращает false и текст причины.
// Читает пробы по 4 КиБ из ~48 мест файла и считает, сколько из них целиком нулевые.
// Результат запоминается по (путь, размер, время изменения). Маленькие файлы (<8 МиБ) не проверяются.
int pkg_probe_zeros(const std::string& path, uint64_t size, long mtime);
// Иконка игры (icon0.png) из таблицы файлов PKG: запись с id 0x1200. false, если её нет или файл не читается.
bool pkg_read_icon(const std::string& path, std::vector<uint8_t>* out);
bool pkg_read_header(const std::string& path, PkgInfo* out, std::string* err);

// Разбор уже прочитанных первых байт файла (для тестов и pkg_read_header).
bool pkg_parse_header(const uint8_t* buf, size_t len, PkgInfo* out, std::string* err);

// Путь допустим для установки: только /data/pkg/... и /mnt/usbN/..., без "..", расширение .pkg.
bool path_allowed(const std::string& path);

// Путь, который понимает системная установка: /data/... -> /user/data/...
std::string bgft_path(const std::string& path);

// Ищет .pkg в /data/pkg и /mnt/usb0../mnt/usb9, читает заголовки.
std::vector<PkgInfo> scan_packages(size_t max_files);

// Просто по размеру, от большего к меньшему (для списка /scan).
void sort_by_size(std::vector<PkgInfo>* list);

// Корневые папки, которые просматривает scan_packages().
std::vector<std::string> pkg_roots();

// Подключённые флешки: /mnt/usbN, где есть отдельное монтирование (st_dev отличается от /mnt) или в папке что-то лежит.
std::vector<std::string> usb_mounts();

// Записывает текстовый файл name (только Имя_magnet.txt, без папок) в корень флешки dir (/mnt/usbN из usb_mounts()).
bool write_magnet_file(const std::string& dir, const std::string& name, const std::string& text, std::string* err);
