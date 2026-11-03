#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Очередь установки. Пакеты ставятся по одному, от самого большого файла к самому маленькому.
// Установку выполняет системная служба консоли (BGFT), служба только регистрирует задачи и следит за ходом.
namespace installer {

struct EnqueueResult {
    bool ok = false;
    int http_code = 200;
    std::string error;       // код ошибки: bad_request | bad_path | bad_pkg | no_space | busy | bgft
    std::string detail;      // пояснение для человека
    std::string order_json;  // JSON-массив путей в порядке установки
    uint64_t need = 0;
    uint64_t free = 0;
};

// Проверяет пути, сортирует (большие вперёд), проверяет свободное место и ставит в очередь.
EnqueueResult enqueue(const std::vector<std::string>& paths);

// Снимает с очереди всё, что ещё не начато (текущая установка продолжается).
void cancel_pending();

// Состояние очереди и последних строк журнала в виде JSON.
std::string status_json();

// Останавливает рабочий поток (при выходе службы).
void shutdown();

// Настройки таймаутов (для тестов).
void set_stall_seconds(int sec);
void set_poll_millis(int ms);

}  // namespace installer
