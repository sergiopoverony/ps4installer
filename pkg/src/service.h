#pragma once
#include <stdint.h>
#include <string>
#include <utility>
#include <vector>

// Служба ps4installerd (payload) живёт отдельно от приложения; приложение говорит с ней по HTTP.
#define SERVICE_HOST "127.0.0.1"
#define SERVICE_PORT 8284

// Один найденный на диске файл PKG (разбор ответа /scan).
struct PkgItem {
    std::string path, content_id, title_id, kind, title, ver;
    uint64_t size = 0;
    bool patch = false, backport = false, rus = false, dlc_name = false, incomplete = false;
};

// GET /scan. false и текст в err, если служба не ответила или ответ непонятен.
bool service_scan(std::vector<PkgItem>& out, std::string& err, bool fresh = false);

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

// Версия прошивки (основная часть, 13 для 13.02) из /health, -1 если не удалось.
int service_fw_major();

// Свободное место на диске консоли (GET /free).
bool service_free(uint64_t& bytes, uint64_t* total = nullptr);   // total - полный размер диска (0, если служба старая)
// Иконка игры (icon0.png) из PKG через службу (GET /icon). false, если её нет.
bool service_icon(const std::string& pkgPath, std::string& png);
bool service_installed(std::vector<std::string>& ids);   // title_id установленных игр
// Установленные игры с названиями (GET /installed_games): пары (title_id, название; название может быть пустым).
bool service_installed_games(std::vector<std::pair<std::string, std::string> >& out);
// Иконка установленной игры (GET /appicon). false, если её нет.
bool service_app_icon(const std::string& titleId, std::string& png);
// POST /launch: запуск установленной игры. В err причина отказа.
bool service_launch(const std::string& titleId, std::string& err);
// POST /uninstall: удалить установленную игру. В err причина отказа.
bool service_uninstall(const std::string& titleId, std::string& err);

// POST /install. true, если служба приняла очередь. При отказе в err текст причины (detail службы).
// skipped: что служба решила не ставить (backport, старые патчи) и почему.
bool service_install(const std::vector<std::string>& paths, std::string& err, std::vector<std::string>* skipped = nullptr);

// Ход установки (GET /status).
struct InstallJob {
    std::string path, kind, status, message;
    uint64_t size = 0, length = 0, transferred = 0;
};
struct InstallStatus {
    bool running = false;
    std::vector<InstallJob> jobs;
};
bool service_status(InstallStatus& out);
