# Служба ps4installerd (payload для GoldHEN BinLoader / AutoRun).
#
#   make        собрать build/ps4installerd.elf
#   make send   отправить на консоль (BinLoader, порт 9090): make send PS4_HOST=192.168.0.24
#   make test   проверки на ПК (сборка с -DHOST_TEST, без консоли)
#   make deps   показать зависимости собранного ELF (должно быть минимум)

# orbis-ld берёт путь к линкер-скрипту из переменной ОКРУЖЕНИЯ, поэтому экспорт обязателен
# (иначе: "cannot find linker script /ldscripts/elf_x86_64.x").
PS4_PAYLOAD_SDK ?= /opt/ps4-payload-sdk
export PS4_PAYLOAD_SDK

PS4_HOST ?= 192.168.0.24
NAME     := ps4installerd

SRCS := src/main.cpp src/log.cpp src/pkg.cpp src/json.cpp src/platform.cpp src/installer.cpp src/http.cpp

CXX      := $(PS4_PAYLOAD_SDK)/bin/orbis-clang++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -pthread -Isrc
LDFLAGS  := -lSceSystemService

.PHONY: all clean send test deps

all: build/$(NAME).elf

build/$(NAME).elf: $(SRCS) $(wildcard src/*.h)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(SRCS) -o $@ $(LDFLAGS)

send: build/$(NAME).elf
	nc -q0 $(PS4_HOST) 9090 < build/$(NAME).elf

deps: build/$(NAME).elf
	readelf -dW build/$(NAME).elf | grep NEEDED
	@echo "--- неразрешённые импорты:"
	readelf --dyn-syms -W build/$(NAME).elf | awk '$$7=="UND"{print $$8}' | sort

test:
	python3 tests/test_service.py
	python3 tests/test_fetch_catalog.py
	python3 tests/test_translate_catalog.py
	python3 tests/test_pkg_games.py
	python3 tests/test_catalog.py

clean:
	rm -rf build
