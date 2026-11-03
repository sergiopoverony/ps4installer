#include "http.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
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
    snprintf(num, sizeof(num), "\"size\":%llu,", (unsigned long long)p.file_size);
    return "{\"path\":\"" + json_escape(p.path) + "\",\"content_id\":\"" + p.content_id + "\",\"title_id\":\"" +
           p.title_id + "\",\"kind\":\"" + p.kind + "\"," + num + "\"patch\":" + (p.is_patch ? "true" : "false") + "}";
}

Response route(const Request& rq) {
    if (rq.path != "/status" && rq.path != "/health") {  // опрос состояния не засоряет журнал
        log_printf("http: %s %s (тело %d байт)", rq.method.c_str(), rq.path.c_str(), (int)rq.body.size());
    }
    Response r;
    if (rq.path == "/health") {
        std::string berr;
        platform::bgft_load(&berr);  // пробуем загрузить заранее, чтобы ошибка была видна сразу
        r.body = std::string("{\"ok\":true,\"version\":\"" APP_VERSION "\",\"bgft\":\"") +
                 json_escape(berr.empty() ? platform::bgft_info() : "error: " + berr) + "\"}";
        return r;
    }
    if (rq.path == "/scan") {
        std::vector<PkgInfo> list = scan_packages(3000);
        sort_for_install(&list);
        r.body = "{\"ok\":true,\"count\":" + std::to_string(list.size()) + ",\"packages\":[";
        for (size_t i = 0; i < list.size(); i++) {
            r.body += (i ? "," : "") + pkg_json(list[i]);
        }
        r.body += "]}";
        return r;
    }
    if (rq.path == "/free") {
        uint64_t b = 0;
        bool ok = platform::free_bytes("/user", &b);
        r.body = std::string("{\"ok\":") + (ok ? "true" : "false") + ",\"free_bytes\":" + std::to_string((unsigned long long)b) + "}";
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
        r.body = "{\"ok\":true,\"queued\":" + std::to_string(paths.size()) + ",\"order\":" + e.order_json +
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
        ssize_t n = send(fd, s.data() + off, s.size() - off, 0);
        if (n <= 0) {
            return;
        }
        off += (size_t)n;
    }
}

void handle_client(int fd) {
    struct timeval tv;
    tv.tv_sec = 10;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    Request rq;
    if (read_request(fd, &rq)) {
        Response r = route(rq);
        std::string out = "HTTP/1.1 " + std::to_string(r.code) + " " + reason(r.code) +
                          "\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: " +
                          std::to_string(r.body.size()) + "\r\nConnection: close\r\n\r\n" + r.body;
        send_all(fd, out);
    }
    close(fd);
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
        std::thread(handle_client, c).detach();
    }
    close(fd);
    log_printf("служба завершается");
    return 0;
}
