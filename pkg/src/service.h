#pragma once
#include <stdint.h>
#include <string>
#include <vector>

// Служба ps4installerd (payload) живёт отдельно от приложения; приложение говорит с ней по HTTP.
#define SERVICE_HOST "127.0.0.1"
#define SERVICE_PORT 8284

// Один найденный на диске файл PKG (разбор ответа /scan).
struct PkgItem {
    std::string path, content_id, title_id, kind, title, ver;
    uint64_t size = 0;
    bool patch = false, backport = false, rus = false, dlc_name = false;
};

// GET /scan. false и текст в err, если служба не ответила или ответ непонятен.
bool service_scan(std::vector<PkgItem>& out, std::string& err);
