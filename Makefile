CC      ?= gcc
CFLAGS  ?= -O2 -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-clobbered
PREFIX  ?= /usr/local
SRC      = src/util.c src/lexer.c src/parser.c src/types.c src/sema.c src/check.c src/lower.c src/opt.c src/emit.c src/main.c
STD      = std/core.orb std/math.orb std/strings.orb std/io.orb std/vec.orb
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

bench: $(BUILD)/orb
	bash bench/run.sh

install: $(BUILD)/orb
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(BUILD)/orb $(DESTDIR)$(PREFIX)/bin/orb

clean:
	rm -rf $(BUILD)

.PHONY: all test bench install clean
