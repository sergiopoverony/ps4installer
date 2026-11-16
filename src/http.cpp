// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SergioPoverony
#include "http.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "installer.h"
#include "json.h"
#include "log.h"
#include "pkg.h"
#include "platform.h"
#include "version.h"

namespace {

std::atomic<bool> g_scan_dirty(false);   // после удаления файлов кэш /scan устарел

std::atomic<bool> g_quit(false);
int g_listen_fd = -1;

struct Request {
    std::string method;
    std::string path;   // без query
    std::string query;  // после '?'
    std::string body;
};

struct Response {
    int code = 200;
    std::string body;
    std::string content_type = "application/json; charset=utf-8";
};

const char* reason(int code) {
    switch (code) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 422: return "Unprocessable Entity";
        default: return "Error";
    }
}

Response json_error(int code, const std::string& error, const std::string& detail) {
    Response r;
    r.code = code;
    r.body = "{\"ok\":false,\"error\":\"" + json_escape(error) + "\",\"detail\":\"" + json_escape(detail) + "\"}";
    return r;
}

std::string pkg_json(const PkgInfo& p) {
    char num[96];
    snprintf(num, sizeof(num), "\"size\":%llu,\"pkg_size\":%llu,\"zero_blocks\":%d,", (unsigned long long)p.file_size, (unsigned long long)p.pkg_size, p.zero_blocks);
    return "{\"path\":\"" + json_escape(p.path) + "\",\"content_id\":\"" + p.content_id + "\",\"title_id\":\"" +
           p.title_id + "\",\"kind\":\"" + p.kind + "\"," + num + "\"patch\":" + (p.is_patch ? "true" : "false") + ",\"title\":\"" + json_escape(p.title) +
           "\",\"ver\":\"" + json_escape(p.app_ver) + "\",\"backport\":" + (p.backport ? "true" : "false") +
           ",\"incomplete\":" + (p.incomplete() ? "true" : "false") + ",\"rus\":" + (p.rus ? "true" : "false") + ",\"dlc_name\":" + (p.dlc_name ? "true" : "false") + "}";
}

std::string url_decode(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() + 0 && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
            o += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else {
            o += s[i];
        }
    }
    return o;
}

