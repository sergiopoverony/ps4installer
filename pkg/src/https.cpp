#include "https.h"
#include "httpc.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>
#ifdef PS4_APP
// Приложение (OpenOrbis): системные модули грузит ядро. dlopen в этой сборке нет.
extern "C" int sceKernelLoadStartModule(const char* name, size_t argc, const void* argv, unsigned int flags, void* opt, int* res);
extern "C" int sceKernelDlsym(int handle, const char* symbol, void** addr);
#else
#include <dlfcn.h>
#endif
#include <vector>

// ---------------------------------------------------------------- http://
static bool split_url(const std::string& url, std::string& scheme, std::string& host, int& port, std::string& path)
{
    size_t p = url.find("://");
    if (p == std::string::npos) return false;
    scheme = url.substr(0, p);
    size_t h = p + 3, s = url.find('/', h);
    std::string hp = url.substr(h, s == std::string::npos ? std::string::npos : s - h);
    path = s == std::string::npos ? "/" : url.substr(s);
    size_t c = hp.find(':');
    host = c == std::string::npos ? hp : hp.substr(0, c);
    port = c == std::string::npos ? (scheme == "https" ? 443 : 80) : atoi(hp.substr(c + 1).c_str());
    return !host.empty() && port > 0;
}

static bool get_plain(const std::string& host, int port, const std::string& path, std::string& body, size_t maxBytes, std::string& err)
{
    HttpResult r = http_get(host, port, path, 30000);
    if (!r.connected) { err = "нет соединения с " + host; return false; }
    if (r.status != 200) { err = "ответ " + std::to_string(r.status); return false; }
    if (r.body.size() > maxBytes) { err = "файл больше допустимого"; return false; }
    body = r.body;
    return true;
}

// ---------------------------------------------------------------- https:// через системные библиотеки
typedef int (*fn_net_pool_create)(const char*, int, int);
typedef int (*fn_ssl_init)(size_t);
typedef int (*fn_http_init)(int, int, size_t);
typedef int (*fn_http_tmpl)(int, const char*, int, int);
typedef int (*fn_http_conn)(int, const char*, int);
typedef int (*fn_http_req)(int, int, const char*, unsigned long long);
typedef int (*fn_http_set)(int, int);
typedef int (*fn_http_send)(int, const void*, size_t);
typedef int (*fn_http_status)(int, int*);
typedef int (*fn_http_read)(int, void*, size_t);
typedef int (*fn_http_del)(int);

struct SysHttp {
    bool tried = false, ok = false;
    std::string why;
    fn_net_pool_create netPoolCreate = nullptr;
    fn_ssl_init sslInit = nullptr;
    fn_http_init httpInit = nullptr;
    fn_http_tmpl createTemplate = nullptr;
    fn_http_conn createConn = nullptr;
    fn_http_req createReq = nullptr;
    fn_http_set setAutoRedirect = nullptr;
    fn_http_send sendReq = nullptr;
    fn_http_status getStatus = nullptr;
    fn_http_read readData = nullptr;
    fn_http_del delReq = nullptr, delConn = nullptr, delTmpl = nullptr;
    int ctx = -1;
};
static SysHttp g_sys;

// Модуль -> «ручка» (указатель или номер модуля, зависит от сборки); 0 - не загрузился.
typedef long LibH;

static LibH open_lib(const char* name)
{
    static const char* dirs[] = {"/system/common/lib/", "/system/priv/lib/", "/system_ex/common_ex/lib/", "/system_ex/priv_ex/lib/"};
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        std::string p = std::string(dirs[i]) + name;
#ifdef PS4_APP
        int h = sceKernelLoadStartModule(p.c_str(), 0, nullptr, 0, nullptr, nullptr);
        if (h > 0) { logf_("https: %s загружена (модуль %d)", p.c_str(), h); return (LibH)h; }
#else
        void* h = dlopen(p.c_str(), RTLD_NOW);
        if (h) { logf_("https: %s загружена", p.c_str()); return (LibH)h; }
#endif
    }
    logf_("https: не удалось загрузить %s", name);
    return 0;
}

static void* lib_sym(LibH h, const char* name)
{
#ifdef PS4_APP
    void* a = nullptr;
    return sceKernelDlsym((int)h, name, &a) == 0 ? a : nullptr;
#else
    return dlsym((void*)h, name);
#endif
}

#define SYM(var, lib, name) do { var = (decltype(var))lib_sym(lib, name); if (!var) { g_sys.why = std::string("нет функции ") + name; logf_("https: %s", g_sys.why.c_str()); return false; } } while (0)

