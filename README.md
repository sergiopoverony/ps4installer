# PS4 Installer

**Offline game store for a jailbroken PS4 · Оффлайн-магазин игр для PS4 с GoldHEN**

[Русский](#русский) · [English](#english) · [docs/](docs/README.md) · License: GPL-3.0-or-later (see `LICENSE`, `THIRD_PARTY.md`)

---

## English

A small service (`ps4installerd`, payload for GoldHEN) installs PKG files through the system BGFT, and the app **PS4 Store**
(a PKG with a gamepad interface) shows the games found on your drives, the installed games, an optional games catalog,
the install queue and a menu. It works **without internet**: games are installed from PKG files you put on a USB drive or in `/data/pkg`.

Tested on: PS4, firmware 13.02, GoldHEN 2.4b18.12. Other firmware versions are not tested.

### Using it
1. Console: GoldHEN running, *Servers settings → Enable Payloader Server* turned on.
2. Install the PKG of the app like any other homebrew PKG.
3. Put game PKG files on a USB drive (exFAT or FAT32; any folder, up to 6 levels deep) or in `/data/pkg`.
4. Start **PS4 Store**. It sends the service to the console itself and registers it for autorun.
5. Controls: **L1 / R1** — tabs, **Cross** — open / confirm, **Circle** — close / cancel, **Triangle** — delete (installed game, or the game's files),
   **Square** — refresh, **Options** — menu (language, quit). Language follows the console (Russian and English so far) and can be changed in the menu.

The store never downloads games and does not include any. The games catalog (covers, descriptions) is optional and is **not** part
of the release; see [Building a catalog](#building-a-catalog).

### Building
```
cd ~/ps4installer && git pull origin dev
```
Service only (payload):
```
PS4_PAYLOAD_SDK=$HOME/sdk1402 make clean all
```
PKG together with the service:
```
cd pkg && export OO_PS4_TOOLCHAIN=/home/sony/OpenOrbis-PS4-Toolchain PS4_PAYLOAD_SDK=$HOME/sdk1402 && make clean && make full
```
Without the catalog (faster, what a release contains): `make full-nocatalog` instead of `make full`.
Send the service to the console: `make send` (BinLoader, port 9090; console address `PS4_HOST`, default 192.168.0.24).
Checks on a PC (no console needed): `make test`.

### Building a catalog
`tools/fetch_catalog.py` collects titles, covers, screenshots and descriptions into `data/` (ignored by git) and packs them into
`data/catalog.pak`; `make full` then embeds it into the PKG. Catalog content belongs to its owners: use it for yourself, do not publish it.

### Notice
For use with games and consoles you own. No games, keys or copyrighted artwork are distributed here. Not affiliated with Sony.
The software is provided without warranty (GPL-3.0).

---

## Русский

Небольшая служба (`ps4installerd`, payload для GoldHEN) устанавливает PKG через системный BGFT, а приложение **PS4 Store**
(PKG с интерфейсом под геймпад) показывает игры с ваших дисков, установленные игры, необязательный каталог игр,
очередь установки и меню. Работает **без интернета**: игры ставятся из PKG-файлов, которые вы положили на флешку или в `/data/pkg`.

Проверено на: PS4, прошивка 13.02, GoldHEN 2.4b18.12. Другие прошивки не проверялись.

### Как пользоваться
1. На консоли запущен GoldHEN, включено *Servers settings → Enable Payloader Server*.
2. Установите PKG приложения, как любой другой PKG.
3. Положите PKG игр на флешку (exFAT или FAT32; любая папка, глубина до 6 уровней) или в `/data/pkg`.
4. Запустите **PS4 Store**: служба отправляется на консоль сама и ставится в автозапуск.
5. Управление: **L1 / R1** — вкладки, **Крест** — открыть / подтвердить, **Круг** — закрыть / отмена, **Треугольник** — удалить
   (установленную игру или файлы игры), **Квадрат** — обновить, **Options** — меню (язык, выход). Язык берётся из консоли
   (пока русский и английский), меняется в меню.

Магазин ничего не скачивает и не содержит игр. Каталог игр (обложки, описания) необязателен и **не входит** в релиз —
см. [Как собрать каталог](#как-собрать-каталог).

### Сборка
```
cd ~/ps4installer && git pull origin dev
```
Только payload (служба):
```
PS4_PAYLOAD_SDK=$HOME/sdk1402 make clean all
```
PKG вместе со службой:
```
cd pkg && export OO_PS4_TOOLCHAIN=/home/sony/OpenOrbis-PS4-Toolchain PS4_PAYLOAD_SDK=$HOME/sdk1402 && make clean && make full
```
Без каталога (быстрее, именно так собирается релиз): `make full-nocatalog` вместо `make full`.
Отправить службу на консоль: `make send` (BinLoader, порт 9090; адрес консоли `PS4_HOST`, по умолчанию 192.168.0.24).
Проверки на ПК без консоли: `make test`.

### Как собрать каталог
`tools/fetch_catalog.py` собирает названия, обложки, скриншоты и описания в `data/` (git их игнорирует) и упаковывает в
`data/catalog.pak`; `make full` вшивает его в PKG. Содержимое каталога принадлежит его владельцам: используйте для себя, не публикуйте.

### Уведомление
Для игр и консолей, которыми вы владеете. Здесь не распространяются игры, ключи и чужие изображения. Проект не связан с Sony.
Программа поставляется без гарантий (GPL-3.0).