Response route(const Request& rq) {
    if (rq.path != "/status" && rq.path != "/health") {  // опрос состояния не засоряет журнал
        log_printf("http: %s %s (тело %d байт)", rq.method.c_str(), rq.path.c_str(), (int)rq.body.size());
    }
    {
        std::string ctx = rq.method + " " + rq.path + (rq.query.empty() ? "" : "?" + rq.query.substr(0, 120));
        log_set_context(ctx.c_str());
    }
    Response r;
    if (rq.path == "/health") {
        std::string berr;
        platform::bgft_load(&berr);  // пробуем загрузить заранее, чтобы ошибка была видна сразу
        std::string fw;
        platform::firmware_major(&fw);
        r.body = std::string("{\"ok\":true,\"version\":\"" APP_VERSION "\",\"fw\":\"") + json_escape(fw) +
                 "\",\"bgft\":\"" + json_escape(berr.empty() ? platform::bgft_info() : "error: " + berr) + "\"}";
        return r;
    }
    if (rq.path == "/scan") {
        // Сканирование тяжёлое (читает флешки), поэтому: одновременно идёт только одно, а ответ
        // запоминается на 30 секунд. Принудительное обновление: /scan?fresh=1 (кнопка «Квадрат»).
        static std::mutex scan_mu;
        static std::string cached;
        static std::chrono::steady_clock::time_point cached_at;
        bool fresh = rq.query.find("fresh=1") != std::string::npos;
        std::lock_guard<std::mutex> lk(scan_mu);
        auto now = std::chrono::steady_clock::now();
        const char* ttl_env = getenv("PS4I_SCAN_CACHE_SEC");   // в тестах 0 (кэша нет)
        int ttl = ttl_env ? atoi(ttl_env) : 30;
        long age = cached.empty() ? 1000000 : (long)std::chrono::duration_cast<std::chrono::seconds>(now - cached_at).count();
        bool usable = ttl > 0 && age < ttl;
        if (g_scan_dirty.exchange(false)) {
            usable = false;   // файлы удалили: список сразу пересобираем
        }
        if (usable && fresh && age >= 3) {
            usable = false;   // «Квадрат»: свежие данные, но не чаще чем раз в 3 секунды
        }
        if (!usable) {
            std::vector<PkgInfo> list = scan_packages(3000);
            sort_by_size(&list);
            cached = "{\"ok\":true,\"count\":" + std::to_string(list.size()) + ",\"packages\":[";
            for (size_t i = 0; i < list.size(); i++) {
                cached += (i ? "," : "") + pkg_json(list[i]);
            }
            cached += "]}";
            cached_at = std::chrono::steady_clock::now();
        }
        r.body = cached;
        return r;
    }
    if (rq.path == "/free") {
        uint64_t b = 0;
        bool ok = platform::free_bytes("/user", &b);
        uint64_t t = 0;
        if (!platform::total_bytes("/user", &t)) t = 0;
        r.body = std::string("{\"ok\":") + (ok ? "true" : "false") + ",\"free_bytes\":" + std::to_string((unsigned long long)b) +
                 ",\"total_bytes\":" + std::to_string((unsigned long long)t) + "}";
        return r;
    }
    if (rq.path == "/usb") {   // подключённые флешки (приложение само в /mnt не заглядывает)
        std::vector<std::string> ms = usb_mounts();
        r.body = "{\"ok\":true,\"roots\":[";
        for (size_t i = 0; i < ms.size(); i++) r.body += (i ? ",\"" : "\"") + json_escape(ms[i]) + "\"";
        r.body += "]}";
        return r;
    }
    if (rq.path == "/write_magnet") {   // сохранить magnet-ссылку файлом Имя_magnet.txt в корень флешки
        if (rq.method != "POST") return json_error(405, "method", "нужен POST");
        std::vector<std::string> a;   // {"args":["/mnt/usb1","Имя_magnet.txt","magnet:?..."]}
        if (!json_get_string_array(rq.body, "args", &a) || a.size() != 3)
            return json_error(400, "bad_request", "ожидается {\"args\":[\"/mnt/usb0\",\"Игра_magnet.txt\",\"magnet:?...\"]}");
        std::string err;
        log_printf("write_magnet request: %s/%s", a[0].c_str(), a[1].c_str());
        if (!write_magnet_file(a[0], a[1], a[2], &err)) return json_error(400, "write_failed", err);
        r.body = "{\"ok\":true}";
        return r;
    }
    if (rq.path == "/installed") {
        std::vector<std::string> ids = platform::installed_title_ids();
        r.body = "{\"ok\":true,\"title_ids\":[";
        for (size_t i = 0; i < ids.size(); i++) r.body += (i ? ",\"" : "\"") + json_escape(ids[i]) + "\"";
        r.body += "]}";
        return r;
    }
    if (rq.path == "/installed_games") {
        std::vector<platform::InstalledGame> gs = platform::installed_games();
        r.body = "{\"ok\":true,\"games\":[";
        for (size_t i = 0; i < gs.size(); i++)
            r.body += std::string(i ? "," : "") + "{\"id\":\"" + json_escape(gs[i].title_id) + "\",\"title\":\"" + json_escape(gs[i].title) + "\",\"loc\":\"" + json_escape(gs[i].loc) + "\"}";
        r.body += "]}";
        return r;
    }
    if (rq.path == "/appicon") {
        size_t k = rq.query.find("id=");
        std::string id = k == std::string::npos ? "" : url_decode(rq.query.substr(k + 3));
        bool known = false;
        for (const std::string& t : platform::installed_title_ids()) known = known || t == id;
        std::string p = known ? platform::app_icon_path(id) : "";
        FILE* f = p.empty() ? nullptr : fopen(p.c_str(), "rb");
        if (!f && known) {   // нет icon0.png рядом: иконка внутри app.pkg (игры на EXT)
            std::string ap = platform::app_pkg_path(id);
            std::vector<uint8_t> icon;
            if (!ap.empty() && pkg_read_icon(ap, &icon)) {
                r.content_type = "image/png";
                r.body.assign((const char*)icon.data(), icon.size());
                return r;
            }
        }
        if (!f) return json_error(404, "no_icon", id);
        std::string data;
        char buf[8192];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && data.size() < (8u << 20)) data.append(buf, n);
        fclose(f);
        r.content_type = "image/png";
        r.body = data;
        return r;
    }
    if (rq.path == "/launch") {
        if (rq.method != "POST") return json_error(405, "method", "нужен POST");
        std::vector<std::string> ids;
        if (!json_get_string_array(rq.body, "title_ids", &ids) || ids.size() != 1)
            return json_error(400, "bad_request", "ожидается {\"title_ids\":[\"CUSA00000\"]}");
        bool known = false;
        for (const std::string& t : platform::installed_title_ids()) known = known || t == ids[0];
        if (!known) return json_error(404, "not_installed", ids[0]);
        std::string err;
        if (!platform::launch_app(ids[0], &err)) return json_error(500, "launch_failed", err);
        r.body = "{\"ok\":true}";
        return r;
    }
    if (rq.path == "/delete_files") {   // физически удалить .pkg-файлы с носителя (только разрешённые папки и только пока нет установки)
        if (rq.method != "POST") return json_error(405, "method", "нужен POST");
        std::vector<std::string> paths;
        if (!json_get_string_array(rq.body, "paths", &paths) || paths.empty() || paths.size() > 200)
            return json_error(400, "bad_request", "ожидается {\"paths\":[\"/mnt/usb0/game.pkg\"]}");
        for (const std::string& p : paths)
            if (!path_allowed(p)) return json_error(403, "path_not_allowed", p);
        if (installer::is_busy()) return json_error(409, "busy", "идёт установка: подождите её окончания");
        int deleted = 0;
        std::string failed;
        for (const std::string& p : paths) {
            log_printf("delete file request: %s", p.c_str());
            if (remove(p.c_str()) == 0) deleted++;
            else failed += std::string(failed.empty() ? "" : ",") + json_escape(p);
        }
        g_scan_dirty = true;
        r.body = "{\"ok\":" + std::string(failed.empty() ? "true" : "false") + ",\"deleted\":" + std::to_string(deleted) + ",\"failed\":\"" + failed + "\"}";
        return r;
    }
    if (rq.path == "/uninstall") {
        if (rq.method != "POST") return json_error(405, "method", "нужен POST");
        std::vector<std::string> ids;
        if (!json_get_string_array(rq.body, "title_ids", &ids) || ids.size() != 1)
            return json_error(400, "bad_request", "ожидается {\"title_ids\":[\"CUSA00000\"]}");
        if (ids[0] == "OFST00001") return json_error(403, "self", "магазин не может удалить сам себя");
        bool known = false;
        for (const std::string& t : platform::installed_title_ids()) known = known || t == ids[0];
        if (!known) return json_error(404, "not_installed", ids[0]);
        if (installer::is_busy()) return json_error(409, "busy", "идёт установка: подождите её окончания");
        std::string err;
        log_printf("uninstall request: %s", ids[0].c_str());
        if (!platform::uninstall_app(ids[0], &err)) return json_error(500, "uninstall_failed", err);
        r.body = "{\"ok\":true}";
        return r;
    }
    if (rq.path == "/icon") {
        size_t k = rq.query.find("path=");
        std::string p = k == std::string::npos ? "" : url_decode(rq.query.substr(k + 5));
        std::vector<uint8_t> icon;
        if (!path_allowed(p) || !pkg_read_icon(p, &icon)) {
            return json_error(404, "no_icon", p);
        }
        r.content_type = "image/png";
        r.body.assign((const char*)icon.data(), icon.size());
        return r;
    }
    if (rq.path == "/status") {
        r.body = installer::status_json();
        return r;
    }
    if (rq.path == "/install") {
        if (rq.method != "POST") {
            return json_error(405, "method", "нужен POST");
        }
        std::vector<std::string> paths;
        if (!json_get_string_array(rq.body, "paths", &paths)) {
            return json_error(400, "bad_request", "ожидается JSON вида {\"paths\":[\"...\"]}");
        }
        installer::EnqueueResult e = installer::enqueue(paths);
        if (!e.ok) {
            r = json_error(e.http_code, e.error, e.detail);
            if (e.error == "no_space") {
                r.body.pop_back();
                r.body += ",\"need\":" + std::to_string((unsigned long long)e.need) + ",\"free\":" +
                          std::to_string((unsigned long long)e.free) + "}";
            }
            return r;
        }
        r.body = "{\"ok\":true,\"queued\":" + std::to_string(paths.size()) + ",\"order\":" + e.order_json + ",\"skipped\":" + e.skipped_json +
                 ",\"need\":" + std::to_string((unsigned long long)e.need) + ",\"free\":" +
                 std::to_string((unsigned long long)e.free) + "}";
        return r;
    }
    if (rq.path == "/cancel") {
        if (rq.method != "POST") {
            return json_error(405, "method", "нужен POST");
        }
        installer::cancel_pending();
        r.body = "{\"ok\":true}";
        return r;
    }
    if (rq.path == "/quit") {
        if (rq.method != "POST") {
            return json_error(405, "method", "нужен POST");
        }
        g_quit = true;
        r.body = "{\"ok\":true}";
        return r;
    }
    return json_error(404, "not_found", rq.path);
}

