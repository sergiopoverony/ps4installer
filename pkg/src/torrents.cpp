#include "torrents.h"
#include "httpc.h"
#include "json.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static std::string g_root = "/data/pkg/torrents";
static std::string g_host = "127.0.0.1";
static int g_port = 8787;

void torrents_configure(const std::string& root, const std::string& host, int port)
{
    g_root = root;
    g_host = host;
    g_port = port;
}

std::string torrents_clean_hash(const std::string& h)
{
    if (h.size() != 40) return "";
    std::string o;
    for (size_t i = 0; i < h.size(); i++) {
        char c = h[i];
        if (c >= 'A' && c <= 'F') c = (char)(c - 'A' + 'a');
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return "";
        o += c;
    }
    return o;
}

static std::string url_encode(const std::string& s)
{
    static const char* hx = "0123456789ABCDEF";
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
        else { o += '%'; o += hx[c >> 4]; o += hx[c & 15]; }
    }
    return o;
}

std::string torrents_magnet(const std::string& hash, const std::string& title)
{
    std::string m = "magnet:?xt=urn:btih:" + torrents_clean_hash(hash) + "&tr=" + url_encode("http://bt.t-ru.org/ann?magnet");
    if (!title.empty()) m += "&dn=" + url_encode(title.substr(0, 200));
    return m;
}

static std::string magnet_path(const std::string& hash) { return g_root + "/" + hash + ".magnet"; }

static void make_dirs(const std::string& path)
{
    for (size_t i = 1; i <= path.size(); i++)
        if (i == path.size() || path[i] == '/') mkdir(path.substr(0, i).c_str(), 0777);
}

bool torrents_request(const std::string& hashIn, const std::string& title, std::string& err)
{
    std::string hash = torrents_clean_hash(hashIn);
    if (hash.empty()) { err = "bad hash"; return false; }
    make_dirs(g_root);
    std::string path = magnet_path(hash);
    struct stat sb;
    if (stat(path.c_str(), &sb) == 0) return true;
    std::string tmp = path + ".tmp", line = torrents_magnet(hash, title) + "\n";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) { err = "cannot write to " + g_root; return false; }
    bool w = fwrite(line.data(), 1, line.size(), f) == line.size();
    w = (fclose(f) == 0) && w;
    if (!w || rename(tmp.c_str(), path.c_str()) != 0) { unlink(tmp.c_str()); err = "write failed"; return false; }
    logf_("torrents: запрос %s (%s)", hash.c_str(), title.c_str());
    return true;
}

bool torrents_pending(const std::string& hashIn)
{
    std::string hash = torrents_clean_hash(hashIn);
    struct stat sb;
    return !hash.empty() && stat(magnet_path(hash).c_str(), &sb) == 0;
}

void torrents_cancel(const std::string& hashIn)
{
    std::string hash = torrents_clean_hash(hashIn);
    if (!hash.empty()) unlink(magnet_path(hash).c_str());
}

std::map<std::string, ReqStatus> torrents_poll(const std::vector<std::string>& hashes)
{
    std::map<std::string, ReqStatus> out;
    std::map<std::string, ReqStatus> live;
    bool svc = false;
    HttpResult r = http_get(g_host, g_port, "/status", 2500);
    JsonValue j;
    if (r.connected && r.status == 200 && json_parse(r.body, j)) {
        svc = true;
        const JsonValue& items = j["items"];
        for (size_t i = 0; i < items.arr.size(); i++) {
            const JsonValue& it = items.arr[i];
            ReqStatus s;
            std::string st = it["status"].asStr();
            s.pct = (int)it["pct"].asInt();
            if (st == "complete") { s.state = RS_DONE; s.pct = 100; }
            else if (st == "downloading" || st == "checking") s.state = RS_DOWNLOADING;
            else if (st == "paused") s.state = RS_PAUSED;
            else s.state = RS_QUEUED;
            live[torrents_clean_hash(it["hash"].asStr())] = s;
        }
    }
    for (size_t i = 0; i < hashes.size(); i++) {
        std::string h = torrents_clean_hash(hashes[i]);
        ReqStatus s;
        if (h.empty()) { s.state = RS_GONE; out[hashes[i]] = s; continue; }
        if (torrents_pending(h)) s.state = RS_WAIT;
        else if (live.count(h)) s = live[h];
        else s.state = svc ? RS_GONE : RS_NOSVC;
        out[h] = s;
    }
    return out;
}

bool net_online(int timeout_ms)
{
    static const char* hosts[] = { "1.1.1.1", "8.8.8.8", "9.9.9.9" };
    for (size_t i = 0; i < sizeof(hosts) / sizeof(hosts[0]); i++) {
        HttpResult r = http_get(hosts[i], 80, "/", timeout_ms);
        if (r.connected) return true;
    }
    return false;
}

