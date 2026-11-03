#include "installer.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

#include "json.h"
#include "log.h"
#include "pkg.h"
#include "platform.h"
#include "version.h"

namespace installer {

namespace {

struct Job {
    PkgInfo pkg;
    std::string status = "queued";  // queued | registering | installing | done | already_installed | failed | cancelled
    int task = -1;
    uint64_t length = 0;
    uint64_t transferred = 0;
    std::string message;
    int group = 0;  // номер пачки (один запрос /install)
};

std::mutex g_mu;
std::condition_variable g_cv;
std::vector<Job> g_jobs;
bool g_stop = false;
bool g_worker_started = false;
int g_next_group = 1;
std::vector<int> g_notified_groups;  // пачки, по которым уведомление уже показано
std::thread g_worker;
int g_stall_sec = 180;
int g_poll_ms = 1000;

bool is_active(const std::string& s) { return s == "queued" || s == "registering" || s == "installing"; }

bool any_active_locked() {
    for (const Job& j : g_jobs) {
        if (is_active(j.status)) {
            return true;
        }
    }
    return false;
}

int next_queued_locked() {
    for (size_t i = 0; i < g_jobs.size(); i++) {
        if (g_jobs[i].status == "queued") {
            return (int)i;
        }
    }
    return -1;
}

std::string hex_err(uint32_t e) {
    char b[16];
    snprintf(b, sizeof(b), "0x%08X", e);
    return b;
}

void set_status(int idx, const char* status, const std::string& msg = "") {
    std::lock_guard<std::mutex> lk(g_mu);
    g_jobs[idx].status = status;
    if (!msg.empty()) {
        g_jobs[idx].message = msg;
    }
}

void run_job(int idx) {
    PkgInfo pkg;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        pkg = g_jobs[idx].pkg;
    }
    log_printf("install begin: %s (%s)", pkg.path.c_str(), pkg.content_id.c_str());

    int task = -1;
    uint32_t err = 0;
    bool registered = platform::bgft_register_local(bgft_path(pkg.path), pkg.content_id, &task, &err);
    // 0x80990015 ("занято"): повторяем с паузой до ~90 секунд (в тестах паузы короче).
    for (int attempt = 1; !registered && err == platform::BGFT_ERR_BUSY && attempt <= 30; attempt++) {
        log_printf("install: система занята (0x80990015), повтор %d через паузу: %s", attempt, pkg.content_id.c_str());
        {
            std::unique_lock<std::mutex> lk(g_mu);
            if (g_cv.wait_for(lk, std::chrono::milliseconds(g_poll_ms * 3), [] { return g_stop; })) {
                return;
            }
        }
        registered = platform::bgft_register_local(bgft_path(pkg.path), pkg.content_id, &task, &err);
    }
    if (!registered) {
        if (err == platform::BGFT_ERR_ALREADY_INSTALLED) {
            set_status(idx, "already_installed", "уже установлено");
            log_printf("install skip (already installed): %s", pkg.content_id.c_str());
        } else {
            set_status(idx, "failed", "регистрация задачи: " + hex_err(err));
            log_printf("install failed (register): %s %s", pkg.content_id.c_str(), hex_err(err).c_str());
            platform::notify("ps4installer: error " + hex_err(err) + " " + pkg.title_id);
        }
        return;
    }
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_jobs[idx].task = task;
    }
    if (!platform::bgft_start(task, &err)) {
        set_status(idx, "failed", "запуск задачи: " + hex_err(err));
        platform::notify("ps4installer: error " + hex_err(err) + " " + pkg.title_id);
        return;
    }
    set_status(idx, "installing");

    uint64_t last_transferred = 0;
    auto last_change = std::chrono::steady_clock::now();
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(g_mu);
            if (g_cv.wait_for(lk, std::chrono::milliseconds(g_poll_ms), [] { return g_stop; })) {
                return;
            }
        }
        platform::power_tick();
        platform::Progress p;
        if (!platform::bgft_progress(task, &p, &err)) {
            set_status(idx, "failed", "ход установки: " + hex_err(err));
            return;
        }
        {
            std::lock_guard<std::mutex> lk(g_mu);
            g_jobs[idx].length = p.length;
            g_jobs[idx].transferred = p.transferred;
        }
        if (p.error_result != 0) {
            set_status(idx, "failed", "ошибка установки: " + hex_err((uint32_t)p.error_result));
            log_printf("install failed: %s %s", pkg.content_id.c_str(), hex_err((uint32_t)p.error_result).c_str());
            platform::notify("ps4installer: error " + hex_err((uint32_t)p.error_result) + " " + pkg.title_id);
            return;
        }
        if (p.length > 0 && p.transferred >= p.length) {
            set_status(idx, "done");
            log_printf("install done: %s", pkg.content_id.c_str());
            return;
        }
        auto now = std::chrono::steady_clock::now();
        if (p.transferred != last_transferred) {
            last_transferred = p.transferred;
            last_change = now;
        } else if (std::chrono::duration_cast<std::chrono::seconds>(now - last_change).count() >= g_stall_sec) {
            set_status(idx, "failed", "установка не продвигается");
            log_printf("install stalled: %s", pkg.content_id.c_str());
            return;
        }
    }
}

