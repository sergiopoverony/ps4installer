#include "patches.h"
#include "log.h"

const char* PATCH_URL = "";   // TODO: адрес с заплатками (решим позже)

PatchCheck patches_check_online(int lastApplied, std::string& info)
{
    (void)lastApplied;
    if (!PATCH_URL || !*PATCH_URL) { info = "адрес заплаток не задан"; return PC_NOT_CONFIGURED; }
    // TODO: GET <PATCH_URL>/index.json -> номер последней заплатки; если он больше lastApplied, скачать catalog_N.json и catalog_N.pak
    // в /data/ps4installer_app/catalog/ (во временное имя, потом rename), проверить, что json разбирается, и вернуть PC_DOWNLOADED.
    info = "загрузка из интернета ещё не реализована";
    return PC_NOT_CONFIGURED;
}