std::string torrents_magnet_filename(const std::string& title, const std::string& id)
{
    std::string n;
    for (size_t i = 0; i < title.size() && n.size() < 80; i++) {
        unsigned char c = (unsigned char)title[i];
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-' || c == '.') n += (char)c;
        else if (!n.empty() && n[n.size() - 1] != '_') n += '_';
    }
    while (!n.empty() && n[n.size() - 1] == '_') n.erase(n.size() - 1);
    if (n.empty()) n = "game";
    std::string i2;
    for (size_t i = 0; i < id.size(); i++) {
        unsigned char c = (unsigned char)id[i];
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-') i2 += (char)c;
    }
    return n + (i2.empty() ? "" : "_" + i2) + "_magnet.txt";
}

bool torrents_running(int timeout_ms)
{
    return http_get(g_host, g_port, "/status", timeout_ms).connected;
}

static long g_netAt = 0;
static bool g_netLast = false;
static int g_netFails = 0;

bool net_online_cached(int timeout_ms, bool background)
{
    long now = (long)time(NULL);
    long age = now - g_netAt;
    if (g_netAt != 0 && age >= 0) {
        long keep = g_netLast ? 20 : (background ? (15L << (g_netFails > 5 ? 5 : g_netFails)) : 10);
        if (background && !g_netLast && keep > 600) keep = 600;
        if (age < keep) return g_netLast;   // недавно уже проверяли: сеть не трогаем
    }
    bool ok = net_online(timeout_ms);
    g_netAt = now == 0 ? 1 : now;
    g_netLast = ok;
    if (ok) g_netFails = 0; else if (g_netFails < 10) g_netFails++;
    return ok;
}

const char* PEER_API_HOST = "api.t-ru.org";
static std::string g_trHost = "bt.t-ru.org";   // трекер рутрекера (тот же, что в magnet): сюда идёт обычный announce
static int g_trPort = 80;

void torrents_configure_tracker(const std::string& host, int port) { g_trHost = host; g_trPort = port; }

static std::string hex_to_pct(const std::string& hex)   // 40 hex -> %XX%XX... (20 байт info_hash)
{
    std::string o;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) { o += '%'; o += hex[i]; o += hex[i + 1]; }
    return o;
}

// В ответе трекера (bencode) число после ключа: "8:completei12e" -> 12. -1, если ключа нет.
static int bencode_int(const std::string& body, const char* key)
{
    std::string k = std::to_string(strlen(key)) + ":" + key + "i";
    size_t p = body.find(k);
    if (p == std::string::npos) return -1;
    p += k.size();
    size_t e = body.find('e', p);
    if (e == std::string::npos || e == p) return -1;
    return atoi(body.substr(p, e - p).c_str());
}

static bool peer_stats_announce(const std::string& hash, int& seeders, int& leechers, std::string* raw)
{
    // numwant=0: список раздающих не нужен, только счётчики complete / incomplete. event=stopped: трекер нас в раздающие не записывает.
    std::string path = "/ann?magnet&info_hash=" + hex_to_pct(hash) + "&peer_id=-PI0001-000000000000&port=6881&uploaded=0&downloaded=0&left=1&compact=1&numwant=0&event=stopped";
    HttpResult r = http_get(g_trHost, g_trPort, path, 4000);
    if (raw) *raw = "announce[подключились=" + std::to_string(r.connected) + " код=" + std::to_string(r.status) + " тело=" + r.body.substr(0, 150) + "]";
    if (!r.connected || r.status != 200) return false;
    int c = bencode_int(r.body, "complete"), i = bencode_int(r.body, "incomplete");
    if (c < 0 && i < 0) return false;
    seeders = c < 0 ? 0 : c;
    leechers = i < 0 ? 0 : i;
    return true;
}

static bool peer_stats_api(const std::string& hash, int& seeders, int& leechers, std::string* raw)
{
    HttpResult r = http_get(PEER_API_HOST, 80, "/v1/get_peer_stats?by=hash&val=" + hash, 4000);
    if (raw) *raw += " api[подключились=" + std::to_string(r.connected) + " код=" + std::to_string(r.status) + " тело=" + r.body.substr(0, 150) + "]";
    if (!r.connected || r.status != 200) return false;
    JsonValue j;
    if (!json_parse(r.body, j)) return false;
    const JsonValue& res = j["result"];   // {"<хэш>": [раздающих, качающих, когда последний раздающий был]}
    if (res.type != JsonValue::OBJ || res.obj.empty()) return false;
    const JsonValue& a = res.obj[0].second;
    if (a.type != JsonValue::ARR || a.arr.size() < 2) return false;
    seeders = (int)a.arr[0].asInt();
    leechers = (int)a.arr[1].asInt();
    return true;
}

bool torrents_peer_stats(const std::string& hash, int& seeders, int& leechers, std::string* raw)
{
    seeders = leechers = 0;
    if (hash.size() != 40) return false;
    if (g_trHost == "bt.t-ru.org" && !net_online_cached(800, true)) return false;   // нет интернета - запросов нет (в проверках трекер подменён)
    if (peer_stats_announce(hash, seeders, leechers, raw)) return true;
    return peer_stats_api(hash, seeders, leechers, raw);
}
