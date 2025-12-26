/*
Copyright (c) 2021 Devine Lu Linvega, Andrew Alderwick

Permission to use, copy, modify, and distribute this software for any
purpose with or without fee is hereby granted, provided that the above
copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
WITH REGARD TO THIS SOFTWARE.
*/



int console_input(int c, int type);
void console_arguments(int i, int argc, char **argv);
Uint8 console_dei(Uint8 addr);
void console_deo(Uint8 addr);

extern int console_vector;
