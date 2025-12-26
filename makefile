SDL_CFLAGS := $(shell sdl2-config --cflags)
SDL_LIBS   := $(shell sdl2-config --libs)

CC_flags=--std=c99 -Wall -Wno-unknown-pragmas
RELEASE_flags=-DNDEBUG -O2 -g0 -s -L/usr/local/lib 
DEBUG_flags=-DDEBUG -Wpedantic -Wshadow -Wextra -Werror=implicit-int -Werror=incompatible-pointer-types -Werror=int-conversion -Wvla -g -Og -fsanitize=address -fsanitize=undefined -L/usr/local/lib

PREFIX=${HOME}/.local

run: bin/uxn2 bin/perifs.rom
	bin/uxn2 bin/perifs.rom
test: bin/uxn2-debug bin/opctest.rom
	bin/uxn2-debug bin/opctest.rom
format:
	clang-format -i src/uxn2.c
grab:
	mkdir -p etc
	mkdir -p etc/utils
	cp ../uxnmin/src/uxnmin.c etc/utils
	cp ../drifblim/etc/drifloon.rom.txt etc/utils/drifloon.rom.txt
	mkdir -p etc/tests
	cp ../uxn-utils/cli/opctest/src/opctest.tal etc/tests/opctest.tal
	cp ../uxn11/etc/tests/perifs.tal etc/tests/perifs.tal
archive:
	cp src/uxn2.c ../oscean/etc/uxn2.c.txt
install: bin/uxn2
	mkdir -p ${PREFIX}/bin
	cp bin/uxn2 ${PREFIX}/bin
uninstall:
	rm -f ${PREFIX}/bin/uxn2 ${PREFIX}/bin/uxncli
clean:
	rm -fr bin

.PHONY: run test clean grab archive install uninstall format

bin/uxn2: src/uxn2.c
	mkdir -p bin
	cc ${CC_flags} $(SDL_CFLAGS) ${RELEASE_flags} $(SDL_LIBS) src/uxn2.c -o bin/uxn2
bin/uxn2-debug: src/uxn2.c
	mkdir -p bin
	cc ${CC_flags} $(SDL_CFLAGS) ${DEBUG_flags} $(SDL_LIBS) src/uxn2.c -o bin/uxn2-debug

# Tools

bin/uxnmin: etc/utils/uxnmin.c
	mkdir -p bin
	cc etc/utils/uxnmin.c -o bin/uxnmin
bin/drifloon.rom: etc/utils/drifloon.rom.txt
	xxd -r -p etc/utils/drifloon.rom.txt bin/drifloon.rom

# Tests

bin/opctest.rom: bin/uxnmin bin/drifloon.rom etc/tests/opctest.tal
	cat etc/tests/opctest.tal | bin/uxnmin bin/drifloon.rom > bin/opctest.rom
bin/perifs.rom: bin/uxnmin bin/drifloon.rom etc/tests/perifs.tal
	cat etc/tests/perifs.tal | bin/uxnmin bin/drifloon.rom > bin/perifs.rom
