#include <signal.h>

#include <cstdlib>
#include <cstring>

#include "http.h"
#include "installer.h"
#include "log.h"
#include "platform.h"
#include "version.h"

#ifndef DATA_DIR
#define DATA_DIR "/data/ps4installer"
#endif
#ifndef HTTP_PORT
#define HTTP_PORT 8284
#endif

int main(int argc, char** argv) {
    // Журнал открываем самым первым: если после запуска payload'а папки DATA_DIR нет,
    // программа не дошла до main() (проблема загрузки, а не нашей логики).
    log_open(DATA_DIR);
    log_printf("ps4installerd %s запущен", APP_VERSION);

    signal(SIGPIPE, SIG_IGN);

    int port = HTTP_PORT;
#ifdef HOST_TEST
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--port") == 0) {
            port = atoi(argv[i + 1]);
        }
        if (strcmp(argv[i], "--stall") == 0) {
            installer::set_stall_seconds(atoi(argv[i + 1]));
        }
        if (strcmp(argv[i], "--poll-ms") == 0) {
            installer::set_poll_millis(atoi(argv[i + 1]));
        }
    }
#else
    (void)argc;
    (void)argv;
#endif

    http_request_quit(port);  // единственный экземпляр: старая копия уходит
    platform::notify(std::string("ps4installer service ") + APP_VERSION + " started");

    int rc = http_serve(port);
    installer::shutdown();
    return rc;
}
