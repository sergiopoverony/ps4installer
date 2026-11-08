// Сеть для payload на ps4-payload-sdk (FreeBSD-совместимые сокеты).
// Отличия от версии для OpenOrbis: DNS делает системная библиотека SDK (getaddrinfo),
// SO_NBIO и ручная проверка errno == 36 не нужны (EINPROGRESS здесь и есть 36).
#include "net.h"
#include "log.h"
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/select.h>
#include <arpa/inet.h>
#include <netdb.h>

bool net_init() { return true; }   // сеть инициализирует библиотека SDK при первом обращении

static void fill_addr(struct sockaddr_in& a, struct in_addr ip, int port) {
    memset(&a, 0, sizeof(a));
#ifndef __linux__
    a.sin_len = sizeof(a);
#endif
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    a.sin_addr = ip;
}

// Свой запрос A-записи по UDP: системный getaddrinfo в приложении иногда отвечает ошибкой (-11), хотя интернет есть.
bool net_resolve_dns(const char* host, const char* server, int port, struct in_addr* out, int timeout_ms) {
    unsigned char q[300];
    size_t n = 0;
    q[n++] = 0x5a; q[n++] = 0x17;   // id
    q[n++] = 0x01; q[n++] = 0x00;   // рекурсия желательна
    q[n++] = 0; q[n++] = 1; q[n++] = 0; q[n++] = 0; q[n++] = 0; q[n++] = 0; q[n++] = 0; q[n++] = 0;
    const char* p = host;
    while (*p) {
        const char* dot = strchr(p, '.');
        size_t len = dot ? (size_t)(dot - p) : strlen(p);
        if (len == 0 || len > 63 || n + len + 6 > sizeof(q)) return false;
        q[n++] = (unsigned char)len;
        memcpy(q + n, p, len); n += len;
        p += len + (dot ? 1 : 0);
        if (!dot) break;
    }
    q[n++] = 0;
    q[n++] = 0; q[n++] = 1; q[n++] = 0; q[n++] = 1;   // тип A, класс IN

    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return false;
    struct sockaddr_in a;
    struct in_addr sip;
    sip.s_addr = inet_addr(server);
    fill_addr(a, sip, port);
    bool ok = false;
    if (sendto(s, q, n, 0, (struct sockaddr*)&a, sizeof(a)) == (ssize_t)n) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(s, &rf);
        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        if (select(s + 1, &rf, NULL, NULL, &tv) > 0) {
            unsigned char r[512];
            ssize_t m = recv(s, r, sizeof(r), 0);
            if (m > 12 && r[0] == q[0] && r[1] == q[1] && (r[3] & 0x0f) == 0) {
                int an = (r[6] << 8) | r[7];
                size_t i = 12;
                while (i < (size_t)m && r[i] != 0) i += (r[i] & 0xc0) == 0xc0 ? 2 : (size_t)r[i] + 1;   // имя вопроса
                if (i < (size_t)m && r[i] == 0) i += 1;
                i += 4;
                for (int k = 0; k < an && i + 12 <= (size_t)m; k++) {
                    while (i < (size_t)m && r[i] != 0 && (r[i] & 0xc0) != 0xc0) i += (size_t)r[i] + 1;
                    i += (i < (size_t)m && (r[i] & 0xc0) == 0xc0) ? 2 : 1;
                    if (i + 10 > (size_t)m) break;
                    int type = (r[i] << 8) | r[i + 1], rdlen = (r[i + 8] << 8) | r[i + 9];
                    i += 10;
                    if (type == 1 && rdlen == 4 && i + 4 <= (size_t)m) { memcpy(&out->s_addr, r + i, 4); ok = true; break; }
                    i += (size_t)rdlen;
                }
            }
        }
    }
    close(s);
    return ok;
}

bool net_resolve(const char* host, struct in_addr* out) {
    in_addr_t lit = inet_addr(host);
    if (lit != (in_addr_t)-1) { out->s_addr = lit; return true; }

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    int rc = getaddrinfo(host, NULL, &hints, &res);
    if (rc == 0 && res) {
        *out = ((struct sockaddr_in*)res->ai_addr)->sin_addr;
        freeaddrinfo(res);
        return true;
    }
    logf_("resolve %s failed rc=%d, пробуем DNS напрямую", host, rc);
    static const char* servers[] = { "8.8.8.8", "1.1.1.1", "77.88.8.8" };
    for (size_t i = 0; i < sizeof(servers) / sizeof(servers[0]); i++) {
        if (net_resolve_dns(host, servers[i], 53, out, 1500)) {
            logf_("resolve %s: DNS %s -> %s", host, servers[i], inet_ntoa(*out));
            return true;
        }
    }
    logf_("resolve %s: DNS напрямую тоже не ответил", host);
    return false;
}

int net_connect(const char* host, int port, int timeout_ms) {
    struct in_addr ip;
    if (!net_resolve(host, &ip)) return -1;

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { logf_("socket failed errno=%d", errno); return -1; }

    int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, fl | O_NONBLOCK);

    struct sockaddr_in a;
    fill_addr(a, ip, port);

    int r = connect(s, (struct sockaddr*)&a, sizeof(a));
    if (r < 0 && errno != EINPROGRESS) {
        logf_("connect %s:%d failed errno=%d", host, port, errno);
        close(s);
        return -1;
    }
    if (r < 0) {
        fd_set wf;
        FD_ZERO(&wf);
        FD_SET(s, &wf);
        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        r = select(s + 1, NULL, &wf, NULL, &tv);
        if (r <= 0) {
            logf_("connect %s:%d timeout", host, port);
            close(s);
            return -1;
        }
        int err = 0;
        socklen_t len = sizeof(err);
        getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len);
        if (err) {
            logf_("connect %s:%d failed so_error=%d", host, port, err);
            close(s);
            return -1;
        }
    }

    fcntl(s, F_SETFL, fl);   // обратно в блокирующий режим
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    return s;
}

int net_connect_nb(const char* ip, int port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;

    int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, fl | O_NONBLOCK);
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif

    struct in_addr addr;
    addr.s_addr = inet_addr(ip);
    struct sockaddr_in a;
    fill_addr(a, addr, port);

    int r = connect(s, (struct sockaddr*)&a, sizeof(a));
    if (r < 0 && errno != EINPROGRESS) { int e = errno; close(s); errno = e; return -1; }
    return s;
}
