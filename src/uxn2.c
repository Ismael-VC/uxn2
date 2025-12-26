#define _XOPEN_SOURCE 500
#include <stdio.h>
#include <time.h>
#include <string.h>
#include <unistd.h>

#include "uxn.h"

#pragma GCC diagnostic push
#pragma clang diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma clang diagnostic ignored "-Wtypedef-redefinition"
#include <SDL.h>
#if defined(_WIN32) && defined(_WIN32_WINNT) && _WIN32_WINNT > 0x0602
#include <processthreadsapi.h>
#elif defined(_WIN32)
#include <windows.h>
#endif
#ifndef __plan9__
#define USED(x) (void)(x)
#endif
#pragma GCC diagnostic pop
#pragma clang diagnostic pop

/*
Copyright (c) 2021-2025 Devine Lu Linvega, Andrew Alderwick

Permission to use, copy, modify, and distribute this software for any
purpose with or without fee is hereby granted, provided that the above
copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
WITH REGARD TO THIS SOFTWARE.
*/

#define BANKS 0x10
#define BANKS_CAP BANKS * 0x10000
#define WIDTH 64 * 8
#define HEIGHT 40 * 8

Uxn uxn;
int console_vector;

static SDL_Window *emu_window;
static SDL_Texture *emu_texture;
static SDL_Renderer *emu_renderer;
static SDL_Rect emu_viewport;
static SDL_AudioDeviceID audio_id;
static SDL_Thread *stdin_thread;

/* devices */

/*
@|System ------------------------------------------------------------ */

char *boot_path;
Uint16 metadata_addr;

#define METADATA_LEN 256
/* allocate one more to ensure a null terminator */
char metadata_buffer[METADATA_LEN + 1];

static void
system_print(char *name, Stack *s)
{
	Uint8 i;
	fprintf(stderr, "%s ", name);
	for(i = s->ptr - 8; i != (Uint8)(s->ptr); i++)
		fprintf(stderr, "%02x%c", s->dat[i], i == 0xff ? '|' : ' ');
	fprintf(stderr, "<%02x\n", s->ptr);
}

static int
system_load(Uint8 *ram, char *rom_path)
{
	FILE *f = fopen(rom_path, "rb");
	if(f) {
		int i = 0, l = fread(ram, PAGE_SIZE - PAGE_PROGRAM, 1, f);
		while(l && ++i < BANKS)
			l = fread(ram + PAGE_SIZE * i - PAGE_PROGRAM, PAGE_SIZE, 1, f);
		fclose(f);
	}
	return !!f;
}

int
system_error(char *msg, const char *err)
{
	fprintf(stderr, "%s: %s\n", msg, err), fflush(stderr);
	return 0;
}

int
system_boot(Uint8 *ram, char *rom_path, int has_args)
{
	uxn.ram = ram;
	boot_path = rom_path;
	uxn.dev[0x17] = has_args;
	if(ram && system_load(uxn.ram + PAGE_PROGRAM, rom_path))
		return uxn_eval(PAGE_PROGRAM);
	return 0;
}

int
system_reboot(int soft)
{
	int i;
	for(i = 0x0; i < 0x100; i++) uxn.dev[i] = 0;
	for(i = soft ? 0x100 : 0; i < PAGE_SIZE; i++) uxn.ram[i] = 0;
	uxn.wst.ptr = uxn.rst.ptr = 0;
	return system_boot(uxn.ram, boot_path, 0);
}

static void
system_expansion(const Uint16 exp)
{
	Uint8 *aptr = uxn.ram + exp;
	unsigned short length = PEEK2(aptr + 1), limit;
	unsigned int bank = PEEK2(aptr + 3) * 0x10000;
	unsigned int addr = PEEK2(aptr + 5);
	if(uxn.ram[exp] == 0x0) {
		unsigned int dst_value = uxn.ram[exp + 7];
		unsigned short a = addr;
		if(bank < BANKS_CAP)
			for(limit = a + length; a != limit; a++)
				uxn.ram[bank + a] = dst_value;
	} else if(uxn.ram[exp] == 0x1) {
		unsigned int dst_bank = PEEK2(aptr + 7) * 0x10000;
		unsigned int dst_addr = PEEK2(aptr + 9);
		unsigned short a = addr, c = dst_addr;
		if(bank < BANKS_CAP && dst_bank < BANKS_CAP)
			for(limit = a + length; a != limit; c++, a++)
				uxn.ram[dst_bank + c] = uxn.ram[bank + a];
	} else if(uxn.ram[exp] == 0x2) {
		unsigned int dst_bank = PEEK2(aptr + 7) * 0x10000;
		unsigned int dst_addr = PEEK2(aptr + 9);
		unsigned short a = addr + length - 1, c = dst_addr + length - 1;
		if(bank < BANKS_CAP && dst_bank < BANKS_CAP)
			for(limit = addr - 1; a != limit; a--, c--)
				uxn.ram[dst_bank + c] = uxn.ram[bank + a];
	} else
		fprintf(stderr, "Unknown command: %s\n", &uxn.ram[exp]);
}

char *
metadata_read_name(void)
{
	int i;
	for(i = 0; i < METADATA_LEN + 1; i++)
		metadata_buffer[i] = 0;
	if(metadata_addr == 0)
		return metadata_buffer;
	if(uxn.ram[metadata_addr] != 0x00)
		return metadata_buffer;
	for(i = 1; i < METADATA_LEN; i++) {
		char c = uxn.ram[metadata_addr + i];
		if(c == 0x00 || c == 0x0a)
			break;
		metadata_buffer[i - 1] = c;
	}
	return metadata_buffer;
}

/* IO */

Uint8
system_dei(Uint8 addr)
{
	switch(addr) {
	case 0x4: return uxn.wst.ptr;
	case 0x5: return uxn.rst.ptr;
	default: return uxn.dev[addr];
	}
}

void
system_deo(Uint8 port)
{
	switch(port) {
	case 0x3: {
		system_expansion(PEEK2(uxn.dev + 2));
		break;
	}
	case 0x4:
		uxn.wst.ptr = uxn.dev[4];
		break;
	case 0x5:
		uxn.rst.ptr = uxn.dev[5];
		break;
	case 0x7:
		metadata_addr = PEEK2(&uxn.dev[0x6]);
		break;
	case 0xe:
		system_print("WST", &uxn.wst);
		system_print("RST", &uxn.rst);
		break;
	}
}

/*
@|Console ----------------------------------------------------------- */

#define CONSOLE_STD 0x1
#define CONSOLE_ARG 0x2
#define CONSOLE_EOA 0x3
#define CONSOLE_END 0x4

int
console_input(int c, int type)
{
	if(c == EOF) c = 0, type = 4;
	uxn.dev[0x12] = c, uxn.dev[0x17] = type;
	uxn_eval(console_vector);
	return type != 4;
}