bool read_request(int fd, Request* rq) {
    std::string data;
    char buf[4096];
    size_t header_end = std::string::npos;
    while (header_end == std::string::npos) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0 || data.size() > 16384) {
            return false;
        }
        data.append(buf, (size_t)n);
        header_end = data.find("\r\n\r\n");
    }
    std::string head = data.substr(0, header_end);
    std::string body = data.substr(header_end + 4);

    size_t sp1 = head.find(' ');
    size_t sp2 = head.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) {
        return false;
    }
    rq->method = head.substr(0, sp1);
    std::string target = head.substr(sp1 + 1, sp2 - sp1 - 1);
    size_t q = target.find('?');
    rq->path = target.substr(0, q);
    rq->query = q == std::string::npos ? "" : target.substr(q + 1);

    size_t content_len = 0;
    size_t cl = head.find("Content-Length:");
    if (cl == std::string::npos) {
        cl = head.find("content-length:");
    }
    if (cl != std::string::npos) {
        content_len = (size_t)strtoul(head.c_str() + cl + 15, nullptr, 10);
    }
    if (content_len > 262144) {
        return false;
    }
    while (body.size() < content_len) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            return false;
        }
        body.append(buf, (size_t)n);
    }
    rq->body = body.substr(0, content_len);
    return true;
}

