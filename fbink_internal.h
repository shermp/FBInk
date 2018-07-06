/*
	FBInk: FrameBuffer eInker, a tool to print strings on eInk devices (Kobo/Kindle)
	Copyright (C) 2018 NiLuJe <ninuje@gmail.com>

	Linux framebuffer routines based on: fbtestfnt.c & fbtest6.c, from
	http://raspberrycompote.blogspot.com/2014/04/low-level-graphics-on-raspberry-pi-text.html &
	https://raspberrycompote.blogspot.com/2013/03/low-level-graphics-on-raspberry-pi-part_8.html
	Original works by J-P Rosti (a.k.a -rst- and 'Raspberry Compote'),
	Licensed under the Creative Commons Attribution 3.0 Unported License
	(http://creativecommons.org/licenses/by/3.0/deed.en_US)

	----

	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU Affero General Public License as
	published by the Free Software Foundation, either version 3 of the
	License, or (at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU Affero General Public License for more details.

	You should have received a copy of the GNU Affero General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef __FBINK_INTERNAL_H
#define __FBINK_INTERNAL_H

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/kd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <unistd.h>

// NOTE: This is from https://www.cprogramming.com/tutorial/unicode.html
//       This helps us handling *VALID* UTF-8 properly, since our font does support extended Latin & Greek.
//       That said, we have no (easy) way of ensuring all input either is or gets converted to valid UTF-8,
//       because the libc is severely hobbled on Kobo: gconv modules are missing & locales are missing or broken,
//       (at the very least), so we can't use iconv and we can't use locale/multibyte aware libc functions,
//       which in turn renders the wchar_t data type fairly unusable.
//       FWIW, we're okay on the Kindle.
//       BusyBox can bypass some of these issues (for its applets) by basically rolling its own
//       locale/unicode/widechar/multibyte handling, provided you set it up properly ;).
//       We're left with handling UTF-8 ourselves, and taking great pains to try not to horribly blow up on invalid input.
//
//       TL;DR; for API users: You have to ensure you feed FBInk valid UTF-8 input,
//              as this is the encoding it effectively uses internally, without any kind of validation.
//
//       Further reading on the subject, in no particular order:
//           https://github.com/benkasminbullock/unicode-c
//           https://stackoverflow.com/q/7298059
//           https://github.com/JuliaLang/utf8proc
//           https://stackoverflow.com/q/25803627
//           https://www.gnu.org/software/libunistring
//           https://www.tldp.org/HOWTO/Unicode-HOWTO-6.html
//           https://www.cl.cam.ac.uk/~mgk25/unicode.html
//           https://unicodebook.readthedocs.io/
#include "utf8/utf8.h"

// NOTE: We always neeed one of those, because we rely on mxcfb_rect in a number of places
#ifdef FBINK_FOR_KINDLE
#	include "eink/mxcfb-kindle.h"
#else
#	include "eink/mxcfb-kobo.h"
#endif
// Legacy einkfb driver
#ifdef FBINK_FOR_LEGACY
#	include "eink/einkfb.h"
#endif

// NOTE: This is from https://github.com/dhepper/font8x8
//       See also https://github.com/achilikin/bdfe & https://unix.stackexchange.com/q/119236
//       for *potentially* fun ways to add new glyphs or just change the font ;).
//       Right now, there's a bit of a format mismatch issue...
//       But in any case, for a repository of BDF fonts, see https://github.com/Tecate/bitmap-fonts
// NOTE: On the other hand, tools/hextoc.py is a perfectly usable experiment with Unifont's hex format,
//       that can yield you a choice of a few different fonts ;).
#include "font8x8/font8x8_basic.h"
#include "font8x8/font8x8_block.h"
#include "font8x8/font8x8_box.h"
#include "font8x8/font8x8_control.h"
#include "font8x8/font8x8_ext_latin.h"
#include "font8x8/font8x8_greek.h"
#include "font8x8/font8x8_hiragana.h"

// Where our (internal) typedefs dwell...
#include "fbink_types.h"

// Speaking of, include the Unscii variants when we're not a minimal build
#ifndef FBINK_MINIMAL
#	include "fbink_unscii.h"
#endif

// Fallback version tag...
#ifndef FBINK_VERSION
#	define FBINK_VERSION "v0.9.13"
#endif

// NOTE: Some of our ifdef combinations may cause a small number of function arguments to become unused...
//       Handle the warnings in these cases with a bit of trickery,
//       by conditionally marking theses arguments as unused ;).
#ifdef FBINK_FOR_LEGACY
#	define UNUSED_BY_LEGACY __attribute__((unused))
#else
#	define UNUSED_BY_LEGACY
#endif
#ifdef FBINK_MINIMAL
#	define UNUSED_BY_MINIMAL __attribute__((unused))
#else
#	define UNUSED_BY_MINIMAL
#endif

// Handle what we send to stdout (i.e., mostly diagnostic stuff)
#define LOG(fmt, ...)                                                                                                  \
	({                                                                                                             \
		if (g_isVerbose) {                                                                                     \
			fprintf(stdout, fmt "\n", ##__VA_ARGS__);                                                      \
		}                                                                                                      \
	})

// And then what we send to stderr (mostly fbink_init stuff)
#define ELOG(fmt, ...)                                                                                                 \
	({                                                                                                             \
		if (!g_isQuiet) {                                                                                      \
			fprintf(stderr, fmt "\n", ##__VA_ARGS__);                                                      \
		}                                                                                                      \
	})

static unsigned short def_r[] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
				  0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF };
static unsigned short def_g[] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
				  0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF };
static unsigned short def_b[] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
				  0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF };

// 'global' variables to store screen info
// With our externs, first...
unsigned char* g_fbink_fbp        = 0U;
size_t         g_fbink_screensize = 0U;
bool           g_fbink_isFbMapped = false;
// And those stay purely inside the library
struct fb_var_screeninfo vinfo;
struct fb_fix_screeninfo finfo;
uint32_t                 viewWidth;
uint32_t                 viewHeight;
unsigned short int       FONTW         = 8U;
unsigned short int       FONTH         = 8U;
unsigned short int       FONTSIZE_MULT = 1U;
// Slightly arbitrary-ish fallback values
unsigned short int MAXROWS = 45U;
unsigned short int MAXCOLS = 32U;
// Verbose is for diagnostic/debug info in general
bool g_isVerbose = false;
// Quiet is for fbink_init's hardware setup info
bool g_isQuiet = false;
// This should be a pretty accurate fallback...
long int USER_HZ = 100;

// Where we track device/screen-specific quirks
FBInkDeviceQuirks deviceQuirks = { 0 };

static void rotate_coordinates(FBInkCoordinates*);

static void put_pixel_Gray4(FBInkCoordinates*, unsigned short int);
static void put_pixel_Gray8(FBInkCoordinates*, unsigned short int);
static void put_pixel_RGB24(FBInkCoordinates*, unsigned short int, unsigned short int, unsigned short int);
static void put_pixel_RGB32(FBInkCoordinates*, unsigned short int, unsigned short int, unsigned short int);
static void put_pixel_RGB565(FBInkCoordinates*, unsigned short int, unsigned short int, unsigned short int);
static void put_pixel(unsigned short int, unsigned short int, unsigned short int);

static void fill_rect(unsigned short int,
		      unsigned short int,
		      unsigned short int,
		      unsigned short int,
		      unsigned short int);
static void clear_screen(unsigned short int);

static const unsigned char* font8x8_get_bitmap(uint32_t);
static void                 font8x8_render(uint32_t, unsigned char*, unsigned short int UNUSED_BY_MINIMAL);

static struct mxcfb_rect
    draw(const char*, unsigned short int, unsigned short int, bool, bool, unsigned short int, unsigned short int, bool);

#ifdef FBINK_FOR_LEGACY
static int refresh_legacy(int, const struct mxcfb_rect, bool);
#else
static long int jiffies_to_ms(long int);
#	ifdef FBINK_FOR_KINDLE
static int      refresh_kindle(int, const struct mxcfb_rect, uint32_t, uint32_t, uint32_t);
static int      refresh_kindle_koa2(int, const struct mxcfb_rect, uint32_t, uint32_t, uint32_t);
#	else
static int refresh_kobo(int, const struct mxcfb_rect, uint32_t, uint32_t, uint32_t);
static int refresh_kobo_mk7(int, const struct mxcfb_rect, uint32_t, uint32_t, uint32_t);
#	endif    // FBINK_FOR_KINDLE
#endif            // FBINK_FOR_LEGACY
static int refresh(int, const struct mxcfb_rect, uint32_t UNUSED_BY_LEGACY, bool);

static const char* fb_rotate_to_string(uint32_t);

// For identify_device
#include "fbink_device_id.h"

#endif