static bool sys_init()
{
    if (g_sys.tried) return g_sys.ok;
    g_sys.tried = true;
    LibH net = open_lib("libSceNet.sprx");
    LibH ssl = open_lib("libSceSsl.sprx");
    LibH http = open_lib("libSceHttp.sprx");
    if (!net || !ssl || !http) { g_sys.why = "системные библиотеки сети не загрузились"; return false; }
    SYM(g_sys.netPoolCreate, net, "sceNetPoolCreate");
    SYM(g_sys.sslInit, ssl, "sceSslInit");
    SYM(g_sys.httpInit, http, "sceHttpInit");
    SYM(g_sys.createTemplate, http, "sceHttpCreateTemplate");
    SYM(g_sys.createConn, http, "sceHttpCreateConnectionWithURL");
    SYM(g_sys.createReq, http, "sceHttpCreateRequestWithURL");
    SYM(g_sys.setAutoRedirect, http, "sceHttpSetAutoRedirect");
    SYM(g_sys.sendReq, http, "sceHttpSendRequest");
    SYM(g_sys.getStatus, http, "sceHttpGetStatusCode");
    SYM(g_sys.readData, http, "sceHttpReadData");
    SYM(g_sys.delReq, http, "sceHttpDeleteRequest");
    SYM(g_sys.delConn, http, "sceHttpDeleteConnection");
    SYM(g_sys.delTmpl, http, "sceHttpDeleteTemplate");
    int pool = g_sys.netPoolCreate("ps4installer_http", 16 * 1024, 0);
    int sslCtx = g_sys.sslInit(256 * 1024);
    logf_("https: sceNetPoolCreate=%d sceSslInit=%d", pool, sslCtx);
    if (pool < 0 || sslCtx < 0) { g_sys.why = "не удалось запустить сеть/SSL (pool=" + std::to_string(pool) + " ssl=" + std::to_string(sslCtx) + ")"; return false; }
    g_sys.ctx = g_sys.httpInit(pool, sslCtx, 64 * 1024);
    logf_("https: sceHttpInit=%d", g_sys.ctx);
    if (g_sys.ctx < 0) { g_sys.why = "sceHttpInit: " + std::to_string(g_sys.ctx); return false; }
    g_sys.ok = true;
    return true;
}

static bool get_sys(const std::string& url, std::string& body, size_t maxBytes, std::string& err)
{
    if (!sys_init()) { err = g_sys.why.empty() ? "системный HTTPS недоступен" : g_sys.why; return false; }
    bool ok = false;
    int tmpl = g_sys.createTemplate(g_sys.ctx, "ps4installer/1.0", 2, 0);   // 2 - HTTP/1.1
    logf_("https: шаблон=%d", tmpl);
    int conn = -1, req = -1;
    if (tmpl < 0) err = "sceHttpCreateTemplate: " + std::to_string(tmpl);
    else {
        g_sys.setAutoRedirect(tmpl, 1);   // GitHub отдаёт файл через перенаправление на другой адрес
        conn = g_sys.createConn(tmpl, url.c_str(), 1);
        logf_("https: соединение=%d", conn);
        if (conn < 0) err = "sceHttpCreateConnectionWithURL: " + std::to_string(conn);
    }
    if (conn >= 0) {
        req = g_sys.createReq(conn, 0, url.c_str(), 0);   // 0 - GET
        logf_("https: запрос=%d", req);
        if (req < 0) err = "sceHttpCreateRequestWithURL: " + std::to_string(req);
    }
    if (req >= 0) {
        g_sys.setAutoRedirect(req, 1);
        int rc = g_sys.sendReq(req, nullptr, 0);   // здесь идёт рукопожатие TLS и проверка сертификата
        int code = 0;
        if (rc < 0) { char b[32]; snprintf(b, sizeof(b), "0x%08X", (unsigned)rc); err = std::string("sceHttpSendRequest: ") + b + " (рукопожатие или сертификат)"; }
        else if (g_sys.getStatus(req, &code) < 0 || code != 200) err = "ответ " + std::to_string(code);
        else {
            std::vector<char> buf(65536);
            body.clear();
            ok = true;
            for (;;) {
                int n = g_sys.readData(req, buf.data(), buf.size());
                if (n < 0) { err = "sceHttpReadData: " + std::to_string(n); ok = false; break; }
                if (n == 0) break;
                body.append(buf.data(), (size_t)n);
                if (body.size() > maxBytes) { err = "файл больше допустимого"; ok = false; break; }
            }
        }
        logf_("https: sceHttpSendRequest=%d, ответ %d, получено %d байт%s%s", rc, code, (int)body.size(), ok ? "" : ", ошибка: ", ok ? "" : err.c_str());
    }
    if (req >= 0) g_sys.delReq(req);
    if (conn >= 0) g_sys.delConn(conn);
    if (tmpl >= 0) g_sys.delTmpl(tmpl);
    return ok;
}

bool https_get(const std::string& url, std::string& body, size_t maxBytes, std::string& err)
{
    std::string scheme, host, path;
    int port = 0;
    if (!split_url(url, scheme, host, port, path)) { err = "неверный адрес"; return false; }
    if (scheme == "http") return get_plain(host, port, path, body, maxBytes, err);
    if (scheme == "https") return get_sys(url, body, maxBytes, err);
    err = "неподдерживаемый адрес";
    return false;
}