void
console_arguments(int i, int argc, char **argv)
{
	for(; i < argc; i++) {
		char *p = argv[i];
		while(*p)
			console_input(*p++, CONSOLE_ARG);
		console_input('\n', i == argc - 1 ? CONSOLE_END : CONSOLE_EOA);
	}
}

void
console_deo(Uint8 addr)
{
	FILE *fd;
	switch(addr) {
	case 0x11: console_vector = PEEK2(&uxn.dev[0x10]); return;
	case 0x18: fd = stdout, fputc(uxn.dev[0x18], fd), fflush(fd); break;
	case 0x19: fd = stderr, fputc(uxn.dev[0x19], fd), fflush(fd); break;
	}
}

static int window_created, fullscreen, borderless;
static Uint32 stdin_event, audio0_event, zoom = 1;

/*
@|Screen ------------------------------------------------------------ */

/* clang-format off */

#define clamp(v,a,b) { if(v < a) v = a; else if(v >= b) v = b; }
#define twos(v) (v & 0x8000 ? (int)v - 0x10000 : (int)v)

/* clang-format on */

typedef struct UxnScreen {
	int width, height, vector, x1, y1, x2, y2, scale;
	Uint32 palette[16], *pixels;
	Uint8 *fg, *bg;
} UxnScreen;

UxnScreen uxn_screen;

#define MAR(x) (x + 0x8)
#define MAR2(x) (x + 0x10)

/* c = !ch ? (color % 5 ? color >> 2 : 0) : color % 4 + ch == 1 ? 0 : (ch - 2 + (color & 3)) % 3 + 1; */

static Uint8 blending[4][16] = {
	{0, 0, 0, 0, 1, 0, 1, 1, 2, 2, 0, 2, 3, 3, 3, 0},
	{0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3},
	{1, 2, 3, 1, 1, 2, 3, 1, 1, 2, 3, 1, 1, 2, 3, 1},
	{2, 3, 1, 2, 2, 3, 1, 2, 2, 3, 1, 2, 2, 3, 1, 2}};

int emu_resize(int width, int height);

int
screen_changed(void)
{
	clamp(uxn_screen.x1, 0, uxn_screen.width);
	clamp(uxn_screen.y1, 0, uxn_screen.height);
	clamp(uxn_screen.x2, 0, uxn_screen.width);
	clamp(uxn_screen.y2, 0, uxn_screen.height);
	return uxn_screen.x2 > uxn_screen.x1 &&
		uxn_screen.y2 > uxn_screen.y1;
}

static void
screen_change(int x1, int y1, int x2, int y2)
{
	if(x1 < uxn_screen.x1) uxn_screen.x1 = x1;
	if(y1 < uxn_screen.y1) uxn_screen.y1 = y1;
	if(x2 > uxn_screen.x2) uxn_screen.x2 = x2;
	if(y2 > uxn_screen.y2) uxn_screen.y2 = y2;
}

void
screen_palette(void)
{
	int i, shift;
	unsigned long colors[4];
	for(i = 0, shift = 4; i < 4; ++i, shift ^= 4) {
		Uint8
			r = (uxn.dev[0x8 + i / 2] >> shift) & 0xf,
			g = (uxn.dev[0xa + i / 2] >> shift) & 0xf,
			b = (uxn.dev[0xc + i / 2] >> shift) & 0xf;
		colors[i] = 0x0f000000 | r << 16 | g << 8 | b;
		colors[i] |= colors[i] << 4;
	}
	for(i = 0; i < 16; i++)
		uxn_screen.palette[i] = colors[(i >> 2) ? (i >> 2) : (i & 3)];
	screen_change(0, 0, uxn_screen.width, uxn_screen.height);
}

void
screen_resize(Uint16 width, Uint16 height, int scale)
{
	Uint32 *pixels;
	clamp(width, 8, 0x800);
	clamp(height, 8, 0x800);
	clamp(scale, 1, 3);
	/* on rescale */
	pixels = realloc(uxn_screen.pixels, width * height * sizeof(Uint32) * scale * scale);
	if(!pixels) return;
	uxn_screen.pixels = pixels;
	uxn_screen.scale = scale;
	/* on resize */
	if(uxn_screen.width != width || uxn_screen.height != height) {
		int i, length = MAR2(width) * MAR2(height);
		Uint8 *bg = realloc(uxn_screen.bg, length), *fg = realloc(uxn_screen.fg, length);
		if(!bg || !fg) return;
		uxn_screen.bg = bg, uxn_screen.fg = fg;
		uxn_screen.width = width, uxn_screen.height = height;
		for(i = 0; i < length; i++)
			uxn_screen.bg[i] = uxn_screen.fg[i] = 0;
	}
	screen_change(0, 0, width, height);
	emu_resize(width, height);
}

void
screen_redraw(void)
{
	int i, x, y, k, l;
	for(y = uxn_screen.y1; y < uxn_screen.y2; y++) {
		int ys = y * uxn_screen.scale;
		for(x = uxn_screen.x1, i = MAR(x) + MAR(y) * MAR2(uxn_screen.width); x < uxn_screen.x2; x++, i++) {
			int c = uxn_screen.palette[uxn_screen.fg[i] << 2 | uxn_screen.bg[i]];
			for(k = 0; k < uxn_screen.scale; k++) {
				int oo = ((ys + k) * uxn_screen.width + x) * uxn_screen.scale;
				for(l = 0; l < uxn_screen.scale; l++)
					uxn_screen.pixels[oo + l] = c;
			}
		}
	}
	uxn_screen.x1 = uxn_screen.y1 = 9999;
	uxn_screen.x2 = uxn_screen.y2 = 0;
}

/* screen registers */

static int rX, rY, rA, rMX, rMY, rMA, rML, rDX, rDY;

Uint8
screen_dei(Uint8 addr)
{
	switch(addr) {
	case 0x22: return uxn_screen.width >> 8;
	case 0x23: return uxn_screen.width;
	case 0x24: return uxn_screen.height >> 8;
	case 0x25: return uxn_screen.height;
	case 0x28: return rX >> 8;
	case 0x29: return rX;
	case 0x2a: return rY >> 8;
	case 0x2b: return rY;
	case 0x2c: return rA >> 8;
	case 0x2d: return rA;
	default: return uxn.dev[addr];
	}
}

