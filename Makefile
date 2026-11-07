# 結（yui）— Linux / macOS / iOS / Android 用
# Windows は build.bat を使ってください（MSVC）。
#
#   make              yui を作る
#   make test         テストを流す
#   make test-san     AddressSanitizer / UBSan を付けてテストを流す
#   make test-wasm    wasm（emscripten）でテストを流す。emcc と node が要ります
#   make san          sanitizer 付きの yui を作る
#   make record       期待を今の実装から作り直す（★目で見てから commit）
#   make clean        消す

CC      ?= cc
CFLAGS  ?= -O2
WARN     = -std=c99 -Wall -Wextra -pedantic -Wshadow -Wstrict-prototypes
SAN      = -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all
EMCC    ?= emcc
# NODERAWFS で、OS のファイルをそのまま読み書きします
WASM     = -s NODERAWFS=1 -s ALLOW_MEMORY_GROWTH=1 -s EXIT_RUNTIME=1

CASES    = $(wildcard tests/cases/*.txt)
# テストのビルドだけに付けます。探すたびに索引と全件走査の答えを突き合わせます
TESTDEF  = -DYUI_SELFCHECK

all: yui

yui: yui.c yui_cli.c yui.h
	$(CC) $(WARN) $(CFLAGS) -o $@ yui.c yui_cli.c -lm

san: build/yui_san

build/yui_san: yui.c yui_cli.c yui.h | build
	$(CC) $(WARN) $(SAN) -o $@ yui.c yui_cli.c -lm

build:
	mkdir -p build

# ------------------------------------------------------------------
# テスト
#   ケースは tests/cases/*.txt。書き方は tests/README.md。
#   CLI（yui_cli.c）はまだ通していません。見ているのはライブラリの側です。
# ------------------------------------------------------------------

build/yui_test: yui.c tests/test_yui.c yui.h | build
	$(CC) $(WARN) $(CFLAGS) $(TESTDEF) -o $@ yui.c tests/test_yui.c -lm

build/yui_test_san: yui.c tests/test_yui.c yui.h | build
	$(CC) $(WARN) $(SAN) $(TESTDEF) -o $@ yui.c tests/test_yui.c -lm

build/yui_test.js: yui.c tests/test_yui.c yui.h | build
	$(EMCC) -std=c99 -O2 $(WASM) $(TESTDEF) -o $@ yui.c tests/test_yui.c

test: build/yui_test
	./build/yui_test $(CASES)

test-san: build/yui_test_san
	./build/yui_test_san $(CASES)

test-wasm: build/yui_test.js
	node build/yui_test.js $(CASES)

# 期待を今の実装から作り直します。
# ★出てきたものを目で見てから commit してください。見ないで入れると
#   「今そうなっている」を固めるだけで、テストの意味がなくなります。
record: build/yui_test
	@for f in $(CASES); do \
	  ./build/yui_test --record "$$f" > "$$f.新" && mv "$$f.新" "$$f" && \
	  echo "書き直しました: $$f"; \
	done

clean:
	rm -rf build yui yui_test_tmp.yui yui_test_tmp.yui.断片

.PHONY: all san test test-san test-wasm record clean
