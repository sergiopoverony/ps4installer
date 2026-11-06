#include "torrents.h"
#include "httpc.h"
#include "json.h"
#include "log.h"

#include <stdio.h>
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

static std::string g_usbBase = "/mnt";

void torrents_set_usb_base(const std::string& base) { g_usbBase = base; }

std::string usb_root()
{
    for (int i = 0; i < 8; i++) {
        std::string p = g_usbBase + "/usb" + std::to_string(i);
        struct stat st;
        if (stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && access(p.c_str(), W_OK) == 0) return p;
    }
    return "";
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

bool torrents_save_usb(const std::string& root, const std::string& hash, const std::string& title, const std::string& id,
                       std::string& path, std::string& err)
{
    std::string h = torrents_clean_hash(hash);
    if (h.empty()) { err = "bad hash"; return false; }
    if (root.empty()) { err = "no usb"; return false; }
    path = root + "/" + torrents_magnet_filename(title, id);
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) { err = "write"; return false; }
    std::string m = torrents_magnet(h, title) + "\n";
    bool ok = fwrite(m.data(), 1, m.size(), f) == m.size();
    if (fclose(f) != 0) ok = false;
    if (!ok) { err = "write"; return false; }
    return true;
}
