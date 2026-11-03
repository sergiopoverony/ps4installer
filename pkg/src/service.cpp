#include "service.h"
#include "httpc.h"
#include "json.h"
#include "log.h"
#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>

static const int LOADER_PORT = 9090;
static int g_fwMajor = -1;
static const char* PAYLOAD_PATH = "/app0/assets/misc/ps4installerd.elf";

// Два разных пути для одного файла автозапуска GoldHEN (так сделано и в других проектах):
//  - AUTORUN_ELF_PATH: под этим путём приложение реально пишет файл (песочница);
//  - AUTORUN_INI_ENTRY: этот путь (с /user) записывается в payloads.ini, по нему GoldHEN ищет файл.
static const char* AUTORUN_ELF_PATH  = "/data/payloads/ps4installerd.elf";
static const char* AUTORUN_INI_ENTRY = "/user/data/payloads/ps4installerd.elf";
static const char* AUTORUN_INI_PATH  = "/data/GoldHEN/payloads.ini";

static bool read_payload(std::string& data, std::string& err)
{
    FILE* f = fopen(PAYLOAD_PATH, "rb");
    if (!f) { err = "no payload file in PKG"; logf_("service: cannot open %s", PAYLOAD_PATH); return false; }
    char buf[16384];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
    fclose(f);
    if (data.size() < 1024) { err = "payload file is empty"; return false; }
    return true;
}

bool service_query(std::string& version)
{
    HttpResult r = http_get(SERVICE_HOST, SERVICE_PORT, "/health", 1500);
    if (!r.connected || r.status != 200) return false;
    JsonValue j;
    if (!json_parse(r.body, j) || !j["ok"].asBool()) return false;
    version = j["version"].asStr();
    g_fwMajor = (int)atoi(j["fw"].asStr("-1").c_str());
    if (g_fwMajor <= 0) g_fwMajor = -1;
    return !version.empty();
}

int service_fw_major() { return g_fwMajor; }

static std::string json_quote(const std::string& s)
{
    std::string o = "\"";
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o + "\"";
}

bool service_free(uint64_t& bytes)
{
    HttpResult r = http_get(SERVICE_HOST, SERVICE_PORT, "/free", 3000);
    if (!r.connected || r.status != 200) return false;
    JsonValue j;
    if (!json_parse(r.body, j) || !j["ok"].asBool()) return false;
    bytes = (uint64_t)j["free_bytes"].asNum();
    return true;
}

bool service_icon(const std::string& pkgPath, std::string& png)
{
    std::string q = "/icon?path=";
    for (size_t i = 0; i < pkgPath.size(); i++) {
        unsigned char c = (unsigned char)pkgPath[i];
        if (isalnum(c) || c == '/' || c == '.' || c == '_' || c == '-') q += (char)c;
        else { char b[8]; snprintf(b, sizeof(b), "%%%02X", c); q += b; }
    }
    HttpResult r = http_get(SERVICE_HOST, SERVICE_PORT, q, 4000);
    if (!r.connected || r.status != 200 || r.body.size() < 16) return false;
    png = r.body;
    return true;
}

bool service_installed(std::vector<std::string>& ids)
{
    ids.clear();
    HttpResult r = http_get(SERVICE_HOST, SERVICE_PORT, "/installed", 3000);
    if (!r.connected || r.status != 200) return false;
    JsonValue j;
    if (!json_parse(r.body, j) || !j["ok"].asBool()) return false;
    const JsonValue& a = j["title_ids"];
    for (size_t i = 0; i < a.arr.size(); i++) ids.push_back(a.arr[i].asStr());
    return true;
}

bool service_install(const std::vector<std::string>& paths, std::string& err, std::vector<std::string>* skipped)
{
    std::string body = "{\"paths\":[";
    for (size_t i = 0; i < paths.size(); i++) body += (i ? "," : "") + json_quote(paths[i]);
    body += "]}";
    HttpResult r = http_post(SERVICE_HOST, SERVICE_PORT, "/install", body, 20000);
    if (!r.connected) { err = "no_service"; return false; }
    JsonValue j;
    if (!json_parse(r.body, j)) { err = "http " + std::to_string(r.status); return false; }
    if (!j["ok"].asBool()) {
        err = j["error"].asStr("error");
        std::string d = j["detail"].asStr();
        if (!d.empty()) err += ": " + d;
        return false;
    }
    if (skipped) {
        const JsonValue& sk = j["skipped"];
        for (size_t i = 0; i < sk.arr.size(); i++)
            skipped->push_back(sk.arr[i]["path"].asStr() + " - " + sk.arr[i]["reason"].asStr());
    }
    return true;
}