void
screen_deo(Uint8 addr)
{
	switch(addr) {
	case 0x21: uxn_screen.vector = PEEK2(&uxn.dev[0x20]); return;
	case 0x23: screen_resize(PEEK2(&uxn.dev[0x22]), uxn_screen.height, uxn_screen.scale); return;
	case 0x25: screen_resize(uxn_screen.width, PEEK2(&uxn.dev[0x24]), uxn_screen.scale); return;
	case 0x26: rMX = uxn.dev[0x26] & 0x1, rMY = uxn.dev[0x26] & 0x2, rMA = uxn.dev[0x26] & 0x4, rML = uxn.dev[0x26] >> 4, rDX = rMX << 3, rDY = rMY << 2; return;
	case 0x28:
	case 0x29: rX = (uxn.dev[0x28] << 8) | uxn.dev[0x29], rX = twos(rX); return;
	case 0x2a:
	case 0x2b: rY = (uxn.dev[0x2a] << 8) | uxn.dev[0x2b], rY = twos(rY); return;
	case 0x2c:
	case 0x2d: rA = (uxn.dev[0x2c] << 8) | uxn.dev[0x2d]; return;
	case 0x2e: {
		int ctrl = uxn.dev[0x2e];
		int color = ctrl & 0x3;
		int len = MAR2(uxn_screen.width);
		Uint8 *layer = ctrl & 0x40 ? uxn_screen.fg : uxn_screen.bg;
		/* fill mode */
		if(ctrl & 0x80) {
			int x1, y1, x2, y2, ax, bx, ay, by, hor, ver;
			if(ctrl & 0x10)
				x1 = 0, x2 = rX;
			else
				x1 = rX, x2 = uxn_screen.width;
			if(ctrl & 0x20)
				y1 = 0, y2 = rY;
			else
				y1 = rY, y2 = uxn_screen.height;
			screen_change(x1, y1, x2, y2);
			x1 = MAR(x1), y1 = MAR(y1);
			hor = MAR(x2) - x1, ver = MAR(y2) - y1;
			for(ay = y1 * len, by = ay + ver * len; ay < by; ay += len)
				for(ax = ay + x1, bx = ax + hor; ax < bx; ax++)
					layer[ax] = color;
		}
		/* pixel mode */
		else {
			if(rX >= 0 && rY >= 0 && rX < len && rY < uxn_screen.height)
				layer[MAR(rX) + MAR(rY) * len] = color;
			screen_change(rX, rY, rX + 1, rY + 1);
			if(rMX) rX++;
			if(rMY) rY++;
		}
		return;
	}
	case 0x2f: {
		int ctrl = uxn.dev[0x2f];
		int blend = ctrl & 0xf, opaque = blend % 5;
		int fx = ctrl & 0x10 ? -1 : 1, fy = ctrl & 0x20 ? -1 : 1;
		int qfx = fx > 0 ? 7 : 0, qfy = fy < 0 ? 7 : 0;
		int dxy = fy * rDX, dyx = fx * rDY;
		int wmar = MAR(uxn_screen.width), wmar2 = MAR2(uxn_screen.width);
		int hmar2 = MAR2(uxn_screen.height);
		int i, x1, x2, y1, y2, ax, ay, qx, qy, x = rX, y = rY;
		Uint8 *layer = ctrl & 0x40 ? uxn_screen.fg : uxn_screen.bg;
		if(ctrl & 0x80) {
			int addr_incr = rMA << 2;
			for(i = 0; i <= rML; i++, x += dyx, y += dxy, rA += addr_incr) {
				Uint16 xmar = MAR(x), ymar = MAR(y);
				Uint16 xmar2 = MAR2(x), ymar2 = MAR2(y);
				if(xmar < wmar && ymar2 < hmar2) {
					Uint8 *sprite = &uxn.ram[rA];
					int by = ymar2 * wmar2;
					for(ay = ymar * wmar2, qy = qfy; ay < by; ay += wmar2, qy += fy) {
						int ch1 = sprite[qy], ch2 = sprite[qy + 8] << 1, bx = xmar2 + ay;
						for(ax = xmar + ay, qx = qfx; ax < bx; ax++, qx -= fx) {
							int color = ((ch1 >> qx) & 1) | ((ch2 >> qx) & 2);
							if(opaque || color) layer[ax] = blending[color][blend];
						}
					}
				}
			}
		} else {
			int addr_incr = rMA << 1;
			for(i = 0; i <= rML; i++, x += dyx, y += dxy, rA += addr_incr) {
				Uint16 xmar = MAR(x), ymar = MAR(y);
				Uint16 xmar2 = MAR2(x), ymar2 = MAR2(y);
				if(xmar < wmar && ymar2 < hmar2) {
					Uint8 *sprite = &uxn.ram[rA];
					int by = ymar2 * wmar2;
					for(ay = ymar * wmar2, qy = qfy; ay < by; ay += wmar2, qy += fy) {
						int ch1 = sprite[qy], bx = xmar2 + ay;
						for(ax = xmar + ay, qx = qfx; ax < bx; ax++, qx -= fx) {
							int color = (ch1 >> qx) & 1;
							if(opaque || color) layer[ax] = blending[color][blend];
						}
					}
				}
			}
		}
		if(fx < 0)
			x1 = x, x2 = rX;
		else
			x1 = rX, x2 = x;
		if(fy < 0)
			y1 = y, y2 = rY;
		else
			y1 = rY, y2 = y;
		screen_change(x1 - 8, y1 - 8, x2 + 8, y2 + 8);
		if(rMX) rX += rDX * fx;
		if(rMY) rY += rDY * fy;
		return;
	}
	}
}

/*
@|Audio ------------------------------------------------------------- */

typedef signed int Sint32;

#define SAMPLE_FREQUENCY 44100
#define POLYPHONY 4

Uint8 audio_get_vu(int instance);
Uint16 audio_get_position(int instance);


#define NOTE_PERIOD (SAMPLE_FREQUENCY * 0x4000 / 11025)
#define ADSR_STEP (SAMPLE_FREQUENCY / 0xf)

typedef struct {
	Uint8 *addr;
	Uint32 count, advance, period, age, a, d, s, r;
	Uint16 i, len;
	Sint8 volume[2];
	Uint8 pitch, repeat;
} UxnAudio;

/* clang-format off */

static Uint32 advances[12] = {
	0x80000, 0x879c8, 0x8facd, 0x9837f, 0xa1451, 0xaadc1,
	0xb504f, 0xbfc88, 0xcb2ff, 0xd7450, 0xe411f, 0xf1a1c
};

static UxnAudio uxn_audio[POLYPHONY];

/* clang-format on */

int audio_render(int instance, Sint16 *sample, Sint16 *end);

static void
audio_callback(void *u, Uint8 *stream, int len)
{
	int instance, running = 0;
	Sint16 *samples = (Sint16 *)stream;
	USED(u);
	SDL_memset(stream, 0, len);
	for(instance = 0; instance < POLYPHONY; instance++)
		running += audio_render(instance, samples, samples + len / 2);
	if(!running)
		SDL_PauseAudioDevice(audio_id, 1);
}

void
audio_finished_handler(int instance)
{
	SDL_Event event;
	event.type = audio0_event + instance;
	SDL_PushEvent(&event);
}

