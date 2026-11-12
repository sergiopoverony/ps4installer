# Системная установка через libSceBgft

Всё это в `src/platform.cpp` (блок `#else  // консоль`) и `src/installer.cpp`. В режиме `HOST_TEST` (проверки на ПК) вместо
вызовов консоли стоят заглушки с теми же именами, поэтому логика очереди проверяется без приставки.

## 1. Загрузка библиотеки
`bgft_load()` ищет и открывает через `dlopen` файл **`libSceBgft.sprx`** в папках по порядку:
`/system/common/lib/`, `/system/priv/lib/`, `/system_ex/common_ex/lib/`, `/system_ex/priv_ex/lib/`.

Имена функций в разных справочниках различаются («с ServiceInt» и без), поэтому `find_sym()` пробует оба варианта через `dlsym`:

| Назначение | Первое имя | Запасное имя |
|-----------|-----------|--------------|
| инициализация | `sceBgftServiceIntInit` | `sceBgftInitialize` |
| регистрация задачи | `sceBgftServiceIntDownloadRegisterTaskByStorageEx` | `sceBgftDownloadRegisterTaskByStorageEx` |
| запуск | `sceBgftServiceDownloadStartTask` | `sceBgftDownloadStartTask` |
| ход | `sceBgftServiceDownloadGetProgress` | `sceBgftDownloadGetProgress` |

Какие именно имена нашлись — видно в ответе `GET /health` (поле `bgft`) и в журнале (`bgft symbols: ...`).

## 2. Инициализация (аккуратно!)
```c
struct BgftInitParams { void* heap; size_t heapSize; };
```
- Куча — отдельная анонимная область `mmap(NULL, 1 MiB, PROT_READ|PROT_WRITE, MAP_ANON|MAP_PRIVATE)`, занулённая.
  Статический буфер в `.bss` и `malloc` уже подводили (инициализация падала), поэтому только `mmap`.
- Вызов: `init(&params)`, ожидаемый результат `0`.
- Результат **`0x80990001`** после замены службы на новую версию означает «в этом процессе BGFT уже инициализирован старой
  копией» — считаем успехом.
- **Защита от падения**: инициализация однажды роняла службу (сигнал 11, консоль зависала). Перед вызовом пишется файл-метка
  `/data/ps4installer/bgft_init_<APP_VERSION>.marker`, после успешного возврата метка удаляется. Если при следующем запуске
  метка этой версии на месте — init пропускается (считаем BGFT уже готовым). Новая версия службы пробует init заново.
- Вызывается один раз за процесс (`g_loaded`), результат запоминается.

## 3. Регистрация задачи установки локального файла
```c
struct BgftDownloadParam {          // sizeof == 0x68 (проверяется static_assert)
    int  userId;                    // 0
    int  entitlementType;           // 5
    const char* id;                 // Content ID (36 символов)
    const char* contentUrl;         // путь к .pkg (локальный, см. ниже)
    const char* contentExUrl;       // NULL
    const char* contentName;        // Content ID (для имени задачи)
    const char* iconPath;           // ""
    const char* skuId;              // NULL
    uint32_t    option;             // 0x8 = BGFT_OPT_FORCE_UPDATE
    const char* playgoScenarioId;   // "0"
    const char* releaseDate;        // NULL
    const char* packageType;        // NULL
    const char* packageSubType;     // NULL
    unsigned long packageSize;      // 0
};
struct BgftDownloadParamEx { BgftDownloadParam params; unsigned int slot; };  // slot = 0
```
Вызов: `int ret = register_ex(&param, &taskId);` — `0` успех, `taskId` нужен дальше.

Значения, которые реально задаются (`bgft_register_local`): `entitlementType = 5`, `id = Content ID`, `contentUrl = путь`,
`contentName = Content ID`, `iconPath = ""`, `playgoScenarioId = "0"`, `option = FORCE_UPDATE (0x8)`, `slot = 0`; всё остальное — нули
(структура целиком `memset(0)`).

**Путь `contentUrl`**: `bgft_path(path)` — если путь начинается с `/data/`, то добавляется префикс `/user`
(`/data/pkg/a.pkg` → `/user/data/pkg/a.pkg`). Пути флешек (`/mnt/usb0/...`) передаются как есть.

Раскладка структур взята из открытых заголовков flatz/ps4_stub_lib_maker_v2 (`bgft.h`). Проверена на консоли: установка работает.

## 4. Запуск и слежение
- `start(taskId)` → `0`.
- Раз в секунду (`g_poll_ms`, 1000) `get_progress(taskId, &BgftTaskProgress)`:
```c
struct BgftTaskProgress {
    unsigned int bits;  int errorResult;
    unsigned long length, transferred;          // текущая часть
    unsigned long lengthTotal, transferredTotal;
    unsigned int numIndex, numTotal, restSec, restSecTotal;
    int preparingPercent, localCopyPercent;
};
```
Служба использует: `errorResult`, `length`, `transferred`.
- `errorResult != 0` → задача `failed`, текст `ошибка установки: 0xXXXXXXXX`, уведомление на экран `ps4installer: error 0x... CUSAxxxxx`.
- `length > 0 && transferred >= length` → `done`.
- Если `transferred` не меняется **180 секунд** (`g_stall_sec`) → `failed`, «установка не продвигается».
- На каждом шаге вызывается `sceSystemServicePowerTick()` (`power_tick`), чтобы консоль не уходила в режим покоя.

## 5. Коды ошибок, которые обрабатываются особо
| Код | Смысл | Что делает служба |
|-----|-------|-------------------|
| `0x80990015` | система занята (`BGFT_ERR_BUSY`) | повторяет регистрацию до 30 раз с паузой (≈ 3×1 с), всего ~90 с |
| `0x80990088` | уже установлено (`BGFT_ERR_ALREADY_INSTALLED`) | помечает `already_installed` («уже установлено»), это не ошибка |
| `0x80990001` | BGFT уже инициализирован | при init считается успехом |
| прочие | регистрация/запуск/ход | `failed` + код в `message`, уведомление на экран |

## 6. Статусы задачи (`status` в `/status`)
`queued` → `registering` → `installing` → `done` | `already_installed` | `failed` | `cancelled`.
Активные: `queued`, `registering`, `installing`. Очередь «идёт», пока есть хоть один активный.

## 7. Уведомления на экране
- По окончании **всей пачки** без ошибок (`notify_if_finished`): `notify_icon("<Название>\nУстановлено", "/user/appmeta/<TITLE_ID>/icon0.png")`.
- При ошибке: `notify("ps4installer: error 0x... <TITLE_ID>")`.
- Реализация в `platform.cpp` (`notify`, `notify_icon`); на ПК просто пишется в журнал (`NOTIFY: ...`).

## 8. Как повторить это в своём проекте (кратко)
1. Запустите код как payload с привилегиями GoldHEN (иначе BGFT не откроется).
2. `dlopen("/system/common/lib/libSceBgft.sprx")`, получите четыре функции (`dlsym`).
3. `init` с кучей из `mmap` (1 МиБ).
4. Для каждого `.pkg`: прочитайте Content ID из заголовка (смещение `0x40`, 36 байт ASCII), заполните `BgftDownloadParamEx`
   как в п. 3, `register_ex` → `start` → опрос `get_progress`.
5. Ставьте **по одному** пакету; патчи и DLC — только после их игры.
6. Учтите `0x80990015` (повторять) и `0x80990088` (уже есть).