bool service_status(InstallStatus& out)
{
    out = InstallStatus();
    HttpResult r = http_get(SERVICE_HOST, SERVICE_PORT, "/status", 2500);
    if (!r.connected || r.status != 200) return false;
    JsonValue j;
    if (!json_parse(r.body, j)) return false;
    out.running = j["state"].asStr() == "running";
    const JsonValue& jobs = j["jobs"];
    for (size_t i = 0; i < jobs.arr.size(); i++) {
        InstallJob jb;
        jb.path = jobs.arr[i]["path"].asStr();
        jb.kind = jobs.arr[i]["kind"].asStr();
        jb.status = jobs.arr[i]["status"].asStr();
        jb.message = jobs.arr[i]["message"].asStr();
        jb.size = (uint64_t)jobs.arr[i]["size"].asNum();
        jb.length = (uint64_t)jobs.arr[i]["length"].asNum();
        jb.transferred = (uint64_t)jobs.arr[i]["transferred"].asNum();
        out.jobs.push_back(jb);
    }
    return true;
}

bool service_send_payload(std::string& err, bool* connectFailed)
{
    if (connectFailed) *connectFailed = false;
    std::string data;
    if (!read_payload(data, err)) return false;

    int s = net_connect(SERVICE_HOST, LOADER_PORT, 3000);
    if (s < 0) {
        err = "BinLoader (port 9090) is not available";
        logf_("service: BinLoader connect failed");
        if (connectFailed) *connectFailed = true;
        return false;
    }
    size_t sent = 0;
    while (sent < data.size()) {
        int w = send(s, data.data() + sent, data.size() - sent, 0);
        if (w <= 0) { err = "sending to BinLoader failed"; logf_("service: send failed after %d of %d bytes", (int)sent, (int)data.size()); close(s); return false; }
        sent += (size_t)w;
    }
    close(s);
    logf_("service: payload sent (%d bytes)", (int)sent);
    return true;
}

bool service_install_autorun(std::string& err)
{
    std::string data;
    if (!read_payload(data, err)) return false;

    mkdir("/data/payloads", 0777);
    FILE* out = fopen(AUTORUN_ELF_PATH, "wb");
    if (!out) { err = std::string("cannot write ") + AUTORUN_ELF_PATH; logf_("autorun: %s", err.c_str()); return false; }
    size_t wn = fwrite(data.data(), 1, data.size(), out);
    fclose(out);
    if (wn != data.size()) { err = "autorun payload write incomplete"; logf_("autorun: %s", err.c_str()); return false; }

    std::string ini;
    FILE* fi = fopen(AUTORUN_INI_PATH, "rb");
    if (fi) {
        char ibuf[4096];
        size_t rn;
        while ((rn = fread(ibuf, 1, sizeof(ibuf), fi)) > 0) ini.append(ibuf, rn);
        fclose(fi);
    }
    if (ini.find(AUTORUN_INI_ENTRY) != std::string::npos) {
        logf_("autorun: payload copied, payloads.ini already has the entry - leaving as is");
        return true;
    }
    std::string entry = std::string(AUTORUN_INI_ENTRY) + " = 1\n";
    size_t sec = ini.find("[AutoRun]");
    if (sec == std::string::npos) {
        if (!ini.empty() && ini.back() != '\n') ini += "\n";
        if (!ini.empty()) ini += "\n";
        ini += "[AutoRun]\n";
        ini += entry;
    } else {
        size_t lineEnd = ini.find('\n', sec);
        size_t insertAt = (lineEnd == std::string::npos) ? ini.size() : lineEnd + 1;
        ini.insert(insertAt, entry);
    }
    FILE* fw = fopen(AUTORUN_INI_PATH, "wb");
    if (!fw) { err = std::string("cannot write ") + AUTORUN_INI_PATH; logf_("autorun: %s", err.c_str()); return false; }
    fwrite(ini.data(), 1, ini.size(), fw);
    fclose(fw);
    logf_("autorun: payload installed and registered in payloads.ini (%s)", AUTORUN_INI_ENTRY);
    return true;
}

bool service_scan(std::vector<PkgItem>& out, std::string& err)
{
    out.clear();
    HttpResult r = http_get(SERVICE_HOST, SERVICE_PORT, "/scan", 8000);
    if (!r.connected) { err = "no_service"; return false; }
    if (r.status != 200) { err = "http " + std::to_string(r.status); return false; }
    JsonValue j;
    if (!json_parse(r.body, j) || !j["ok"].asBool()) { err = "bad_json"; return false; }
    const JsonValue& arr = j["packages"];
    for (size_t i = 0; i < arr.arr.size(); i++) {
        const JsonValue& p = arr.arr[i];
        PkgItem it;
        it.path = p["path"].asStr();
        it.content_id = p["content_id"].asStr();
        it.title_id = p["title_id"].asStr();
        it.kind = p["kind"].asStr();
        it.title = p["title"].asStr();
        it.ver = p["ver"].asStr();
        it.size = (uint64_t)p["size"].asNum();
        it.patch = p["patch"].asBool();
        it.backport = p["backport"].asBool();
        it.incomplete = p["incomplete"].asBool();
        it.rus = p["rus"].asBool();
        it.dlc_name = p["dlc_name"].asBool();
        if (!it.path.empty()) out.push_back(it);
    }
    logf_("service: /scan -> %d файлов", (int)out.size());
    return true;
}
