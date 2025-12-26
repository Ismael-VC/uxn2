# Uxn2

An emulator for the [Uxn CPU](https://wiki.xxiivv.com/site/uxn.html) and [Varvara Computer](https://wiki.xxiivv.com/site/varvara.html), written in C99(SDL2). 

## Building 

### Makefile

For your convenience a [Makefile](https://en.wikipedia.org/wiki/Make_(software)#Makefile) is provided. You can run `make install` to build and install the files.

By default, files are installed into `~/.local` but this can be overridden using `PREFIX`:

```sh
# installs files into ~/.local/bin and ~/.local/share
$ make install

# installs files into /opt/uxn/bin and /opt/uxn/share
$ make PREFIX=/opt/uxn install
```

### Graphical

All you need is SDL2.

```sh
cc --std=c99 -Wall -Wno-unknown-pragmas -I/usr/include/SDL2 -D_GNU_SOURCE=1 -D_REENTRANT -DNDEBUG -O2 -g0 -s -L/usr/local/lib  -L/usr/lib -lSDL2 src/uxn2.c -o bin/uxn2
```

## Usage

The first parameter is the rom file, the subsequent arguments will be accessible to the rom, via the [Console vector](https://wiki.xxiivv.com/site/varvara.html#console).

```sh
bin/uxn2 bin/example.rom arg1 arg2
```

## Devices

The file device is _sandboxed_, meaning that it should not be able to read or write outside of the working directory.

- `00` system
- `10` console(+)
- `20` screen
- `80` controller
- `90` mouse
- `a0` file
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

## Need a hand?

The following resources are a good place to start:

* [XXIIVV — uxntal](https://wiki.xxiivv.com/site/uxntal.html)
* [XXIIVV — uxntal reference](https://wiki.xxiivv.com/site/uxntal_reference.html)
* [compudanzas — uxn tutorial](https://compudanzas.net/uxn_tutorial.html)

## Contributing

Submit patches using [`git send-email`](https://git-send-email.io/) to the [~rabbits/public-inbox mailing list](https://lists.sr.ht/~rabbits/public-inbox).