static Sint32
envelope(UxnAudio *c, Uint32 age)
{
	if(!c->r) return 0x0888;
	if(age < c->a) return 0x0888 * age / c->a;
	if(age < c->d) return 0x0444 * (2 * c->d - c->a - age) / (c->d - c->a);
	if(age < c->s) return 0x0444;
	if(age < c->r) return 0x0444 * (c->r - age) / (c->r - c->s);
	c->advance = 0;
	return 0x0000;
}

int
audio_render(int instance, Sint16 *sample, Sint16 *end)
{
	UxnAudio *c = &uxn_audio[instance];
	Sint32 s;
	if(!c->advance || !c->period) return 0;
	while(sample < end) {
		c->count += c->advance;
		c->i += c->count / c->period;
		c->count %= c->period;
		if(c->i >= c->len) {
			if(!c->repeat) {
				c->advance = 0;
				break;
			}
			c->i %= c->len;
		}
		s = (Sint8)(c->addr[c->i] + 0x80) * envelope(c, c->age++);
		*sample++ += s * c->volume[0] / 0x180;
		*sample++ += s * c->volume[1] / 0x180;
	}
	if(!c->advance) audio_finished_handler(instance);
	return 1;
}

void
audio_start(int instance, Uint8 *d)
{
	UxnAudio *c = &uxn_audio[instance];
	Uint8 pitch = d[0xf] & 0x7f;
	Uint16 addr = PEEK2(d + 0xc);
	Uint16 adsr = PEEK2(d + 0x8);
	c->len = PEEK2(d + 0xa);
	if(c->len > 0x10000 - addr)
		c->len = 0x10000 - addr;
	c->addr = &uxn.ram[addr];
	c->volume[0] = d[0xe] >> 4;
	c->volume[1] = d[0xe] & 0xf;
	c->repeat = !(d[0xf] & 0x80);
	if(pitch < 108 && c->len)
		c->advance = advances[pitch % 12] >> (8 - pitch / 12);
	else {
		c->advance = 0;
		return;
	}
	c->a = ADSR_STEP * (adsr >> 12);
	c->d = ADSR_STEP * (adsr >> 8 & 0xf) + c->a;
	c->s = ADSR_STEP * (adsr >> 4 & 0xf) + c->d;
	c->r = ADSR_STEP * (adsr >> 0 & 0xf) + c->s;
	c->age = 0;
	c->i = 0;
	if(c->len <= 0x100) /* single cycle mode */
		c->period = NOTE_PERIOD * 337 / 2 / c->len;
	else /* sample repeat mode */
		c->period = NOTE_PERIOD;
}

Uint8
audio_get_vu(int instance)
{
	int i;
	UxnAudio *c = &uxn_audio[instance];
	Sint32 sum[2] = {0, 0};
	if(!c->advance || !c->period) return 0;
	for(i = 0; i < 2; i++) {
		if(!c->volume[i]) continue;
		sum[i] = 1 + envelope(c, c->age) * c->volume[i] / 0x800;
		if(sum[i] > 0xf) sum[i] = 0xf;
	}
	return (sum[0] << 4) | sum[1];
}

Uint16
audio_get_position(int instance)
{
	return uxn_audio[instance].i;
}

static Uint8
audio_dei(int instance, Uint8 *d, Uint8 port)
{
	if(!audio_id) return d[port];
	switch(port) {
	case 0x4: return audio_get_vu(instance);
	case 0x2: POKE2(d + 0x2, audio_get_position(instance)); /* fall through */
	default: return d[port];
	}
}

static void
audio_deo(int instance, Uint8 *d, Uint8 port)
{
	if(!audio_id) return;
	if(port == 0xf) {
		SDL_LockAudioDevice(audio_id);
		audio_start(instance, d);
		SDL_UnlockAudioDevice(audio_id);
		SDL_PauseAudioDevice(audio_id, 0);
	}
}

/*
@|Controller -------------------------------------------------------- */

static unsigned int controller_vector;

void
controller_down(Uint8 mask)
{
	if(mask) {
		uxn.dev[0x82] |= mask;
		uxn_eval(controller_vector);
	}
}

void
controller_up(Uint8 mask)
{
	if(mask) {
		uxn.dev[0x82] &= (~mask);
		uxn_eval(controller_vector);
	}
}

void
controller_key(Uint8 key)
{
	if(key) {
		uxn.dev[0x83] = key;
		uxn_eval(controller_vector);
		uxn.dev[0x83] = 0;
	}
}

void
controller_deo(Uint8 addr)
{
	switch(addr) {
	case 0x81: controller_vector = PEEK2(&uxn.dev[0x80]); break;
	}
}

/*
@|Mouse ------------------------------------------------------------- */

static unsigned int mouse_vector;

void
mouse_down(Uint8 mask)
{
	uxn.dev[0x96] |= mask;
	uxn_eval(mouse_vector);
}

void
mouse_up(Uint8 mask)
{
	uxn.dev[0x96] &= (~mask);
	uxn_eval(mouse_vector);
}

void
mouse_pos(Uint16 x, Uint16 y)
{
	uxn.dev[0x92] = x >> 8, uxn.dev[0x93] = x;
	uxn.dev[0x94] = y >> 8, uxn.dev[0x95] = y;
	uxn_eval(mouse_vector);
}

void
mouse_scroll(Uint16 x, Uint16 y)
{
	uxn.dev[0x9a] = x >> 8, uxn.dev[0x9b] = x;
	uxn.dev[0x9c] = -y >> 8, uxn.dev[0x9d] = -y;
	uxn_eval(mouse_vector);
	uxn.dev[0x9a] = 0, uxn.dev[0x9b] = 0;
	uxn.dev[0x9c] = 0, uxn.dev[0x9d] = 0;
}

void
mouse_deo(Uint8 addr)
{
	switch(addr) {
	case 0x91: mouse_vector = PEEK2(&uxn.dev[0x90]); break;
	}
}

/*
@|File -------------------------------------------------------------- */

#define POLYFILEY 2
#define DEV_FILE0 0xa

#include <stdio.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef _WIN32
#include <direct.h>
#include <libiberty/libiberty.h>
#define realpath(s, dummy) lrealpath(s)
#define DIR_SEP_CHAR '\\'
#define DIR_SEP_STR "\\"
#define pathcmp(path1, path2, length) strncasecmp(path1, path2, length) /* strncasecmp provided by libiberty */
#define notdriveroot(file_name) (file_name[0] != DIR_SEP_CHAR && ((strlen(file_name) > 2 && file_name[1] != ':') || strlen(file_name) <= 2))
#define mkdir(file_name) (_mkdir(file_name) == 0)
#else
#define DIR_SEP_CHAR '/'
#define DIR_SEP_STR "/"
#define pathcmp(path1, path2, length) strncmp(path1, path2, length)
#define notdriveroot(file_name) (file_name[0] != DIR_SEP_CHAR)
#define mkdir(file_name) (mkdir(file_name, 0755) == 0)
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/*
Copyright (c) 2021-2023 Devine Lu Linvega, Andrew Alderwick

Permission to use, copy, modify, and distribute this software for any
purpose with or without fee is hereby granted, provided that the above
copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
WITH REGARD TO THIS SOFTWARE.
*/

