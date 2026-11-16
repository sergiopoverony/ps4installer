// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SergioPoverony
#pragma once
// Офлайн-каталог игр: data/catalog.json (индекс) + data/catalog.pak (JPEG и тексты по смещениям).
// Оба файла вшиты в PKG: /app0/assets/misc/ (или для проверок /data/ps4installer_app/catalog/). Здесь нет ничего консольного - можно проверять на ПК.
#include <stdint.h>
#include <string>
#include <vector>

struct CatRef { uint64_t off = 0, len = 0; int pak = 0; };   // pak: 0 - основной catalog.pak, N - заплатка catalog_N.pak

struct CatEntry {
    std::string id, cid, title_id;   // title_id вынут из cid (CUSA12345)
    std::string name_en, name_ru;
    CatRef cover, desc;
    std::vector<CatRef> shots;
    // Раздача (tools/merge_torrents.py): хэш (40 hex, пусто - раздачи нет) и пометки. Нет полей - все нули.
    std::string hash;
    int ru = 0, voice = 0, dlc = 0, fix = 0, backport = 0;   // русский интерфейс, русская озвучка, DLC, исправление, Backport
    int size_mb = 0;
    int year = 0;    // год выпуска из описания раздачи (0 - не указан)
};

// Загрузка индекса. dir - папка с catalog.json и catalog.pak. false, если каталога нет (приложение работает и без него).
// patchDir - папка с заплатками catalog_2.json (+ catalog_2.pak), catalog_3.json ... (по умолчанию та же); они применяются по порядку номеров.
bool catalog_load(const std::string& dir, const std::string& patchDir = "");
// Сколько заплаток применено при последней загрузке и номер последней (0 - заплаток нет).
int catalog_patch_count();
int catalog_patch_last();
int catalog_count();
const CatEntry* catalog_by_title_id(const std::string& title_id);
const CatEntry* catalog_at(int i);
// true, если в каталоге есть данные о раздачах (запущен tools/merge_torrents.py): тогда вкладка «Каталог игр» показывает только игры с раздачей.
bool catalog_has_torrents();
// Поиск по названию игры (без регистра, знаков и ™®): запасной вариант, если Title ID в каталоге другого региона.
const CatEntry* catalog_by_name(const std::string& name);
// Сначала по Title ID, потом по названию. what (необязательно) получает "id" / "name".
const CatEntry* catalog_find(const std::string& title_id, const std::string& name, const char** how = nullptr);

// Текст описания (UTF-8, переводы строк заменены пробелами). Пусто, если описания нет.
std::string catalog_text(const CatEntry& e);

// Картинка, уже подогнанная под рамку w*h (масштаб с обрезкой по центру). Пиксели 0x00RRGGBB.
struct CatImage {
    int w = 0, h = 0;
    std::vector<uint32_t> px;
    bool ok() const { return w > 0 && h > 0 && (int)px.size() == w * h; }
};
bool catalog_image(const CatRef& ref, int w, int h, CatImage& out);
// То же для картинки (JPEG или PNG), уже лежащей в памяти (например, иконка из PKG).
// Картинка с прозрачностью (PNG) как есть, без масштабирования. Пиксели 0xAARRGGBB.
bool catalog_decode_rgba(const std::string& blob, int& w, int& h, std::vector<uint32_t>& argb);
bool catalog_image_from_blob(const std::string& blob, int w, int h, CatImage& out);
