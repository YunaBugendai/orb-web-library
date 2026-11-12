CC      ?= gcc
CFLAGS  ?= -O2 -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-clobbered -Wno-unknown-warning-option
PREFIX  ?= /usr/local
SRC      = src/util.c src/lexer.c src/parser.c src/types.c src/sema.c src/check.c src/lower.c src/opt.c src/emit.c src/wasm.c src/platform.c src/main.c
STD      = std/core.orb std/math.orb std/strings.orb std/io.orb std/vec.orb std/os_linux.orb std/os_macos.orb std/os_windows.orb std/os_wasm.orb std/web.orb
BUILD    = build

all: $(BUILD)/orb

$(BUILD)/embed: tools/embed.c
	@mkdir -p $(BUILD)
	$(CC) -O1 -o $@ $<

$(BUILD)/stdlib_data.c: $(BUILD)/embed $(STD) Makefile
	$(BUILD)/embed $(STD) > $@

$(BUILD)/orb: $(SRC) $(BUILD)/stdlib_data.c src/*.h
	$(CC) $(CFLAGS) -Isrc -o $@ $(SRC) $(BUILD)/stdlib_data.c

test: $(BUILD)/orb
	sh tests/run.sh

# WebAssembly: compiler tests (need node) and browser tests (need python3 + playwright + chromium)
test-wasm: $(BUILD)/orb
	sh tests/wasm/run.sh

test-web: $(BUILD)/orb
	python3 tests/web/run.py

# builds the example web applications next to their index.html; then: make serve
web-examples: $(BUILD)/orb
	for d in counter todo form fetch stopwatch; do $(BUILD)/orb build examples/web/$$d/main.orb --target wasm -o examples/web/$$d/app.wasm || exit 1; done

serve:
	@echo "open http://localhost:8000/examples/web/counter/index.html"
	python3 -m http.server 8000

bench: $(BUILD)/orb
	bash bench/run.sh

install: $(BUILD)/orb
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(BUILD)/orb $(DESTDIR)$(PREFIX)/bin/orb

vsix:
	python3 tools/package-vsix.py $(BUILD)/dist

clean:
	rm -rf $(BUILD)

.PHONY: all test test-wasm test-web web-examples serve bench install vsix clean