typedef struct {
	FILE *f;
	DIR *dir;
	char current_filename[4096];
	struct dirent *de;
	enum { IDLE,
		FILE_READ,
		FILE_WRITE,
		DIR_READ,
		DIR_WRITE
	} state;
	int outside_sandbox;
} UxnFile;

static UxnFile uxn_file[POLYFILEY];

static void
reset(UxnFile *c)
{
	if(c->f != NULL) {
		fclose(c->f);
		c->f = NULL;
	}
	if(c->dir != NULL) {
		closedir(c->dir);
		c->dir = NULL;
	}
	c->de = NULL;
	c->state = IDLE;
	c->outside_sandbox = 0;
}

static Uint16
get_entry(char *p, Uint16 len, const char *pathname, const char *basename, int fail_nonzero)
{
	struct stat st;
	if(len < strlen(basename) + 8)
		return 0;
	if(stat(pathname, &st))
		return fail_nonzero ? snprintf(p, len, "!!!! %s\n", basename) : 0;
	else if(S_ISDIR(st.st_mode))
		return snprintf(p, len, "---- %s/\n", basename);
	else if(st.st_size < 0x10000)
		return snprintf(p, len, "%04x %s\n", (unsigned int)st.st_size, basename);
	else
		return snprintf(p, len, "???? %s\n", basename);
}

static Uint16
file_read_dir(UxnFile *c, char *dest, Uint16 len)
{
	static char pathname[4352];
	char *p = dest;
	if(c->de == NULL) c->de = readdir(c->dir);
	for(; c->de != NULL; c->de = readdir(c->dir)) {
		Uint16 n;
		if(c->de->d_name[0] == '.' && c->de->d_name[1] == '\0')
			continue;
		if(strcmp(c->de->d_name, "..") == 0) {
			/* hide "sandbox/.." */
			char cwd[PATH_MAX] = {'\0'}, *t;
			/* Note there's [currently] no way of chdir()ing from uxn, so $PWD
			 * is always the sandbox top level. */
			getcwd(cwd, sizeof(cwd));
			/* We already checked that c->current_filename exists so don't need a wrapper. */
			t = realpath(c->current_filename, NULL);
			if(strcmp(cwd, t) == 0) {
				free(t);
				continue;
			}
			free(t);
		}
		if(strlen(c->current_filename) + 1 + strlen(c->de->d_name) < sizeof(pathname))
			snprintf(pathname, sizeof(pathname), "%s/%s", c->current_filename, c->de->d_name);
		else
			pathname[0] = '\0';
		n = get_entry(p, len, pathname, c->de->d_name, 1);
		if(!n) break;
		p += n;
		len -= n;
	}
	return p - dest;
}

static char *
retry_realpath(const char *file_name)
{
	char *r, p[PATH_MAX] = {'\0'}, *x;
	int fnlen;
	if(file_name == NULL) {
		errno = EINVAL;
		return NULL;
	} else if((fnlen = strlen(file_name)) >= PATH_MAX) {
		errno = ENAMETOOLONG;
		return NULL;
	}
	if(notdriveroot(file_name)) {
		getcwd(p, sizeof(p));
		if(strlen(p) + strlen(DIR_SEP_STR) + fnlen >= PATH_MAX) {
			errno = ENAMETOOLONG;
			return NULL;
		}
		strcat(p, DIR_SEP_STR);
	}
	strcat(p, file_name);
	while((r = realpath(p, NULL)) == NULL) {
		if(errno != ENOENT)
			return NULL;
		x = strrchr(p, DIR_SEP_CHAR);
		if(x)
			*x = '\0';
		else
			return NULL;
	}
	return r;
}

static void
file_check_sandbox(UxnFile *c)
{
	char *x, *rp, cwd[PATH_MAX] = {'\0'};
	x = getcwd(cwd, sizeof(cwd));
	rp = retry_realpath(c->current_filename);
	if(rp == NULL || (x && pathcmp(cwd, rp, strlen(cwd)) != 0)) {
		c->outside_sandbox = 1;
		fprintf(stderr, "file warning: blocked attempt to access %s outside of sandbox\n", c->current_filename);
	}
	free(rp);
}

static Uint16
file_init(UxnFile *c, char *filename, size_t max_len, int override_sandbox)
{
	char *p = c->current_filename;
	size_t len = sizeof(c->current_filename);
	reset(c);
	if(len > max_len) len = max_len;
	while(len) {
		if((*p++ = *filename++) == '\0') {
			if(!override_sandbox) /* override sandbox for loading roms */
				file_check_sandbox(c);
			return 0;
		}
		len--;
	}
	c->current_filename[0] = '\0';
	return 0;
}

static Uint16
file_read(UxnFile *c, void *dest, int len)
{
	if(c->outside_sandbox) return 0;
	if(c->state != FILE_READ && c->state != DIR_READ) {
		reset(c);
		if((c->dir = opendir(c->current_filename)) != NULL)
			c->state = DIR_READ;
		else if((c->f = fopen(c->current_filename, "rb")) != NULL)
			c->state = FILE_READ;
	}
	if(c->state == FILE_READ)
		return fread(dest, 1, len, c->f);
	if(c->state == DIR_READ)
		return file_read_dir(c, dest, len);
	return 0;
}

static int
is_dir_path(char *p)
{
	char c;
	int saw_slash = 0;
	while((c = *p++))
		saw_slash = c == DIR_SEP_CHAR;
	return saw_slash;
}

