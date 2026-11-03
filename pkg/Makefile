# Сборка PKG-приложения «оффлайн-магазин» (экран и геймпад; ставит игры через службу ps4installerd).
# Нужен OpenOrbis PS4 Toolchain: export OO_PS4_TOOLCHAIN=/home/sony/OpenOrbis-PS4-Toolchain
# Для сборки вместе со службой нужен и PS4_PAYLOAD_SDK (как для корневого make).
#
# Собирать так:   make full
# Он собирает службу в корне, кладёт её внутрь PKG и пересобирает PKG с нуля. Простой "make"
# возьмёт уже готовую службу из ../build (можно получить PKG со старой службой внутри).

# PkgTool.Core работает на .NET: без этих настроек он падает без ICU или ругается на версию .NET.
export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT := 1
export DOTNET_ROLL_FORWARD := LatestMajor

TITLE       := Game Store
TITLE_ID    := OFST00001
CONTENT_ID  := IV0000-OFST00001_00-OFFLINESTORE0000
LIBS        := -lc -lkernel -lc++ -lSceVideoOut -lSceSysmodule -lSceFreeType -lScePad -lSceUserService -lSceSystemService -lSceNet
EXTRAFLAGS  := -O2

# Версия приложения = версия службы (из ../src/version.h).
APPVER      := $(shell sed -n 's/.*APP_VERSION "\(.*\)".*/\1/p' ../src/version.h)

# Порядок важен: create-gp4 собирает папки по списку файлов, и список должен идти по алфавиту папок
# (data, fonts, misc). Каталог игр (data/catalog.pak + data/catalog.json, делает tools/fetch_catalog.py);
# если его нет, в PKG он не кладётся: приложение работает, но без обложек и описаний.
ifneq ($(wildcard ../data/catalog.pak),)
ASSETS      := assets/data/catalog.json assets/data/catalog.pak
endif
ASSETS      += assets/fonts/DejaVuSansMono.ttf assets/fonts/LICENSE-DejaVu.txt assets/misc/ps4installerd.elf

# Системные модули для приложения (без libc.prx приложение сразу вылетает). Берутся из набора OpenOrbis.
LIBMODULES  := sce_module/libc.prx sce_module/libSceFios2.prx

TOOLCHAIN   := $(OO_PS4_TOOLCHAIN)
PROJDIR     := src
INTDIR      := build/obj

CFILES      := $(wildcard $(PROJDIR)/*.c)
CPPFILES    := $(wildcard $(PROJDIR)/*.cpp)
OBJS        := $(patsubst $(PROJDIR)/%.c, $(INTDIR)/%.o, $(CFILES)) $(patsubst $(PROJDIR)/%.cpp, $(INTDIR)/%.o, $(CPPFILES))

CFLAGS      := --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -c $(EXTRAFLAGS) -DAPP_VERSION=\"$(APPVER)\" -isysroot $(TOOLCHAIN) -isystem $(TOOLCHAIN)/include
CXXFLAGS    := $(CFLAGS) -isystem $(TOOLCHAIN)/include/c++/v1
LDFLAGS     := -m elf_x86_64 -pie --script $(TOOLCHAIN)/link.x --eh-frame-hdr -L$(TOOLCHAIN)/lib $(LIBS) $(TOOLCHAIN)/lib/crt1.o

_unused     := $(shell mkdir -p $(INTDIR) sce_sys/about sce_module assets/misc assets/data)

UNAME_S     := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
		CC      := clang
		CCX     := clang++
		LD      := ld.lld
		CDIR    := linux
endif
ifeq ($(UNAME_S),Darwin)
		CC      := /usr/local/opt/llvm/bin/clang
		CCX     := /usr/local/opt/llvm/bin/clang++
		LD      := /usr/local/opt/llvm/bin/ld.lld
		CDIR    := macos
endif
ifneq (,$(findstring MSYS_NT,$(UNAME_S)))
		CC      := clang
		CCX     := clang++
		LD      := ld.lld
		CDIR    := windows
endif

all: $(CONTENT_ID).pkg

$(CONTENT_ID).pkg: pkg.gp4
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core pkg_build $< .

pkg.gp4: eboot.bin sce_sys/about/right.sprx sce_sys/param.sfo sce_sys/icon0.png sce_sys/pic1.png $(LIBMODULES) $(ASSETS)
	$(TOOLCHAIN)/bin/$(CDIR)/create-gp4 -out $@ --content-id=$(CONTENT_ID) --files "$^"

# Системные модули: ищем файл с нужным именем в наборе OpenOrbis (обычно он лежит в samples/*/sce_module).
sce_module/%.prx:
	@src="$$(find '$(TOOLCHAIN)' -name '$*.prx' 2>/dev/null | head -n 1)"; \
	if [ -z "$$src" ]; then echo "Не найден $*.prx в $(TOOLCHAIN). Скопируйте его вручную в pkg/sce_module/"; exit 1; fi; \
	echo "cp $$src $@"; cp "$$src" $@

