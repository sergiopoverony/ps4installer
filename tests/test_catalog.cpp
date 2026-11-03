// Проверка чтения каталога: tests/test_catalog.py готовит папку с catalog.json/catalog.pak и передаёт путь.
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include "catalog.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

int main(int argc, char** argv)
{
    if (argc < 2) return 2;
    CHECK(catalog_load(argv[1]));
    CHECK(catalog_count() == 2);
    const CatEntry* e = catalog_by_title_id("CUSA18535");
    CHECK(e != nullptr);
    CHECK(e->name_en == "Test Game");
    CHECK(e->name_ru == "\xD0\xA2\xD0\xB5\xD1\x81\xD1\x82");   // «Тест»
    CHECK(e->shots.size() == 2);
    CHECK(catalog_by_title_id("CUSA99999") == nullptr);
    std::string t = catalog_text(*e);
    CHECK(t == "Line one line two");
    // поиск по названию: без регистра и знаков; русские буквы тоже
    CHECK(catalog_by_name("TEST game\xE2\x84\xA2!") == e);
    CHECK(catalog_by_name("\xD0\xA2\xD0\x95\xD0\xA1\xD0\xA2") == e);   // «ТЕСТ»
    CHECK(catalog_by_name("Nothing here") == nullptr);
    CHECK(catalog_by_name("Test") == nullptr);                 // слишком коротко для неточного поиска
    CHECK(catalog_by_name("Test Game: Deluxe Edition") == nullptr);   // наше название длиннее каталожного - не берём
    const char* how = "";
    CHECK(catalog_find("CUSA00000", "test game", &how) == e);
    CHECK(std::string(how) == "name");
    CHECK(catalog_find("CUSA18535", "whatever", &how) == e);
    CHECK(std::string(how) == "id");
    CatImage img;
    CHECK(catalog_image(e->cover, 120, 80, img));
    CHECK(img.ok() && img.w == 120 && img.h == 80);
    // обложка красная слева и синяя справа: проверяем цвета после масштаба
    uint32_t l = img.px[40 * 120 + 5], r = img.px[40 * 120 + 114];
    CHECK(((l >> 16) & 0xFF) > 200 && (l & 0xFF) < 80);
    CHECK((r & 0xFF) > 200 && ((r >> 16) & 0xFF) < 80);
    CatImage bad;
    CHECK(!catalog_image(e->desc, 50, 50, bad));   // текст - не картинка
    CHECK(!catalog_image(CatRef(), 50, 50, bad));
    printf("ok\n");
    return 0;
}
