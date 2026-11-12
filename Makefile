CC      ?= gcc
CFLAGS  ?= -O2 -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-clobbered -Wno-unknown-warning-option
PREFIX  ?= /usr/local
SRC      = src/util.c src/lexer.c src/parser.c src/types.c src/sema.c src/check.c src/lower.c src/opt.c src/emit.c src/wasm.c src/platform.c src/main.c
STD      = std/core.orb std/math.orb std/strings.orb std/io.orb std/vec.orb std/os_linux.orb std/os_macos.orb std/os_windows.orb std/os_wasm.orb std/web.orb
WEB_EXAMPLES = counter todo form fetch stopwatch starter
BUILD    = build

all: $(BUILD)/orb

$(BUILD)/embed: tools/embed.c
	@mkdir -p $(BUILD)
	$(CC) -O1 -o $@ $<

$(BUILD)/stdlib_data.c: $(BUILD)/embed $(STD) web/orb.js Makefile
	$(BUILD)/embed --runtime web/orb.js $(STD) > $@

$(BUILD)/orb: $(SRC) $(BUILD)/stdlib_data.c src/*.h
	$(CC) $(CFLAGS) -Isrc -o $@ $(SRC) $(BUILD)/stdlib_data.c

test: $(BUILD)/orb
	sh tests/run.sh

# WebAssembly: compiler tests (need node) and browser tests (need python3 + playwright + chromium)
test-wasm: $(BUILD)/orb
	sh tests/wasm/run.sh

test-web: $(BUILD)/orb
	python3 tests/web/run.py

# Compile the browser examples into runnable .wasm files.
web-examples: $(BUILD)/orb
	@for d in $(WEB_EXAMPLES); do \
		$(BUILD)/orb build examples/web/$$d/main.orb --target wasm -o examples/web/$$d/app.wasm || exit 1; \
	done

# Build a clean npm-compatible runtime package in build/packages.
package-web: test-package
	@mkdir -p $(BUILD)/packages
	npm pack --pack-destination $(BUILD)/packages

test-package:
	npm test
	npm pack --dry-run

serve:
	@echo "starter: http://localhost:8000/examples/web/starter/"
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

.PHONY: all test test-wasm test-web web-examples package-web test-package serve bench install vsix clean