int
dir_exists(char *p)
{
	struct stat st;
	return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

int
ensure_parent_dirs(char *p)
{
	int ok = 1;
	char c, *s = p;
	for(; ok && (c = *p); p++) {
		if(c == DIR_SEP_CHAR) {
			*p = '\0';
			ok = dir_exists(s) || mkdir(s);
			*p = c;
		}
	}
	return ok;
}

static Uint16
file_write(UxnFile *c, void *src, Uint16 len, Uint8 flags)
{
	Uint16 ret = 0;
	if(c->outside_sandbox) return 0;
	ensure_parent_dirs(c->current_filename);
	if(c->state != FILE_WRITE && c->state != DIR_WRITE) {
		reset(c);
		if(is_dir_path(c->current_filename))
			c->state = DIR_WRITE;
		else if((c->f = fopen(c->current_filename, (flags & 0x01) ? "ab" : "wb")) != NULL)
			c->state = FILE_WRITE;
	}
	if(c->state == FILE_WRITE) {
		if((ret = fwrite(src, 1, len, c->f)) > 0 && fflush(c->f) != 0)
			ret = 0;
	}
	if(c->state == DIR_WRITE) {
		ret = dir_exists(c->current_filename);
	}
	return ret;
}

static Uint16
stat_fill(Uint8 *dest, Uint16 len, char c)
{
	Uint16 i;
	for(i = 0; i < len; i++)
		*(dest++) = c;
	return len;
}

static Uint16
stat_size(Uint8 *dest, Uint16 len, off_t size)
{
	Uint16 i;
	dest += len - 1;
	for(i = 0; i < len; i++) {
		char c = '0' + (Uint8)(size & 0xf);
		if(c > '9') c += 39;
		*(dest--) = c;
		size = size >> 4;
	}
	return size == 0 ? len : stat_fill(dest, len, '?');
}

static Uint16
file_stat(UxnFile *c, void *dest, Uint16 len)
{
	struct stat st;
	if(c->outside_sandbox)
		return 0;
	else if(stat(c->current_filename, &st))
		return stat_fill(dest, len, '!');
	else if(S_ISDIR(st.st_mode))
		return stat_fill(dest, len, '-');
	else
		return stat_size(dest, len, st.st_size);
}

static Uint16
file_delete(UxnFile *c)
{
	return c->outside_sandbox ? 0 : unlink(c->current_filename);
}

/* IO */

void
file_deo(Uint8 port)
{
	Uint16 addr, len, res;
	switch(port) {
	case 0xa5:
		addr = PEEK2(&uxn.dev[0xa4]);
		len = PEEK2(&uxn.dev[0xaa]);
		if(len > 0x10000 - addr)
			len = 0x10000 - addr;
		res = file_stat(&uxn_file[0], &uxn.ram[addr], len);
		POKE2(&uxn.dev[0xa2], res);
		break;
	case 0xa6:
		res = file_delete(&uxn_file[0]);
		POKE2(&uxn.dev[0xa2], res);
		break;
	case 0xa9:
		addr = PEEK2(&uxn.dev[0xa8]);
		res = file_init(&uxn_file[0], (char *)&uxn.ram[addr], 0x10000 - addr, 0);
		POKE2(&uxn.dev[0xa2], res);
		break;
	case 0xad:
		addr = PEEK2(&uxn.dev[0xac]);
		len = PEEK2(&uxn.dev[0xaa]);
		if(len > 0x10000 - addr)
			len = 0x10000 - addr;
		res = file_read(&uxn_file[0], &uxn.ram[addr], len);
		POKE2(&uxn.dev[0xa2], res);
		break;
	case 0xaf:
		addr = PEEK2(&uxn.dev[0xae]);
		len = PEEK2(&uxn.dev[0xaa]);
		if(len > 0x10000 - addr)
			len = 0x10000 - addr;
		res = file_write(&uxn_file[0], &uxn.ram[addr], len, uxn.dev[0xa7]);
		POKE2(&uxn.dev[0xa2], res);
		break;
	/* File 2 */
	case 0xb5:
		addr = PEEK2(&uxn.dev[0xb4]);
		len = PEEK2(&uxn.dev[0xba]);
		if(len > 0x10000 - addr)
			len = 0x10000 - addr;
		res = file_stat(&uxn_file[1], &uxn.ram[addr], len);
		POKE2(&uxn.dev[0xb2], res);
		break;
	case 0xb6:
		res = file_delete(&uxn_file[1]);
		POKE2(&uxn.dev[0xb2], res);
		break;
	case 0xb9:
		addr = PEEK2(&uxn.dev[0xb8]);
		res = file_init(&uxn_file[1], (char *)&uxn.ram[addr], 0x10000 - addr, 0);
		POKE2(&uxn.dev[0xb2], res);
		break;
	case 0xbd:
		addr = PEEK2(&uxn.dev[0xbc]);
		len = PEEK2(&uxn.dev[0xba]);
		if(len > 0x10000 - addr)
			len = 0x10000 - addr;
		res = file_read(&uxn_file[1], &uxn.ram[addr], len);
		POKE2(&uxn.dev[0xb2], res);
		break;
	case 0xbf:
		addr = PEEK2(&uxn.dev[0xbe]);
		len = PEEK2(&uxn.dev[0xba]);
		if(len > 0x10000 - addr)
			len = 0x10000 - addr;
		res = file_write(&uxn_file[1], &uxn.ram[addr], len, uxn.dev[0xb7]);
		POKE2(&uxn.dev[0xb2], res);
		break;
	}
}

/*
@|Datetime ---------------------------------------------------------- */

#include <time.h>

Uint8
datetime_dei(Uint8 addr)
{
	time_t seconds = time(NULL);
	struct tm zt = {0};
	struct tm *t = localtime(&seconds);
	if(t == NULL)
		t = &zt;
	switch(addr) {
	case 0xc0: return (t->tm_year + 1900) >> 8;
	case 0xc1: return (t->tm_year + 1900);
	case 0xc2: return t->tm_mon;
	case 0xc3: return t->tm_mday;
	case 0xc4: return t->tm_hour;
	case 0xc5: return t->tm_min;
	case 0xc6: return t->tm_sec;
	case 0xc7: return t->tm_wday;
	case 0xc8: return t->tm_yday >> 8;
	case 0xc9: return t->tm_yday;
	case 0xca: return t->tm_isdst;
	default: return uxn.dev[addr];
	}
}

/*
@|Core -------------------------------------------------------------- */


Uint8
emu_dei(Uint8 addr)
{
	Uint8 p = addr & 0x0f, d = addr & 0xf0;
	switch(d) {
	case 0x00: return system_dei(addr);
	case 0x20: return screen_dei(addr);
	case 0x30: return audio_dei(0, &uxn.dev[d], p);
	case 0x40: return audio_dei(1, &uxn.dev[d], p);
	case 0x50: return audio_dei(2, &uxn.dev[d], p);
	case 0x60: return audio_dei(3, &uxn.dev[d], p);
	case 0xc0: return datetime_dei(addr);
	}
	return uxn.dev[addr];
}

void
emu_deo(Uint8 addr, Uint8 value)
{
	Uint8 p = addr & 0x0f, d = addr & 0xf0;
	uxn.dev[addr] = value;
	switch(d) {
	case 0x00:
		system_deo(addr);
		if(p > 0x7 && p < 0xe) screen_palette();
		break;
	case 0x10: console_deo(addr); break;
	case 0x20: screen_deo(addr); break;
	case 0x30: audio_deo(0, &uxn.dev[d], p); break;
	case 0x40: audio_deo(1, &uxn.dev[d], p); break;
	case 0x50: audio_deo(2, &uxn.dev[d], p); break;
	case 0x60: audio_deo(3, &uxn.dev[d], p); break;
	case 0x80: controller_deo(addr); break;
	case 0x90: mouse_deo(addr); break;
	case 0xa0: file_deo(addr); break;
	case 0xb0: file_deo(addr); break;
	}
}

/* Handlers */

static int
stdin_handler(void *p)
{
	SDL_Event event;
	USED(p);
	event.type = stdin_event;
	event.cbutton.state = CONSOLE_STD;
	while(read(0, &event.cbutton.button, 1) > 0) {
		while(SDL_PushEvent(&event) < 0)
			SDL_Delay(25); /* slow down - the queue is most likely full */
	}
	/* EOF */
	event.cbutton.button = 0x00;
	event.cbutton.state = CONSOLE_END;
	while(SDL_PushEvent(&event) < 0)
		SDL_Delay(25);
	return 0;
}

static void
set_window_size(SDL_Window *window, int w, int h)
{
	SDL_Point win_old;
	SDL_GetWindowSize(window, &win_old.x, &win_old.y);
	if(w == win_old.x && h == win_old.y) return;
	SDL_RenderClear(emu_renderer);
	SDL_SetWindowSize(window, w, h);
	screen_resize(uxn_screen.width, uxn_screen.height, 1);
}

static void
set_zoom(Uint8 z, int win)
{
	if(z < 1) return;
	if(win)
		set_window_size(emu_window, uxn_screen.width * z, uxn_screen.height * z);
	zoom = z;
}

static void
set_fullscreen(int value, int win)
{
	Uint32 flags = 0; /* windowed mode; SDL2 has no constant for this */
	fullscreen = value;
	if(fullscreen)
		flags = SDL_WINDOW_FULLSCREEN_DESKTOP;
	if(win)
		SDL_SetWindowFullscreen(emu_window, flags);
}

static void
set_borderless(int value)
{
	if(fullscreen) return;
	borderless = value;
	SDL_SetWindowBordered(emu_window, !value);
}

/* emulator primitives */

int
emu_resize(int width, int height)
{
	if(!window_created)
		return 0;
	if(emu_texture != NULL)
		SDL_DestroyTexture(emu_texture);
	SDL_RenderSetLogicalSize(emu_renderer, width, height);
	emu_texture = SDL_CreateTexture(emu_renderer, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STATIC, width, height);
	if(emu_texture == NULL || SDL_SetTextureBlendMode(emu_texture, SDL_BLENDMODE_NONE))
		return system_error("SDL_SetTextureBlendMode", SDL_GetError());
	if(SDL_UpdateTexture(emu_texture, NULL, uxn_screen.pixels, sizeof(Uint32)) != 0)
		return system_error("SDL_UpdateTexture", SDL_GetError());
	emu_viewport.x = 0;
	emu_viewport.y = 0;
	emu_viewport.w = uxn_screen.width;
	emu_viewport.h = uxn_screen.height;
	set_window_size(emu_window, width * zoom, height * zoom);
	return 1;
}

static void
emu_redraw(void)
{
	if(SDL_UpdateTexture(emu_texture, NULL, uxn_screen.pixels, uxn_screen.width * sizeof(Uint32)) != 0)
		system_error("SDL_UpdateTexture", SDL_GetError());
	SDL_RenderClear(emu_renderer);
	SDL_RenderCopy(emu_renderer, emu_texture, NULL, &emu_viewport);
	SDL_RenderPresent(emu_renderer);
}

static void
emu_init_audio(void)
{
	SDL_AudioSpec as;
	SDL_zero(as);
	as.freq = SAMPLE_FREQUENCY;
	as.format = AUDIO_S16SYS;
	as.channels = 2;
	as.callback = audio_callback;
	as.samples = 512;
	as.userdata = NULL;
	audio_id = SDL_OpenAudioDevice(NULL, 0, &as, NULL, 0);
	if(!audio_id)
		system_error("sdl_audio", SDL_GetError());
	audio0_event = SDL_RegisterEvents(POLYPHONY);
	SDL_PauseAudioDevice(audio_id, 1);
}

static int
emu_init(void)
{
	if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK) < 0)
		return system_error("sdl", SDL_GetError());
	emu_init_audio();
	if(SDL_NumJoysticks() > 0 && SDL_JoystickOpen(0) == NULL)
		system_error("sdl_joystick", SDL_GetError());
	stdin_event = SDL_RegisterEvents(1);
	SDL_DetachThread(stdin_thread = SDL_CreateThread(stdin_handler, "stdin", NULL));
	SDL_StartTextInput();
	SDL_ShowCursor(SDL_DISABLE);
	SDL_EventState(SDL_DROPFILE, SDL_ENABLE);
	SDL_SetRenderDrawColor(emu_renderer, 0x00, 0x00, 0x00, 0xff);
	screen_resize(WIDTH, HEIGHT, 1);
	return 1;
}