// Когда вся пачка закончилась без ошибок - показываем на экране консоли «ИМЯ ИГРЫ / УСТАНОВЛЕНО».
// Иконка берётся из каталога метаданных игры; работает ли это на консоли - не проверено.
void notify_if_finished() {
    for (;;) {
        std::string title, tid;
        int grp = 0;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            // Ищем пачку, в которой всё закончилось, и по ней ещё не уведомляли.
            for (const Job& j : g_jobs) {
                bool done_before = false;
                for (int g : g_notified_groups) {
                    done_before = done_before || g == j.group;
                }
                if (done_before) {
                    continue;
                }
                bool all_done = true, failed = false;
                const Job* pick = nullptr;
                for (const Job& k : g_jobs) {
                    if (k.group != j.group) {
                        continue;
                    }
                    all_done = all_done && !is_active(k.status);
                    failed = failed || k.status == "failed";
                    if (!pick || (k.pkg.kind == "game" && pick->pkg.kind != "game")) {
                        pick = &k;
                    }
                }
                if (!all_done) {
                    continue;
                }
                grp = j.group;
                g_notified_groups.push_back(grp);
                if (!failed && pick) {
                    title = pick->pkg.title.empty() ? pick->pkg.title_id : pick->pkg.title;
                    tid = pick->pkg.title_id;
                }
                break;
            }
        }
        if (grp == 0) {
            return;
        }
        if (!title.empty()) {
            platform::notify_icon(title + "\nУстановлено", "/user/appmeta/" + tid + "/icon0.png");
        }
    }
}

void worker_main() {
    for (;;) {
        int idx;
        {
            std::unique_lock<std::mutex> lk(g_mu);
            g_cv.wait(lk, [] { return g_stop || next_queued_locked() >= 0; });
            if (g_stop) {
                return;
            }
            idx = next_queued_locked();
            g_jobs[idx].status = "registering";
        }
        run_job(idx);
        notify_if_finished();
    }
}

std::string order_json(const std::vector<PkgInfo>& list) {
    std::string o = "[";
    for (size_t i = 0; i < list.size(); i++) {
        if (i) {
            o += ",";
        }
        o += "\"" + json_escape(list[i].path) + "\"";
    }
    return o + "]";
}

EnqueueResult fail(int code, const char* error, const std::string& detail) {
    EnqueueResult r;
    r.ok = false;
    r.http_code = code;
    r.error = error;
    r.detail = detail;
    return r;
}

}  // namespace

