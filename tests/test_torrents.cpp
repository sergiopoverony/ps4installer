// Проверка запросов через ps4torrent: tests/test_torrents.py поднимает подставной /status и передаёт папку и порт.
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include <sys/stat.h>
#include "torrents.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

static std::string slurp(const std::string& p)
{
    std::string o;
    if (FILE* f = fopen(p.c_str(), "rb")) { char b[512]; size_t n; while ((n = fread(b, 1, sizeof(b), f)) > 0) o.append(b, n); fclose(f); }
    return o;
}

int main(int argc, char** argv)
{
    if (argc < 3) return 2;
    std::string root = std::string(argv[1]) + "/new/dir/torrents";   // папки ещё нет: запрос должен создать
    int port = atoi(argv[2]);
    torrents_configure(root, "127.0.0.1", port);
    const std::string H1 = "4403A31A21012CD5FD942887D1F5C255922B20F9", L1 = "4403a31a21012cd5fd942887d1f5c255922b20f9";
    const std::string L2 = "8b911a531e8867750f1b7ce2eb6406eee98ae4a7", L3 = "551c3ce9b365f750daedb0405425de704002500b";
    CHECK(torrents_clean_hash(H1) == L1);
    CHECK(torrents_clean_hash("nope").empty() && torrents_clean_hash(std::string(40, 'g')).empty());
    CHECK(torrents_magnet(H1, "A & B") == "magnet:?xt=urn:btih:" + L1 + "&tr=http%3A%2F%2Fbt.t-ru.org%2Fann%3Fmagnet&dn=A%20%26%20B");

    std::string err;
    CHECK(!torrents_request("zzz", "x", err) && !err.empty());
    CHECK(torrents_request(H1, "Game One", err));
    CHECK(torrents_pending(H1) && torrents_pending(L1));
    CHECK(slurp(root + "/" + L1 + ".magnet") == torrents_magnet(L1, "Game One") + "\n");
    CHECK(torrents_request(H1, "Game One again", err));           // повтор - не ошибка и файл не меняется
    CHECK(slurp(root + "/" + L1 + ".magnet") == torrents_magnet(L1, "Game One") + "\n");
    CHECK(torrents_request(L2, "Two", err));
    CHECK(torrents_request(L3, "Three", err));

    // подставной /status: L1 удалён из списка клиента после получения (файл .magnet убираем), L2 качается 42%, L3 скачан, 4-й на паузе
    std::vector<std::string> hs;
    hs.push_back(L1); hs.push_back(L2); hs.push_back(L3);
    hs.push_back("1111111111111111111111111111111111111111");   // на паузе
    hs.push_back("2222222222222222222222222222222222222222");   // клиент о нём не знает
    hs.push_back("bad");
    torrents_cancel(L3);                                          // как будто описание получено: файла .magnet нет
    std::map<std::string, ReqStatus> st = torrents_poll(hs);
    CHECK(st[L1].state == RS_WAIT);                               // файл ещё лежит - ждёт
    CHECK(st[L2].state == RS_WAIT);
    CHECK(st[L3].state == RS_DONE && st[L3].pct == 100);
    CHECK(st["1111111111111111111111111111111111111111"].state == RS_PAUSED && st["1111111111111111111111111111111111111111"].pct == 7);
    CHECK(st["2222222222222222222222222222222222222222"].state == RS_GONE);
    CHECK(st["bad"].state == RS_GONE);
    torrents_cancel(L2);
    st = torrents_poll(hs);
    CHECK(st[L2].state == RS_DOWNLOADING && st[L2].pct == 42);
    CHECK(!torrents_pending(L2));
    torrents_cancel(L1);
    CHECK(!torrents_pending(L1));

    // клиент не запущен: порт, на котором никто не слушает
    torrents_configure(root, "127.0.0.1", 1);
    st = torrents_poll(hs);
    CHECK(st[L2].state == RS_NOSVC);
    CHECK(torrents_request(L1, "Game One", err) && torrents_poll(hs)[L1].state == RS_WAIT);   // ожидающие видны и без клиента

    // имя файла magnet и запись на «флешку»
    CHECK(torrents_magnet_filename("Fallout 4: GOTY / ReMOD", "CUSA02557") == "Fallout_4_GOTY_ReMOD_CUSA02557_magnet.txt");
    CHECK(torrents_magnet_filename("\xd0\x98\xd0\xb3\xd1\x80\xd0\xb0", "") == "game_magnet.txt");
    printf("ok\n");
    return 0;
}
