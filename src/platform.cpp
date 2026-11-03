#include <dirent.h>
#include <cctype>
#include "platform.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "log.h"

#ifdef HOST_TEST
#include <sys/statvfs.h>

#include <map>
#include <mutex>
#else
#include <dlfcn.h>
#include <sys/mount.h>
#include <sys/param.h>
#endif

namespace platform {

// ----------------------------------------------------------------------------------
#ifdef HOST_TEST
// Заглушки для проверок на ПК. Поведение управляется содержимым content id:
//   "...-FAIL..."   регистрация возвращает ошибку 0x80990001
//   повторная установка того же content id -> "уже установлено"
//   "...-BUSY..."   первые две регистрации возвращают 0x80990015 (занято), потом успех
//   "...-STALL..."  прогресс никогда не растёт (проверка таймаута)

static std::mutex g_fake_mu;
static std::map<int, std::pair<std::string, uint64_t>> g_fake_tasks;  // task -> (content id, polls)
static std::map<std::string, bool> g_fake_installed;
static int g_fake_next_task = 100;
static std::map<std::string, int> g_fake_busy;

void notify(const std::string& msg) { log_printf("NOTIFY: %s", msg.c_str()); }
void notify_icon(const std::string& msg, const std::string& icon_uri) { log_printf("NOTIFY: %s [icon %s]", msg.c_str(), icon_uri.c_str()); }
std::vector<std::string> installed_title_ids() {
    std::vector<std::string> out;
    const char* env = getenv("FAKE_INSTALLED");
    std::string s = env ? env : "";
    size_t i = 0;
    while (i < s.size()) {
        size_t k = s.find(',', i);
        if (k == std::string::npos) k = s.size();
        if (k > i) out.push_back(s.substr(i, k - i));
        i = k + 1;
    }
    return out;
}
void power_tick() {}

bool free_bytes(const char* path, uint64_t* out) {
    const char* env = getenv("FAKE_FREE_BYTES");
    if (env) {
        *out = strtoull(env, nullptr, 10);
        return true;
    }
    struct statvfs sv;
    if (statvfs(path, &sv) != 0) {
        return false;
    }
    *out = (uint64_t)sv.f_bavail * sv.f_frsize;
    return true;
}

int firmware_major(std::string* text) {
    const char* env = getenv("FAKE_FW");  // например "13.02" или "9.00"
    std::string v = env ? env : "13.02";
    *text = v;
    return atoi(v.c_str());
}

bool bgft_load(std::string* err) {
    (void)err;
    return true;
}
std::string bgft_info() { return "fake"; }

bool bgft_register_local(const std::string& path, const std::string& content_id, int* task_id, uint32_t* err) {
    std::lock_guard<std::mutex> lk(g_fake_mu);
    log_printf("fake register %s (%s)", path.c_str(), content_id.c_str());
    if (content_id.find("FAIL") != std::string::npos) {
        *err = 0x80990001;
        return false;
    }
    if (content_id.find("BUSY") != std::string::npos && g_fake_busy[content_id]++ < 2) {
        *err = BGFT_ERR_BUSY;
        return false;
    }
    if (g_fake_installed[content_id]) {
        *err = BGFT_ERR_ALREADY_INSTALLED;
        return false;
    }
    *task_id = g_fake_next_task++;
    g_fake_tasks[*task_id] = {content_id, 0};
    return true;
}

bool bgft_start(int task_id, uint32_t* err) {
    (void)err;
    (void)task_id;
    return true;
}

bool bgft_progress(int task_id, Progress* p, uint32_t* err) {
    std::lock_guard<std::mutex> lk(g_fake_mu);
    auto it = g_fake_tasks.find(task_id);
    if (it == g_fake_tasks.end()) {
        *err = 0x80990002;
        return false;
    }
    p->length = 1000;
    if (it->second.first.find("STALL") != std::string::npos) {
        p->transferred = 10;
        return true;
    }
    it->second.second++;
    uint64_t t = it->second.second * 250;
    p->transferred = t > 1000 ? 1000 : t;
    if (p->transferred >= 1000) {
        g_fake_installed[it->second.first] = true;
    }
    return true;
}

// ----------------------------------------------------------------------------------
#else  // консоль

// Структуры системной установки - по открытым заголовкам flatz/ps4_stub_lib_maker_v2 (bgft.h).
// Раскладка проверена только чтением исходников, на консоли не проверена.
struct BgftInitParams {
    void* heap;
    size_t heapSize;
};

struct BgftDownloadParam {
    int userId;
    int entitlementType;
    const char* id;
    const char* contentUrl;
    const char* contentExUrl;
    const char* contentName;
    const char* iconPath;
    const char* skuId;
    uint32_t option;
    const char* playgoScenarioId;
    const char* releaseDate;
    const char* packageType;
    const char* packageSubType;
    unsigned long packageSize;
};

struct BgftDownloadParamEx {
    BgftDownloadParam params;
    unsigned int slot;
};

struct BgftTaskProgress {
    unsigned int bits;
    int errorResult;
    unsigned long length;
    unsigned long transferred;
    unsigned long lengthTotal;
    unsigned long transferredTotal;
    unsigned int numIndex;
    unsigned int numTotal;
    unsigned int restSec;
    unsigned int restSecTotal;
    int preparingPercent;
    int localCopyPercent;
};

static_assert(sizeof(BgftDownloadParam) == 0x68, "BgftDownloadParam: размер не совпадает с ожидаемым");

typedef int (*fn_init_t)(BgftInitParams*);
typedef int (*fn_reg_ex_t)(BgftDownloadParamEx*, int*);
typedef int (*fn_task_t)(int);
typedef int (*fn_progress_t)(int, BgftTaskProgress*);

static const uint32_t BGFT_OPT_FORCE_UPDATE = 0x8;

static bool g_loaded = false;
static std::string g_load_err;
static std::string g_info = "not loaded";
static fn_reg_ex_t g_register_ex = nullptr;
static fn_task_t g_start = nullptr;
static fn_progress_t g_progress = nullptr;

// Структура уведомления на экран (3120 байт).
struct NotificationRequest {
    int type;
    int reqId;
    int priority;
    int msgId;
    int targetId;
    int userId;
    int unk1;
    int unk2;
    int appId;
    int errorNum;
    int unk3;
    uint8_t useIconImageUri;
    char message[1024];
    char iconUri[1024];
    char unk[1024];
};
static_assert(sizeof(NotificationRequest) == 3120, "NotificationRequest должна быть 3120 байт");

extern "C" int sceKernelSendNotificationRequest(int device, NotificationRequest* req, size_t size, int blocking);
extern "C" int sceSystemServicePowerTick(void);

void notify(const std::string& msg) {
    NotificationRequest req;
    memset(&req, 0, sizeof(req));
    req.type = 0;
    req.targetId = -1;
    req.useIconImageUri = 0;
    snprintf(req.message, sizeof(req.message), "%s", msg.c_str());
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

void notify_icon(const std::string& msg, const std::string& icon_uri) {
    NotificationRequest req;
    memset(&req, 0, sizeof(req));
    req.type = 0;
    req.targetId = -1;
    req.useIconImageUri = icon_uri.empty() ? 0 : 1;
    snprintf(req.message, sizeof(req.message), "%s", msg.c_str());
    snprintf(req.iconUri, sizeof(req.iconUri), "%s", icon_uri.c_str());
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

std::vector<std::string> installed_title_ids() {
    std::vector<std::string> out;
    DIR* d = opendir("/user/app");
    if (!d) return out;
    while (struct dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n.size() == 9 && isalpha((unsigned char)n[0])) out.push_back(n);
    }
    closedir(d);
    return out;
}

void power_tick() { sceSystemServicePowerTick(); }

bool free_bytes(const char* path, uint64_t* out) {
    struct statfs sf;
    if (statfs(path, &sf) != 0) {
        return false;
    }
    *out = (uint64_t)sf.f_bavail * (uint64_t)sf.f_bsize;
    return true;
}

extern "C" int sceKernelGetSystemSwVersion(void* out);

// Структура версии системы (по открытым описаниям; на консоли не проверена).
struct SystemSwVersion {
    uint64_t size;
    char version_string[0x1C];
    uint32_t version;  // BCD: 0x13020000 для 13.02
};

int firmware_major(std::string* text) {
    static int cached = -2;
    static std::string cached_text;
    if (cached == -2) {
        SystemSwVersion sw;
        memset(&sw, 0, sizeof(sw));
        sw.size = sizeof(sw);
        int ret = sceKernelGetSystemSwVersion(&sw);
        sw.version_string[sizeof(sw.version_string) - 1] = 0;
        cached = -1;
        if (ret == 0) {
            int major = atoi(sw.version_string);
            if (major <= 0) {  // запасной путь: BCD из числа версии
                major = (int)(((sw.version >> 28) & 0xF) * 10 + ((sw.version >> 24) & 0xF));
            }
            cached = major > 0 ? major : -1;
            cached_text = sw.version_string;
            if (cached_text.empty()) {
                char b[16];
                snprintf(b, sizeof(b), "%X.%02X", (unsigned)(sw.version >> 24), (unsigned)((sw.version >> 16) & 0xFF));
                cached_text = b;
            }
        } else {
            cached_text = "unknown";
        }
        log_printf("firmware: %s (major=%d, ret=0x%08X)", cached_text.c_str(), cached, (unsigned)ret);
    }
    *text = cached_text;
    return cached;
}

static void* find_sym(void* h, const char* a, const char* b, std::string* used) {
    void* p = dlsym(h, a);
    if (p) {
        used->append(a).append(" ");
        return p;
    }
    if (b) {
        p = dlsym(h, b);
        if (p) {
            used->append(b).append(" ");
            return p;
        }
    }
    used->append("MISSING:").append(a).append(" ");
    return nullptr;
}

bool bgft_load(std::string* err) {
    if (g_loaded) {
        return true;
    }
    if (!g_load_err.empty()) {
        *err = g_load_err;
        return false;
    }
    static const char* dirs[] = {"/system/common/lib/", "/system/priv/lib/", "/system_ex/common_ex/lib/",
                                 "/system_ex/priv_ex/lib/"};
    void* h = nullptr;
    for (const char* d : dirs) {
        std::string p = std::string(d) + "libSceBgft.sprx";
        h = dlopen(p.c_str(), 0);
        log_printf("dlopen %s -> %p", p.c_str(), h);
        if (h) {
            break;
        }
    }
    if (!h) {
        g_load_err = "не удалось загрузить libSceBgft.sprx";
        *err = g_load_err;
        return false;
    }
    // Имена функций в разных справочниках отличаются (с "ServiceInt" и без), поэтому пробуем оба.
    std::string used;
    fn_init_t init = (fn_init_t)find_sym(h, "sceBgftServiceIntInit", "sceBgftInitialize", &used);
    g_register_ex = (fn_reg_ex_t)find_sym(h, "sceBgftServiceIntDownloadRegisterTaskByStorageEx",
                                          "sceBgftDownloadRegisterTaskByStorageEx", &used);
    g_start = (fn_task_t)find_sym(h, "sceBgftServiceDownloadStartTask", "sceBgftDownloadStartTask", &used);
    g_progress = (fn_progress_t)find_sym(h, "sceBgftServiceDownloadGetProgress", "sceBgftDownloadGetProgress", &used);
    g_info = used;
    log_printf("bgft symbols: %s", used.c_str());
    if (!init || !g_register_ex || !g_start || !g_progress) {
        g_load_err = "не найдены функции BGFT: " + used;
        *err = g_load_err;
        return false;
    }
    // Память под BGFT берём не из кучи (она может быть исчерпана после многих замен службы), а из
    // заранее выделенного статического буфера: он есть с момента загрузки программы.
    static BgftInitParams params;
    alignas(4096) static uint8_t bgft_heap[1 * 1024 * 1024];
    memset(&params, 0, sizeof(params));
    params.heap = bgft_heap;
    params.heapSize = sizeof(bgft_heap);
    int ret = init(&params);
    log_printf("bgft init -> 0x%08X", (unsigned)ret);
    // 0x80990001 после замены службы на новую версию: старая копия уже инициализировала BGFT
    // в этом же процессе. Считаем это "уже инициализирован" (на консоли не проверено).
    if ((unsigned)ret == 0x80990001u) {
        g_info += "(init=0x80990001: считаем, что уже инициализирован) ";
        ret = 0;
    }
    if (ret != 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "инициализация BGFT: 0x%08X", (unsigned)ret);
        g_load_err = buf;
        *err = g_load_err;
        return false;
    }
    g_loaded = true;
    return true;
}

std::string bgft_info() { return g_info; }

bool bgft_register_local(const std::string& path, const std::string& content_id, int* task_id, uint32_t* err) {
    BgftDownloadParamEx p;
    memset(&p, 0, sizeof(p));
    p.params.entitlementType = 5;
    p.params.id = content_id.c_str();
    p.params.contentUrl = path.c_str();
    p.params.contentName = content_id.c_str();
    p.params.iconPath = "";
    p.params.playgoScenarioId = "0";
    p.params.option = BGFT_OPT_FORCE_UPDATE;
    p.slot = 0;
    int tid = -1;
    int ret = g_register_ex(&p, &tid);
    log_printf("bgft register %s -> 0x%08X task=%d", path.c_str(), (unsigned)ret, tid);
    if (ret != 0) {
        *err = (uint32_t)ret;
        return false;
    }
    *task_id = tid;
    return true;
}

bool bgft_start(int task_id, uint32_t* err) {
    int ret = g_start(task_id);
    log_printf("bgft start task=%d -> 0x%08X", task_id, (unsigned)ret);
    if (ret != 0) {
        *err = (uint32_t)ret;
        return false;
    }
    return true;
}

bool bgft_progress(int task_id, Progress* out, uint32_t* err) {
    BgftTaskProgress p;
    memset(&p, 0, sizeof(p));
    int ret = g_progress(task_id, &p);
    if (ret != 0) {
        *err = (uint32_t)ret;
        return false;
    }
    out->bits = p.bits;
    out->error_result = p.errorResult;
    out->length = p.length;
    out->transferred = p.transferred;
    return true;
}

#endif  // HOST_TEST

}  // namespace platform
