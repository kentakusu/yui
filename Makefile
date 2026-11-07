# 結（yui）— Linux / macOS / iOS / Android 用
# Windows は build.bat を使ってください（MSVC）。
#
#   make            yui を作る
#   make san        AddressSanitizer / UBSan 付きで作る
#   make clean      消す

CC      ?= cc
CFLAGS  ?= -O2
WARN     = -std=c99 -Wall -Wextra -pedantic -Wshadow -Wstrict-prototypes
SAN      = -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all

all: yui

yui: yui.c yui_cli.c yui.h
	$(CC) $(WARN) $(CFLAGS) -o $@ yui.c yui_cli.c -lm

san: build/yui_san

build/yui_san: yui.c yui_cli.c yui.h | build
	$(CC) $(WARN) $(SAN) -o $@ yui.c yui_cli.c -lm

build:
	mkdir -p build

# ★テストはまだ書いていません。
#   「あるのに通らない」を避けるため、黙って成功させずに止めます。
test:
	@echo 'テストはまだありません（tests/ が未作成です）。README の「これから」を見てください。'
	@exit 1

clean:
	rm -rf build yui

.PHONY: all san test clean
