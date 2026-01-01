# Uxn2

A graphical emulator for the [Varvara Computer](https://wiki.xxiivv.com/site/varvara.html), written in C99(SDL2). 

## Building 

You must have [SDL2](https://www.libsdl.org/) installed.

```sh
cc -I/usr/include/SDL2 -DNDEBUG -O2 -g0 -s -lSDL2 src/uxn2.c -o bin/uxn2
```

For your convenience a [Makefile](https://en.wikipedia.org/wiki/Make_(software)#Makefile) is provided. 

```sh
make run
```

You can run `make install` to build and install the files. By default, files are installed into `~/.local` but this can be overridden using `PREFIX`:

```sh
# installs files into /opt/uxn/bin
$ make PREFIX=/opt/uxn install
```

### Plan 9

If you're on plan 9.

```sh
cd src/
cc -p -I/sys/include/npe -I /sys/include/npe/SDL2/ uxn2.c
6l -o uxn2 uxn2.6
```

## Usage

The first parameter is the rom file, the subsequent arguments will be accessible to the rom, via the [Console vector](https://wiki.xxiivv.com/site/varvara.html#console).

```sh
bin/uxn2 bin/example.rom arg1 arg2
```

## Assembler

This repository comes with a copy of the compiled [Drifblim](https://git.sr.ht/~rabbits/drifblim) assembler. 

```sh
cat etc/utils/drifblim.rom.txt | bin/uxn11 etc/utils/xh.rom > bin/drifblim.rom
bin/uxn11 bin/drifblim.rom etc/tests/opctest.tal bin/opctest.rom
```

## Devices

The file device is _sandboxed_, meaning that it should not be able to read or write outside of the working directory.

- `00` system
- `10` console
- `20` screen
- `40` audio
- `50` audio
- `60` audio
- `70` audio
- `80` controller
- `90` mouse
- `a0` file(a)
- `b0` file(b)
- `c0` datetime

## Emulator Controls

- `F1` toggle zoom
- `F2` toggle debugger
- `F4` reboot
- `F5` reboot(soft)

### Buttons

- `LCTRL` A
- `LALT` B
- `LSHIFT` SEL 
- `HOME` START

## SDL2

To build this emulator, you must install [SDL2](https://wiki.libsdl.org/) for your distro. If you are using a package manager:

```sh
sudo pacman -Sy sdl2             # Arch
sudo apt install libsdl2-dev     # Ubuntu
sudo xbps-install SDL2-devel     # Void Linux
brew install sdl2                # OS X
```

## Need a hand?

The following resources are a good place to start:

* [XXIIVV — uxntal](https://wiki.xxiivv.com/site/uxntal.html)
* [XXIIVV — uxntal reference](https://wiki.xxiivv.com/site/uxntal_reference.html)
* [compudanzas — uxn tutorial](https://compudanzas.net/uxn_tutorial.html)

## Contributing

Submit patches using [`git send-email`](https://git-send-email.io/) to the [~rabbits/public-inbox mailing list](https://lists.sr.ht/~rabbits/public-inbox).
