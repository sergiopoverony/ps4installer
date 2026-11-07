#pragma once
// Запросы игр через торрент-клиент ps4torrent (тот же консоль, его папка /data/pkg/torrents и страница 127.0.0.1:8787).
// Запрос = файл <хэш>.magnet в папке торрентов: ps4torrent сам подхватит его, найдёт раздающих, получит описание раздачи
// (даже если сети сейчас нет - будет ждать) и начнёт загрузку. Здесь нет ничего консольного - можно проверять на ПК.
#include <map>
#include <string>
#include <vector>

// Пути и адрес клиента (по умолчанию как на консоли); меняются только в проверках.
void torrents_configure(const std::string& root, const std::string& host, int port);

// Ссылка magnet: хэш + трекер рутрекера + название.
std::string torrents_magnet(const std::string& hash, const std::string& title);

// Ставит запрос: создаёт папку торрентов (если её нет) и файл <хэш>.magnet. Если файл уже есть, это не ошибка.
// false и текст ошибки в err (негодный хэш или не удалось записать).
bool torrents_request(const std::string& hash, const std::string& title, std::string& err);

// Ожидает ли запрос (файл <хэш>.magnet ещё лежит: описания раздачи пока нет).
bool torrents_pending(const std::string& hash);

// Отмена ожидающего запроса (удаляет файл <хэш>.magnet). Когда раздача уже получена и стоит в ps4torrent, она удаляется
// только самим ps4torrent (из списка или вместе с файлами); здесь её не трогаем.
void torrents_cancel(const std::string& hash);

enum ReqState { RS_WAIT = 0, RS_QUEUED, RS_DOWNLOADING, RS_PAUSED, RS_DONE, RS_GONE, RS_NOSVC };
struct ReqStatus { ReqState state = RS_GONE; int pct = 0; };

// Состояние запросов: один вызов /status у ps4torrent на все хэши. Ключ результата - хэш в нижнем регистре.
std::map<std::string, ReqStatus> torrents_poll(const std::vector<std::string>& hashes);

// Хэш в нижний регистр; "" если это не 40 символов hex.
std::string torrents_clean_hash(const std::string& h);

// То же, но без лишних запросов: онлайн-результат помним 20 секунд; офлайн - 10 секунд для окна игры (background=false)
// и всё дольше для фоновых проверок (background=true): 15 с, 30 с, 1 мин ... до 10 минут, пока связи нет. Появилась связь - счётчик сбрасывается.
bool net_online_cached(int timeout_ms, bool background);

// Отвечает ли ps4torrent (запущен ли он сейчас).
bool torrents_running(int timeout_ms);

// Есть ли выход в интернет (пробное подключение к нескольким публичным адресам).
bool net_online(int timeout_ms);

// Имя файла со ссылкой: Имя_игры_ID_magnet.txt (только латиница, цифры, '_', '-', '.').
std::string torrents_magnet_filename(const std::string& title, const std::string& id);
