# HTTP-интерфейс службы ps4installerd

Служба слушает **порт 8284** на всех интерфейсах (`INADDR_ANY`). Приложение обращается к `127.0.0.1:8284`.
Ответы — JSON (`application/json`), кроме картинок. Ошибки: `{"ok":false,"error":"код","detail":"пояснение"}` с HTTP-статусом.
Заголовок запроса, о котором стоит знать: тело POST — JSON в UTF-8 с `Content-Length`. Размер запроса ограничен (~16 КиБ на заголовки).

> Безопасность: службу видно из сети. Пути проверяются `path_allowed`, удаление игры самого магазина запрещено,
> но авторизации нет. Возможное улучшение (в TODO): слушать только `127.0.0.1`.

| Метод и путь | Что делает |
|--------------|-----------|
| `GET /health` | `{"ok":true,"version":"0.1.0.NN","fw":"13.02","bgft":"имена найденных функций или error: ..."}`. Заодно пробует загрузить BGFT |
| `GET /scan[?fresh=1]` | список пакетов: `{"ok":true,"count":N,"packages":[{path,content_id,title_id,kind,size,pkg_size,zero_blocks,patch,title,ver,backport,incomplete,rus,dlc_name}]}`; отсортирован по размеру (от большего). Кэш 30 с; `fresh=1` обходит кэш, но не чаще раза в 3 с |
| `GET /free` | `{"ok":true,"free_bytes":N,"total_bytes":M}` для `/user` |
| `GET /installed` | `{"ok":true,"title_ids":[...]}` — установленные приложения |
| `GET /installed_games` | `{"ok":true,"games":[{"id":"CUSA..","title":".."}]}` — название из `param.sfo` по путям `/user/appmeta/<ID>/param.sfo`, `/system_data/priv/appmeta/...`, `/user/app/<ID>/sce_sys/param.sfo` |
| `GET /appicon?id=CUSA..` | `icon0.png` установленной игры |
| `GET /icon?path=/mnt/usb0/x.pkg` | значок из самого PKG (запись `0x1200`); путь должен пройти `path_allowed` |
| `POST /install` | тело `{"paths":["/mnt/usb0/a.pkg", ...]}` (до 500). Проверяет, планирует и ставит в очередь. Ответ: `{"ok":true,"queued":N,"order":[путь,...],"skipped":[{path,reason}],"need":N,"free":M}` |
| `GET /status` | `{"version","state":"running|idle","jobs":[{path,content_id,title_id,kind,size,length,transferred,status,message}],"log":[последние 15 строк журнала]}` |
| `POST /cancel` | снимает всё, что ещё не начато (текущая установка продолжается) |
| `POST /launch` | `{"title_ids":["CUSA00000"]}` — запускает установленную игру (`sceSystemServiceLaunchApp`, пользователь `0xFF`) |
| `POST /uninstall` | `{"title_ids":["CUSA00000"]}` — удаляет установленное приложение (`sceAppInstUtilAppUnInstall`). `409 busy`, пока идёт установка; `403 self` для самого магазина `OFST00001`; `404 not_installed` |
| `POST /delete_files` | `{"paths":[...]}` — физически удаляет `.pkg` с носителя. Только пути из `path_allowed`; если хоть один не из разрешённых — `403 path_not_allowed` и ничего не удалено; `409 busy` во время установки. Ответ `{"ok":true|false,"deleted":N,"failed":"пути через запятую"}` |
| `POST /quit` | служба завершается (используется при замене на новую версию) |

Коды ошибок `/install`: `400 bad_request` (пустой список, не JSON, >500), `400 bad_path`, `422 bad_pkg` (заголовок не разобран:
«нет сигнатуры PKG», «некорректный content id», «неподдерживаемый тип пакета»…), `409 no_space` (+ `need`, `free`),
`500 bgft` (не загрузилась библиотека или init).

Журнал службы: `/data/ps4installer/log.txt` (открывается самым первым действием в `main`). Метка аварии BGFT:
`/data/ps4installer/bgft_init_<версия>.marker`.

Единственный экземпляр: при запуске служба шлёт `POST /quit` старой копии на `127.0.0.1:8284` (`http_request_quit`), затем занимает порт.
Приложение сравнивает версию службы со своей (`APP_VERSION`) и при несовпадении отправляет новую службу.
