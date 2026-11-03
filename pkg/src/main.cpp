#include <stdio.h>
#include <signal.h>
#include <sys/stat.h>
#include <orbis/libkernel.h>

#include "log.h"
#include "ui.h"

#ifndef APP_VERSION
#define APP_VERSION "0.0.0.0"
#endif

int main()
{
    signal(SIGPIPE, SIG_IGN);

    mkdir("/data/ps4installer_app", 0777);
    log_reset();
    logf_("---- ps4installer app %s start ----", APP_VERSION);

    if (!ui_init()) logf_("ui_init failed: no screen");

    for (;;) {
        ui_pump();
        sceKernelUsleep(30000);
    }
    return 0;
}
