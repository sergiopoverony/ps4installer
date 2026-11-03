#include "log.h"

#include <sys/stat.h>
#include <sys/types.h>

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <deque>
#include <mutex>

static std::mutex g_mu;
static FILE* g_fp = nullptr;
static std::deque<std::string> g_tail;

void log_open(const char* dir) {
    std::lock_guard<std::mutex> lk(g_mu);
    mkdir(dir, 0777);
    std::string path = std::string(dir) + "/log.txt";
    g_fp = fopen(path.c_str(), "a");
}

void log_printf(const char* fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    char ts[32] = "";
    time_t t = time(nullptr);
    struct tm tmv;
    if (localtime_r(&t, &tmv)) {
        strftime(ts, sizeof(ts), "%H:%M:%S", &tmv);
    }
    std::string line = std::string(ts) + " " + msg;

    std::lock_guard<std::mutex> lk(g_mu);
    if (g_fp) {
        fprintf(g_fp, "%s\n", line.c_str());
        fflush(g_fp);
    }
#ifdef HOST_TEST
    fprintf(stderr, "%s\n", line.c_str());
#endif
    g_tail.push_back(line);
    while (g_tail.size() > 60) {
        g_tail.pop_front();
    }
}

std::vector<std::string> log_tail(size_t max_lines) {
    std::lock_guard<std::mutex> lk(g_mu);
    std::vector<std::string> out;
    size_t n = g_tail.size() < max_lines ? g_tail.size() : max_lines;
    for (size_t i = g_tail.size() - n; i < g_tail.size(); i++) {
        out.push_back(g_tail[i]);
    }
    return out;
}
