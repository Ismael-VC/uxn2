SDL_CFLAGS := $(shell sdl2-config --cflags)
SDL_LIBS   := $(shell sdl2-config --libs)

CC_flags=--std=c89 -Wall -Wno-unknown-pragmas
RELEASE_flags=-DNDEBUG -O2 -g0 -s -L/usr/local/lib 
DEBUG_flags=-DDEBUG -Wpedantic -Wshadow -Wextra -Werror=implicit-int -Werror=incompatible-pointer-types -Werror=int-conversion -Wvla -g -Og -fsanitize=address -fsanitize=undefined -L/usr/local/lib
FILES=src/uxn.c src/devices/file.c src/devices/screen.c src/devices/audio.c src/uxn2.c

run: bin/uxn2
	@ bin/uxn2 bin/perifs.rom
test: bin/uxn2-debug
	@ bin/uxn2-debug bin/perifs.rom
format:
	@ clang-format -i src/uxn2.c
grab:
	cp ../drifblim/etc/drifblim.rom.txt etc/utils/drifblim.rom.txt
	cp ../uxn-utils/cli/opctest/src/opctest.tal etc/tests/opctest.tal
archive:
	cp src/uxn2.c ../oscean/etc/uxn2.c.txt
clean:
	@ rm -fr bin

.PHONY: run test debug install uninstall format clean grab archive

bin/uxn2: src/uxn2.c
	@ mkdir -p bin
	@ cp ../uxn11/bin/perifs.rom bin/
	@ cc ${CC_flags} $(SDL_CFLAGS) ${RELEASE_flags} $(SDL_LIBS) ${FILES} -o bin/uxn2
bin/uxn2-debug: src/uxn2.c
	@ mkdir -p bin
	@ cp ../uxn11/bin/perifs.rom bin/
	@ cc ${CC_flags} $(SDL_CFLAGS) ${DEBUG_flags} $(SDL_LIBS) ${FILES} -o bin/uxn2-debug
