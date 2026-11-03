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

// Спрашивает /health. true, если служба ответила; version = её версия.
bool service_query(std::string& version);

// Отправляет ELF службы из PKG (/app0/assets/misc/ps4installerd.elf) в BinLoader GoldHEN (127.0.0.1:9090).
// Если старая копия ещё работает, новая сама попросит её завершиться.
// connectFailed (необязателен): true, если не удалось именно подключиться к порту (BinLoader выключен).
bool service_send_payload(std::string& err, bool* connectFailed = nullptr);

// Копирует ELF в /data/payloads/ps4installerd.elf и один раз прописывает его в
// /data/GoldHEN/payloads.ini (секция [AutoRun], путь с /user): тогда GoldHEN сам запускает
// службу после включения консоли. Если строка уже есть (включена или выключена пользователем), файл не трогаем.
bool service_install_autorun(std::string& err);
