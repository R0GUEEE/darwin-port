# darwin-port — Mach-O loader + libSystem shim
#
#   make            Linux host → bin/darwinld
#   make test       build, then run the compatibility suite
#   make clean
CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Wno-unused-parameter
SRC      = src/main.c src/macho.c src/dso.c src/shim.c src/fmt.c src/plat_posix.c
ASM      = src/fmt_asm.S
BIN      = bin/darwinld

all: $(BIN)

$(BIN): $(SRC) $(ASM) src/macho.h src/dso.h src/shim.h src/plat.h
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $@ $(SRC) $(ASM)
	@chmod +x $@

test: $(BIN)
	sh tests/run.sh

clean:
	rm -rf bin build

.PHONY: all test clean