# Отправить службу на консоль (GoldHEN BinLoader, порт 9090) прямо из этой папки: make send
# Адрес консоли: PS4_HOST (по умолчанию 192.168.0.24). Нужен собранный ../build/ps4installerd.elf (make в корне).
PS4_HOST ?= 192.168.0.24
.PHONY: send
send:
	nc -q0 $(PS4_HOST) 9090 < ../build/ps4installerd.elf

# Служба берётся из корня проекта (сборка: make в корне).
assets/misc/ps4installerd.elf: ../build/ps4installerd.elf
	cp $< $@

assets/data/catalog.pak: ../data/catalog.pak
	cp $< $@

assets/data/catalog.json: ../data/catalog.json
	cp $< $@

sce_sys/icon0.png: gfx/icon0.png
	cp $< $@

sce_sys/pic1.png: gfx/pic1.png
	cp $< $@

sce_sys/about/right.sprx:
	cp $(TOOLCHAIN)/samples/piglet/sce_sys/about/right.sprx $@

sce_sys/param.sfo: Makefile
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_new $@
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ APP_TYPE --type Integer --maxsize 4 --value 1
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ APP_VER --type Utf8 --maxsize 8 --value '01.00'
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ ATTRIBUTE --type Integer --maxsize 4 --value 0
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ CATEGORY --type Utf8 --maxsize 4 --value 'gd'
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ CONTENT_ID --type Utf8 --maxsize 48 --value '$(CONTENT_ID)'
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ DOWNLOAD_DATA_SIZE --type Integer --maxsize 4 --value 0
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ SYSTEM_VER --type Integer --maxsize 4 --value 0
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ TITLE --type Utf8 --maxsize 128 --value '$(TITLE)'
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ TITLE_ID --type Utf8 --maxsize 12 --value '$(TITLE_ID)'
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ VERSION --type Utf8 --maxsize 8 --value '01.00'

eboot.bin: $(OBJS)
	$(LD) $(INTDIR)/*.o -o $(INTDIR)/ps4installer_app.elf $(LDFLAGS)
	$(TOOLCHAIN)/bin/$(CDIR)/create-fself -in=$(INTDIR)/ps4installer_app.elf -out=$(INTDIR)/ps4installer_app.oelf --eboot "eboot.bin" --paid 0x3800000000000010

$(INTDIR)/%.o: $(PROJDIR)/%.c
	$(CC) $(CFLAGS) -o $@ $<

$(INTDIR)/%.o: $(PROJDIR)/%.cpp
	$(CCX) $(CXXFLAGS) -o $@ $<

# Очистка только папок и файлов сборки внутри pkg/.
clean:
	rm -rf $(CONTENT_ID).pkg pkg.gp4 eboot.bin build sce_sys sce_module assets/misc/ps4installerd.elf

# Собирает службу в корне (нужен PS4_PAYLOAD_SDK), затем чисто пересобирает PKG со свежей службой внутри.
.PHONY: full
full:
	$(MAKE) -C ..
	$(MAKE) clean
	$(MAKE) all

.PHONY: all clean
