SDL_CFLAGS := $(shell sdl2-config --cflags)
SDL_LIBS   := $(shell sdl2-config --libs)

RELEASE_flags=-DNDEBUG -O2 -g0 -s -L/usr/local/lib 
DEBUG_flags=-DDEBUG -Wpedantic -Wshadow -Wextra -Werror=implicit-int -Werror=incompatible-pointer-types -Werror=int-conversion -Wvla -g -Og -fsanitize=address -fsanitize=undefined -L/usr/local/lib

PREFIX=${HOME}/.local

run: bin/uxn2 bin/perifs.rom
	bin/uxn2 bin/perifs.rom
test: bin/uxn2-debug tests
	@ -bin/uxn2-debug
	@ -bin/uxn2-debug -v
	@ bin/uxn2-debug bin/opctest.rom
	@ bin/uxn2-debug bin/system.rom
	@ printf "ghi" | bin/uxn2-debug bin/console.rom "abc" "def"
	@ bin/uxn2-debug bin/file.rom
	@ bin/uxn2-debug bin/datetime.rom
	@ bin/uxn2-debug bin/audio.rom
format:
	clang-format -i src/uxn2.c
grab:
	mkdir -p etc
	mkdir -p etc/utils
	cp ../drifblim/etc/drifblim.rom.txt etc/utils/
	mkdir -p etc/tests
	cp ../uxn11/etc/tests/* etc/tests/
archive:
	cp src/uxn2.c ../oscean/etc/uxn2.c.txt
	cp etc/tests/audio.tal ../oscean/etc/varvara.audio.tal.txt
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
	cc $(SDL_CFLAGS) ${RELEASE_flags} src/uxn2.c -o bin/uxn2 $(SDL_LIBS)
bin/uxn2-debug: src/uxn2.c
	mkdir -p bin
	cc $(SDL_CFLAGS) ${DEBUG_flags} src/uxn2.c -o bin/uxn2-debug $(SDL_LIBS)

# Tools

bin/drifblim.rom: bin/uxn2 etc/utils/drifblim.rom.txt
	@ cat etc/utils/drifblim.rom.txt | bin/uxn2 etc/utils/xh.rom > bin/drifblim.rom

# Tests

tests: bin/opctest.rom bin/system.rom bin/console.rom bin/perifs.rom bin/file.rom bin/datetime.rom bin/audio.rom

bin/opctest.rom: bin/uxn2 bin/drifblim.rom etc/tests/opctest.tal
	@ bin/uxn2 bin/drifblim.rom etc/tests/opctest.tal bin/opctest.rom
bin/system.rom: bin/uxn2 bin/drifblim.rom etc/tests/system.tal
	@ bin/uxn2 bin/drifblim.rom etc/tests/system.tal bin/system.rom
bin/console.rom: bin/uxn2 bin/drifblim.rom etc/tests/console.tal
	@ bin/uxn2 bin/drifblim.rom etc/tests/console.tal bin/console.rom
bin/audio.rom: bin/uxn2 bin/drifblim.rom etc/tests/audio.tal
	@ bin/uxn2 bin/drifblim.rom etc/tests/audio.tal bin/audio.rom
bin/perifs.rom: bin/uxn2 bin/drifblim.rom etc/tests/perifs.tal
	@ bin/uxn2 bin/drifblim.rom etc/tests/perifs.tal bin/perifs.rom
bin/file.rom: bin/uxn2 bin/drifblim.rom etc/tests/file.tal
	@ bin/uxn2 bin/drifblim.rom etc/tests/file.tal bin/file.rom
bin/datetime.rom: bin/uxn2 bin/drifblim.rom etc/tests/datetime.tal
	@ bin/uxn2 bin/drifblim.rom etc/tests/datetime.tal bin/datetime.rom
