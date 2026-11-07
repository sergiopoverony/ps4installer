#pragma once
// Заплатки каталога из интернета (catalog_N.json + catalog_N.pak). ЗАГЛУШКА: адрес для загрузки пока не выбран.
// Заплатки можно положить на консоль и вручную: /data/ps4installer_app/catalog/ (приложение применяет их при запуске, см. catalog_load).
#include <string>

enum PatchCheck { PC_NOT_CONFIGURED = 0, PC_OFFLINE, PC_UP_TO_DATE, PC_DOWNLOADED };

// Откуда качать: пусто - загрузка выключена (так сейчас).
extern const char* PATCH_URL;

// Проверяет, есть ли в интернете заплатка новее применённой (номер lastApplied), и скачивает её.
// Пока PATCH_URL пуст, ничего не делает и в сеть не ходит. Когда адрес появится: звать из фонового потока,
// связь проверять через net_online_cached(..., true), чтобы без интернета запросы не повторялись.
PatchCheck patches_check_online(int lastApplied, std::string& info);