static void
emu_restart(int soft)
{
	screen_resize(WIDTH, HEIGHT, uxn_screen.scale);
	system_reboot(soft);
	SDL_SetWindowTitle(emu_window, "Varvara");
}

static Uint8
get_button(SDL_Event *event)
{
	switch(event->key.keysym.sym) {
	case SDLK_LCTRL: return 0x01;
	case SDLK_LALT: return 0x02;
	case SDLK_LSHIFT: return 0x04;
	case SDLK_HOME: return 0x08;
	case SDLK_UP: return 0x10;
	case SDLK_DOWN: return 0x20;
	case SDLK_LEFT: return 0x40;
	case SDLK_RIGHT: return 0x80;
	}
	return 0x00;
}

static Uint8
get_button_joystick(SDL_Event *event)
{
	return 0x01 << (event->jbutton.button & 0x3);
}

static Uint8
get_vector_joystick(SDL_Event *event)
{
	if(event->jaxis.value < -3200)
		return 1;
	if(event->jaxis.value > 3200)
		return 2;
	return 0;
}

static Uint8
get_key(SDL_Event *event)
{
	int sym = event->key.keysym.sym;
	SDL_Keymod mods = SDL_GetModState();
	if(sym < 0x20 || sym == SDLK_DELETE)
		return sym;
	if(mods & KMOD_CTRL) {
		if(sym < SDLK_a)
			return sym;
		else if(sym <= SDLK_z)
			return sym - (mods & KMOD_SHIFT) * 0x20;
	}
	return 0x00;
}

