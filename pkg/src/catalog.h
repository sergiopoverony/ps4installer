#pragma once
// Офлайн-каталог игр: data/catalog.json (индекс) + data/catalog.pak (JPEG и тексты по смещениям).
// Оба файла вшиты в PKG: /app0/assets/data/. Здесь нет ничего консольного - можно проверять на ПК.
#include <stdint.h>
#include <string>
#include <vector>

struct CatRef { uint64_t off = 0, len = 0; };

struct CatEntry {
    std::string id, cid, title_id;   // title_id вынут из cid (CUSA12345)
    std::string name_en, name_ru;
    CatRef cover, desc;
    std::vector<CatRef> shots;
};

// Загрузка индекса. dir - папка с catalog.json и catalog.pak. false, если каталога нет (приложение работает и без него).
bool catalog_load(const std::string& dir);
int catalog_count();
const CatEntry* catalog_by_title_id(const std::string& title_id);
const CatEntry* catalog_at(int i);

// Текст описания (UTF-8, переводы строк заменены пробелами). Пусто, если описания нет.
std::string catalog_text(const CatEntry& e);

// Картинка, уже подогнанная под рамку w*h (масштаб с обрезкой по центру). Пиксели 0x00RRGGBB.
struct CatImage {
    int w = 0, h = 0;
    std::vector<uint32_t> px;
    bool ok() const { return w > 0 && h > 0 && (int)px.size() == w * h; }
};
bool catalog_image(const CatRef& ref, int w, int h, CatImage& out);
