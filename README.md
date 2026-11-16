# PS4 Installer — оффлайн-магазин игр для PS4

Служба-payload (`ps4installerd`, порт 8284) устанавливает PKG через системный BGFT, а приложение «PS4 Store»
(PKG с интерфейсом под геймпад) показывает игры, каталог, очередь установки и меню. Как устроена установка —
в [docs/README.md](docs/README.md).

Проверено на: PS4, прошивка 13.02, GoldHEN 2.4b18.12.

## Обновить исходники
```
cd ~/ps4installer && git pull origin main
```
(пока работа идёт в ветке `dev`, на GitHub тогда: `git pull origin dev`)

## Сборка

Только payload (служба):
```
PS4_PAYLOAD_SDK=$HOME/sdk1402 make clean all
```

PKG вместе со службой:
```
cd pkg && export OO_PS4_TOOLCHAIN=/home/sony/OpenOrbis-PS4-Toolchain PS4_PAYLOAD_SDK=$HOME/sdk1402 && make clean && make full
```

PKG без каталога игр (быстрее, для проверки интерфейса) — `make full-nocatalog` вместо `make full`.

## Отправка на консоль
- Служба: `make send` в корне (BinLoader GoldHEN, порт 9090; адрес консоли `PS4_HOST`, по умолчанию 192.168.0.24).
- Службу приложение при запуске отправляет само, если она не запущена или версия не совпадает.
- PKG: `cd pkg && make send` отправляет службу; сам PKG ставится с флешки или любым привычным способом.

## Проверки на ПК
`make test` в корне — служба собирается с `-DHOST_TEST` без консоли; тесты каталога, PKG и языков.

## Документация
Папка [docs/](docs/): установка PKG, BGFT, API службы, жизненный цикл, известные проблемы.