static int
handle_events(void)
{
	SDL_Event event;
	while(SDL_PollEvent(&event)) {
		/* Window */
		if(event.type == SDL_QUIT)
			return 0;
		else if(event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_EXPOSED)
			emu_redraw();
		/* Mouse */
		else if(event.type == SDL_MOUSEMOTION)
			mouse_pos(event.motion.x, event.motion.y);
		else if(event.type == SDL_MOUSEBUTTONUP)
			mouse_up(SDL_BUTTON(event.button.button));
		else if(event.type == SDL_MOUSEBUTTONDOWN)
			mouse_down(SDL_BUTTON(event.button.button));
		else if(event.type == SDL_MOUSEWHEEL)
			mouse_scroll(event.wheel.x, event.wheel.y);
		/* Audio */
		else if(event.type >= audio0_event && event.type < audio0_event + POLYPHONY) {
			Uint8 *port_value = &uxn.dev[0x30 + 0x10 * (event.type - audio0_event)];
			uxn_eval(port_value[0] << 8 | port_value[1]);
		}
		/* Controller */
		else if(event.type == SDL_TEXTINPUT) {
			char *c;
			for(c = event.text.text; *c; c++)
				controller_key(*c);
		} else if(event.type == SDL_KEYDOWN) {
			int ksym;
			if(get_key(&event))
				controller_key(get_key(&event));
			else if(get_button(&event))
				controller_down(get_button(&event));
			else if(event.key.keysym.sym == SDLK_F1)
				set_zoom(zoom == 3 ? 1 : zoom + 1, 1);
			else if(event.key.keysym.sym == SDLK_F2)
				emu_deo(0xe, 0x1);
			else if(event.key.keysym.sym == SDLK_F3)
				uxn.dev[0x0f] = 0xff;
			else if(event.key.keysym.sym == SDLK_F4)
				emu_restart(0);
			else if(event.key.keysym.sym == SDLK_F5)
				emu_restart(1);
			else if(event.key.keysym.sym == SDLK_F11)
				set_fullscreen(!fullscreen, 1);
			else if(event.key.keysym.sym == SDLK_F12)
				set_borderless(!borderless);
			ksym = event.key.keysym.sym;
			if(SDL_PeepEvents(&event, 1, SDL_PEEKEVENT, SDL_KEYUP, SDL_KEYUP) == 1 && ksym == event.key.keysym.sym)
				return 1;
		} else if(event.type == SDL_KEYUP)
			controller_up(get_button(&event));
		else if(event.type == SDL_JOYAXISMOTION) {
			Uint8 vec = get_vector_joystick(&event);
			if(!vec)
				controller_up((3 << (!event.jaxis.axis * 2)) << 4);
			else
				controller_down((1 << ((vec + !event.jaxis.axis * 2) - 1)) << 4);
		} else if(event.type == SDL_JOYBUTTONDOWN)
			controller_down(get_button_joystick(&event));
		else if(event.type == SDL_JOYBUTTONUP)
			controller_up(get_button_joystick(&event));
		else if(event.type == SDL_JOYHATMOTION) {
			/* NOTE: Assuming there is only one joyhat in the controller */
			switch(event.jhat.value) {
			case SDL_HAT_UP: controller_down(0x10); break;
			case SDL_HAT_DOWN: controller_down(0x20); break;
			case SDL_HAT_LEFT: controller_down(0x40); break;
			case SDL_HAT_RIGHT: controller_down(0x80); break;
			case SDL_HAT_LEFTDOWN: controller_down(0x40 | 0x20); break;
			case SDL_HAT_LEFTUP: controller_down(0x40 | 0x10); break;
			case SDL_HAT_RIGHTDOWN: controller_down(0x80 | 0x20); break;
			case SDL_HAT_RIGHTUP: controller_down(0x80 | 0x10); break;
			case SDL_HAT_CENTERED: controller_up(0x10 | 0x20 | 0x40 | 0x80); break;
			}
		}
		/* Console */
		else if(event.type == stdin_event)
			console_input(event.cbutton.button, event.cbutton.state);
	}
	return 1;
}

static int
emu_run(void)
{
	Uint64 next_refresh = 0;
	Uint64 perf_freq = SDL_GetPerformanceFrequency();
	Uint64 frame_interval = perf_freq / 60;
	Uint64 ms_interval = perf_freq / 1000;
	Uint32 window_flags = SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI;
	char *rom_name = metadata_read_name();
	window_created = 0;
	if(fullscreen)
		window_flags = window_flags | SDL_WINDOW_FULLSCREEN_DESKTOP;
	emu_window = SDL_CreateWindow(rom_name,
		SDL_WINDOWPOS_UNDEFINED,
		SDL_WINDOWPOS_UNDEFINED,
		uxn_screen.width * zoom,
		uxn_screen.height * zoom,
		window_flags);
	if(emu_window == NULL)
		return system_error("sdl_window", SDL_GetError());
	window_created = 1;
	emu_renderer = SDL_CreateRenderer(emu_window, -1, SDL_RENDERER_ACCELERATED);
	if(emu_renderer == NULL)
		return system_error("sdl_renderer", SDL_GetError());
	emu_resize(uxn_screen.width, uxn_screen.height);
	/* game loop */
	for(;;) {
		Uint64 now = SDL_GetPerformanceCounter();
		/* .System/halt */
		if(uxn.dev[0x0f])
			return system_error("Run", "Ended.");
		if(!handle_events())
			return 0;
		if(now >= next_refresh) {
			next_refresh = now + frame_interval;
			uxn_eval(uxn_screen.vector);
			if(uxn_screen.x2 && uxn_screen.y2 && screen_changed())
				screen_redraw(), emu_redraw();
		}
		if(uxn_screen.vector) {
			now = SDL_GetPerformanceCounter();
			if(now < next_refresh) {
				Uint64 delay_ms = (next_refresh - now) / ms_interval;
				if (delay_ms > 0) SDL_Delay(delay_ms);
			}
		} else
			SDL_WaitEvent(NULL);
	}
}

int
main(int argc, char **argv)
{
	int i = 1;
	char *rom_path;
	/* flags */
	if(argc > 1 && argv[i][0] == '-') {
		if(!strcmp(argv[i], "-v"))
			return system_error("Uxn(gui) - Varvara Emulator", "12 Jul 2025.");
		else if(!strcmp(argv[i], "-2x"))
			set_zoom(2, 0);
		else if(!strcmp(argv[i], "-3x"))
			set_zoom(3, 0);
		else if(strcmp(argv[i], "-f") == 0)
			set_fullscreen(1, 0);
		i++;
	}
	/* init */
	rom_path = i == argc ? "boot.rom" : argv[i++];
	if(!emu_init())
		return system_error("Init", "Failed to initialize varvara.");
	if(!system_boot((Uint8 *)calloc(PAGE_SIZE * BANKS + 1, sizeof(Uint8)), rom_path, argc > i))
		return system_error("usage:", "uxnemu [-v | -f | -2x | -3x] file.rom [args...]");
	/* start */
	console_arguments(i, argc, argv);
	emu_run();
	/* end */
	SDL_CloseAudioDevice(audio_id);
#ifdef _WIN32
#pragma GCC diagnostic ignored "-Wint-to-pointer-cast"
	TerminateThread((HANDLE)SDL_GetThreadID(stdin_thread), 0);
#elif !defined(__APPLE__)
	close(0); /* make stdin thread exit */
#endif
	SDL_Quit();
	return uxn.dev[0x0f] & 0x7f;
}
