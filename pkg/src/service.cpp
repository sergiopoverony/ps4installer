#include "service.h"
#include "httpc.h"
#include "json.h"
#include "log.h"

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
        it.rus = p["rus"].asBool();
        it.dlc_name = p["dlc_name"].asBool();
        if (!it.path.empty()) out.push_back(it);
    }
    logf_("service: /scan -> %d файлов", (int)out.size());
    return true;
}