void send_all(int fd, const std::string& s) {
    size_t off = 0;
    while (off < s.size()) {
        ssize_t n = send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
        if (n <= 0) {
            return;
        }
        off += (size_t)n;
    }
}

std::atomic<int> g_active_clients{0};
const int MAX_CLIENTS = 6;  // больше одновременных запросов службе не нужно

void handle_client(int fd) {
    struct timeval tv;
    tv.tv_sec = 4;  // пустое соединение (браузеры открывают лишние) не должно висеть долго
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    Request rq;
    try {
        if (read_request(fd, &rq)) {
            Response r = route(rq);
            std::string head = "HTTP/1.1 " + std::to_string(r.code) + " " + reason(r.code) +
                               "\r\nContent-Type: " + r.content_type + "\r\nContent-Length: " +
                               std::to_string(r.body.size()) + "\r\nConnection: close\r\n\r\n";
            send_all(fd, head);
            send_all(fd, r.body);   // без лишней копии большого тела (иконки)
        }
    } catch (const std::exception& e) {   // например, не хватило памяти: не роняем всю службу
        log_printf("http: ошибка при обработке %s %s: %s", rq.method.c_str(), rq.path.c_str(), e.what());
    } catch (...) {
        log_printf("http: неизвестная ошибка при обработке %s", rq.path.c_str());
    }
    close(fd);
    g_active_clients--;
}

}  // namespace

void http_request_quit(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return;
    }
    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr*)&a, sizeof(a)) == 0) {
        const char* req = "POST /quit HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        send(fd, req, strlen(req), 0);
        char buf[256];
        recv(fd, buf, sizeof(buf), 0);
        log_printf("старая копия на порту %d попросила завершиться", port);
    }
    close(fd);
}

int http_serve(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        log_printf("socket() не удался");
        return 1;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    int tries = 0;
    while (bind(fd, (struct sockaddr*)&a, sizeof(a)) != 0) {
        if (++tries > 20) {
            log_printf("bind() на порт %d не удался", port);
            close(fd);
            return 1;
        }
        usleep(250 * 1000);  // старая копия ещё закрывается
    }
    if (listen(fd, 8) != 0) {
        log_printf("listen() не удался");
        close(fd);
        return 1;
    }
    g_listen_fd = fd;
    log_printf("HTTP слушает порт %d", port);

    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));  // чтобы accept просыпался и видел флаг выхода
    while (!g_quit) {
        int c = accept(fd, nullptr, nullptr);
        if (c < 0) {
            continue;
        }
        if (g_active_clients >= MAX_CLIENTS) {  // перегрузка (например, быстрые обновления страницы)
            static const char busy[] =
                "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            send(c, busy, sizeof(busy) - 1, 0);
            close(c);
            continue;
        }
        g_active_clients++;
        try {
            std::thread(handle_client, c).detach();
        } catch (const std::exception& e) {  // не удалось создать поток: не падаем
            g_active_clients--;
            log_printf("http: не удалось создать поток: %s", e.what());
            close(c);
        }
    }
    close(fd);
    log_printf("служба завершается");
    return 0;
}
