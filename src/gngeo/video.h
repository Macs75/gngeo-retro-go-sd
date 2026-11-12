/*  gngeo a neogeo emulator
 *  Copyright (C) 2001 Peponas Mathieu
 * 
 *  This program is free software; you can redistribute it and/or modify  
 *  it under the terms of the GNU General Public License as published by   
 *  the Free Software Foundation; either version 2 of the License, or    
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Library General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA. 
 */

#ifndef _VIDEO_H_
#define _VIDEO_H_

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "SDL.h"

typedef struct gfx_cache {
	Uint8 *data;  /* The cache */
	Uint32 size;  /* Tha allocated size of the cache */      
	Uint32 total_bank;  /* total number of rom bank */
	Uint8 **ptr/*[TOTAL_GFX_BANK]*/; /* ptr[i] Contain a pointer to cached data for bank i */
	int max_slot; /* Maximal numer of bank that can be cached (depend on cache size) */
	int slot_size;
	int *usage;   /* contain index to the banks in used order */
	FILE *gno;
    Uint32 *offset;
    Uint8* in_buf;
}GFX_CACHE;

#define VRAM_BYTES 0x11000
/* a VRAM word address through the LSPC address register: the upper 2K
 * words repeat over 0x8000-0xFFFF, as MAME's set_videoram_offset */
#define VRAM_WORD(o) (((o) & 0x8000) ? (0x8000 | ((o) & 0x7ff)) : ((o) & 0x7fff))

typedef struct VIDEO {
	/* Video Ram&Pal. The LSPC's VRAM: 0x8000 words (SCB1 + fix) and 0x800
	 * upper words (SCB2-4), mirrored over 0x8000-0xFFFF (VRAM_WORD) */
	Uint8 ram[VRAM_BYTES];
	Uint8 pal_neo[2][0x2000];
	Uint16 pal_host[2][0x1000];     /* RGB565, the screen's format */
	Uint8 currentpal;
        Uint8 currentfix; /* 0=bios fix */
	Uint16 rbuf;

	/* Auto anim counter */
	Uint32 fc;
	Uint32 fc_speed;

	Uint32 vptr;
	Sint16 modulo;

	Uint32 current_line;

	/* IRQ2 related */
	Uint32 irq2control;
	Uint32 irq2taken;
	Uint32 irq2start;
	Uint32 irq2pos;

    GFX_CACHE spr_cache;
}VIDEO;

#define RASTER_LINES 261

extern unsigned int neogeo_frame_counter;
extern unsigned int neogeo_frame_counter_speed;

void init_video(void);
void debug_draw_tile(unsigned int tileno,int sx,int sy,int zx,int zy,
		     int color,int xflip,int yflip,unsigned char *bmp);
void draw_screen_scanline(int start_line, int end_line, int refresh);
void draw_screen(void);
// void show_cache(void);
int init_sprite_cache(Uint32 size,Uint32 bsize);
void free_sprite_cache(void);

#endif
