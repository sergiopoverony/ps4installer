#pragma once

#include <cstdint>
#include <string>

// Всё, что зависит от консоли. На консоли (payload SDK) вызывает системные функции,
// при сборке с -DHOST_TEST заменено заглушками, чтобы логику можно было проверять на ПК.
namespace platform {

void notify(const std::string& msg);  // всплывающее уведомление на экране консоли
void power_tick();                    // не давать консоли уйти в сон на время установки
bool free_bytes(const char* path, uint64_t* out);

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