EnqueueResult enqueue(const std::vector<std::string>& paths) {
    if (paths.empty()) {
        return fail(400, "bad_request", "пустой список пакетов");
    }
    if (paths.size() > 500) {
        return fail(400, "bad_request", "слишком много пакетов за один запрос");
    }
    std::vector<PkgInfo> list;
    for (const std::string& p : paths) {
        if (!path_allowed(p)) {
            return fail(400, "bad_path", "путь не разрешён: " + p);
        }
        bool dup = false;
        for (const PkgInfo& e : list) {
            dup = dup || e.path == p;
        }
        if (dup) {
            continue;
        }
        PkgInfo info;
        std::string err;
        if (!pkg_read_header(p, &info, &err)) {
            return fail(422, "bad_pkg", p + ": " + err);
        }
        // Один и тот же пакет на диске и на флешке (тот же content_id, тип и размер) ставим один раз.
        bool same = false;
        for (const PkgInfo& e : list) {
            same = same || (e.content_id == info.content_id && e.content_type == info.content_type &&
                            e.file_size == info.file_size);
        }
        if (same) {
            continue;
        }
        list.push_back(info);
    }
    std::vector<SkippedPkg> skipped;
    {
        std::vector<PkgInfo> ok;
        for (const PkgInfo& e : list) {
            if (false && e.incomplete()) {  // проверка отменена по решению пользователя
                skipped.push_back(SkippedPkg{e, "incomplete"});
            } else {
                ok.push_back(e);
            }
        }
        list = ok;
    }
    std::string fw_str;
    int fw_major = platform::firmware_major(&fw_str);
    list = plan_install(list, fw_major, &skipped);
    for (const SkippedPkg& sk : skipped) {
        log_printf("enqueue: пропущен %s (%s)", sk.pkg.path.c_str(), sk.reason.c_str());
    }

    uint64_t need = 0;
    for (const PkgInfo& e : list) {
        need += e.file_size;
    }
    uint64_t pending = 0;  // ещё не скачанное по уже идущей очереди
    {
        std::lock_guard<std::mutex> lk(g_mu);
        for (const Job& j : g_jobs) {
            if (is_active(j.status)) {
                pending += j.pkg.file_size;
            }
        }
    }
    need += pending;
    uint64_t free_b = 0;
    // Грубая проверка: размер пакетов против свободного места. Реальный размер установленной
    // игры может отличаться (не проверено на консоли), поэтому это лишь защита от явной нехватки.
    if (platform::free_bytes("/user", &free_b) && free_b < need) {
        EnqueueResult r = fail(409, "no_space", "недостаточно места");
        r.need = need;
        r.free = free_b;
        return r;
    }

    EnqueueResult r;
    r.skipped_json = "[";
    for (size_t i = 0; i < skipped.size(); i++) {
        r.skipped_json += std::string(i ? "," : "") + "{\"path\":\"" + json_escape(skipped[i].pkg.path) +
                          "\",\"reason\":\"" + json_escape(skipped[i].reason) + "\"}";
    }
    r.skipped_json += "]";
    if (list.empty()) {  // всё пропущено: ставить нечего
        r.ok = true;
        r.order_json = "[]";
        return r;
    }

    std::string berr;
    if (!platform::bgft_load(&berr)) {
        return fail(500, "bgft", berr);
    }

    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!any_active_locked()) {
            g_jobs.clear();
            g_notified_groups.clear();
        }
        // Уже стоящее в очереди или идущее второй раз не добавляем.
        std::vector<PkgInfo> fresh;
        for (const PkgInfo& e : list) {
            bool dup = false;
            for (const Job& j : g_jobs) {
                dup = dup || (is_active(j.status) && j.pkg.path == e.path);
            }
            if (!dup) {
                fresh.push_back(e);
            }
        }
        list = fresh;
        int grp = g_next_group++;
        for (const PkgInfo& e : list) {
            Job j;
            j.pkg = e;
            j.group = grp;
            g_jobs.push_back(j);
        }
        if (!g_worker_started) {
            g_worker_started = true;
            g_worker = std::thread(worker_main);
        }
    }
    g_cv.notify_all();
    log_printf("enqueue: %zu пакетов, нужно %llu байт, свободно %llu", list.size(), (unsigned long long)need,
               (unsigned long long)free_b);

    r.ok = true;
    r.order_json = order_json(list);
    r.need = need;
    r.free = free_b;
    return r;
}

void cancel_pending() {
    std::lock_guard<std::mutex> lk(g_mu);
    for (Job& j : g_jobs) {
        if (j.status == "queued") {
            j.status = "cancelled";
        }
    }
}

std::string status_json() {
    std::string o = "{\"version\":\"" APP_VERSION "\",";
    std::lock_guard<std::mutex> lk(g_mu);
    o += any_active_locked() ? "\"state\":\"running\"," : "\"state\":\"idle\",";
    o += "\"jobs\":[";
    for (size_t i = 0; i < g_jobs.size(); i++) {
        const Job& j = g_jobs[i];
        if (i) {
            o += ",";
        }
        char num[160];
        snprintf(num, sizeof(num), "\"size\":%llu,\"length\":%llu,\"transferred\":%llu,",
                 (unsigned long long)j.pkg.file_size, (unsigned long long)j.length,
                 (unsigned long long)j.transferred);
        o += "{\"path\":\"" + json_escape(j.pkg.path) + "\",\"content_id\":\"" + j.pkg.content_id +
             "\",\"title_id\":\"" + j.pkg.title_id + "\",\"kind\":\"" + j.pkg.kind + "\"," + num +
             "\"status\":\"" + j.status + "\",\"message\":\"" + json_escape(j.message) + "\"}";
    }
    o += "],\"log\":[";
    std::vector<std::string> tail = log_tail(15);
    for (size_t i = 0; i < tail.size(); i++) {
        if (i) {
            o += ",";
        }
        o += "\"" + json_escape(tail[i]) + "\"";
    }
    o += "]}";
    return o;
}

void shutdown() {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_stop = true;
    }
    g_cv.notify_all();
    if (g_worker.joinable()) {
        g_worker.join();
    }
}

void set_stall_seconds(int sec) { g_stall_sec = sec; }
void set_poll_millis(int ms) { g_poll_ms = ms; }

}  // namespace installer
