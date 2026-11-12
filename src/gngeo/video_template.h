/* Tile drawing template
   use RENAME to set the name of the function
   use PUTPIXEL(dest,src) to set the putpixel function/macro
*/

/* .gno payloads are often not 4-byte aligned; Cortex-M7 LDRD on an
 * unaligned address UsageFaults. Force byte-safe loads. */
typedef uint32_t neo_u32_ua __attribute__((aligned(1)));

/* An opaque row (no transparent pixel: no zero nibble in either 32-bit
 * word) is written without the per-pixel test. xflip: pixel i is nibble i
 * of word 1, then of word 0; no xflip: nibble 7-i of word 0, then of word
 * 1 -- the order the per-pixel code below uses. NEO_TILE_PAIRS (the plain
 * variant, which does not read the destination): two pixels per 32-bit
 * store, the odd first or last one alone when the row is not 4-byte
 * aligned (catalogue §8/§8a). Output identical either way. */
#ifndef NEO_TILE_ROW_HELPERS
#define NEO_TILE_ROW_HELPERS
#define NEO_NIB_HASZERO(w) (((w) - 0x11111111u) & ~(w) & 0x88888888u)
#define NEO_NIB(i) (xf ? ((i) < 8 ? (w1 >> (4 * (i))) & 15 : (w0 >> (4 * ((i) - 8))) & 15) \
                       : ((i) < 8 ? (w0 >> (28 - 4 * (i))) & 15 : (w1 >> (28 - 4 * ((i) - 8))) & 15))
typedef Uint32 neo_u32_alias __attribute__((may_alias));
#endif

/* One 16-pixel row from its two words, in the order of the original code:
 * xflip, pixel i is nibble i of w1 then of w0; no xflip, nibble 7-i of w0
 * then of w1. A row with no transparent pixel skips the per-pixel test,
 * and the plain variant (NEO_TILE_PAIRS) stores two pixels at a time.
 * Each path walks its OWN copy of the words with shifts, laundered through
 * an empty asm -- GCC: otherwise it computes all 16 pixel values of both
 * paths before the opaque test (shared subexpressions) and spills 13 of
 * them to the stack on every row. Drop the launders only with a check of
 * the device code that this does not come back. */
#ifndef NEO_TILE_ROW_STEP
#define NEO_TILE_ROW_STEP
#define NEO_LAUNDER(v) __asm__("" : "+r"(v))
/* the next pixel's colour index, then advance: xflip from the low nibble */
#define NEO_TAKE(a, xf) ((xf) ? ((a) & 15u) : ((a) >> 28))
#define NEO_TAKE2(a, xf) ((xf) ? (((a) >> 4) & 15u) : (((a) >> 24) & 15u))
#define NEO_NEXT(a, xf, n) do { if (xf) (a) >>= (n); else (a) <<= (n); NEO_LAUNDER(a); } while (0)
#endif

static __inline__ __attribute__((always_inline)) void RENAME(row16)(unsigned short *br,
		const Uint16 *pal, const unsigned int w0, const unsigned int w1, const int xf)
{
    unsigned int a = xf ? w1 : w0, b = xf ? w0 : w1, col;
    int i;
    if (!(NEO_NIB_HASZERO(w0) | NEO_NIB_HASZERO(w1))) {
        NEO_LAUNDER(a);
        NEO_LAUNDER(b);
#ifdef NEO_TILE_PAIRS
        if (!((uintptr_t)br & 2)) {
            neo_u32_alias *d = (neo_u32_alias *)br;
            _Pragma("GCC unroll 8")
            for (i = 0; i < 4; i++) {
                d[i] = pal[NEO_TAKE(a, xf)] | ((Uint32)pal[NEO_TAKE2(a, xf)] << 16);
                NEO_NEXT(a, xf, 8);
            }
            _Pragma("GCC unroll 8")
            for (i = 4; i < 8; i++) {
                d[i] = pal[NEO_TAKE(b, xf)] | ((Uint32)pal[NEO_TAKE2(b, xf)] << 16);
                NEO_NEXT(b, xf, 8);
            }
            return;
        }
#endif
        _Pragma("GCC unroll 8")
        for (i = 0; i < 8; i++) {
            PUTPIXEL(br[i], pal[NEO_TAKE(a, xf)]);
            NEO_NEXT(a, xf, 4);
        }
        _Pragma("GCC unroll 8")
        for (i = 8; i < 16; i++) {
            PUTPIXEL(br[i], pal[NEO_TAKE(b, xf)]);
            NEO_NEXT(b, xf, 4);
        }
        return;
    }
    NEO_LAUNDER(a);
    NEO_LAUNDER(b);
    _Pragma("GCC unroll 8")
    for (i = 0; i < 8; i++) {
        col = NEO_TAKE(a, xf);
        if (col)
            PUTPIXEL(br[i], pal[col]);
        NEO_NEXT(a, xf, 4);
    }
    _Pragma("GCC unroll 8")
    for (i = 8; i < 16; i++) {
        col = NEO_TAKE(b, xf);
        if (col)
            PUTPIXEL(br[i], pal[col]);
        NEO_NEXT(b, xf, 4);
    }
}

/* The full-width rows of a tile (zx 16): each row's two words loaded once
 * -- word loads when the tile data is 4-byte aligned (always, from the zip
 * cache; checked once per tile, tiles are 128 bytes), byte-safe loads
 * otherwise (.gno payloads may not be: LDRD there faults) -- then the
 * fast or the per-pixel path. The pitch is a local: the pixel stores could
 * alias buffer->pitch, which GCC otherwise reloads every row. */
static __inline__ __attribute__((always_inline)) void RENAME(rows16)(unsigned short *br,
		int step, const neo_u32_ua *g, const char *l_y_skip, int zy, const Uint16 *pal,
		const int xf)
{
    int y;
    if (!((uintptr_t)g & 3)) {
        const Uint32 *ga = (const Uint32 *)g;
        for (y = 0; y < zy; y++) {
            unsigned int w0, w1;
            ga += l_y_skip[y] << 1;
            w0 = ga[0];
            w1 = ga[1];
            if (w0 | w1)
                RENAME(row16)(br, pal, w0, w1, xf);
            br += step;
        }
    } else {
        for (y = 0; y < zy; y++) {
            unsigned int w0, w1;
            g += l_y_skip[y] << 1;
            w0 = g[0];
            w1 = g[1];
            if (w0 | w1)
                RENAME(row16)(br, pal, w0, w1, xf);
            br += step;
        }
    }
}

/* A narrow (x-zoomed) row: exactly the original per-position code -- for
 * each of the 16 source pixels its skip byte, then the colour test (GCC
 * gives a skipped position two instructions, a kept one eight; a
 * predicated version cost eight for both and was slower) -- on the two
 * words the caller loaded once. Same pixel order as rows16's. */
static __inline__ __attribute__((always_inline)) void RENAME(rowN)(unsigned short *br,
		const Uint16 *pal, const unsigned int w0, const unsigned int w1,
		const char *dda, const int xf)
{
    unsigned int col;
    const unsigned int a = xf ? w1 : w0, b = xf ? w0 : w1;
#define NEO_NPIX(i, w, sh) if (dda[i]) { col = ((w) >> (sh)) & 0xf; if (col) PUTPIXEL(*br, pal[col]); br++; }
    if (xf) {
        NEO_NPIX(0, a, 0)   NEO_NPIX(1, a, 4)   NEO_NPIX(2, a, 8)   NEO_NPIX(3, a, 12)
        NEO_NPIX(4, a, 16)  NEO_NPIX(5, a, 20)  NEO_NPIX(6, a, 24)  NEO_NPIX(7, a, 28)
        NEO_NPIX(8, b, 0)   NEO_NPIX(9, b, 4)   NEO_NPIX(10, b, 8)  NEO_NPIX(11, b, 12)
        NEO_NPIX(12, b, 16) NEO_NPIX(13, b, 20) NEO_NPIX(14, b, 24) NEO_NPIX(15, b, 28)
    } else {
        NEO_NPIX(0, a, 28)  NEO_NPIX(1, a, 24)  NEO_NPIX(2, a, 20)  NEO_NPIX(3, a, 16)
        NEO_NPIX(4, a, 12)  NEO_NPIX(5, a, 8)   NEO_NPIX(6, a, 4)   NEO_NPIX(7, a, 0)
        NEO_NPIX(8, b, 28)  NEO_NPIX(9, b, 24)  NEO_NPIX(10, b, 20) NEO_NPIX(11, b, 16)
        NEO_NPIX(12, b, 12) NEO_NPIX(13, b, 8)  NEO_NPIX(14, b, 4)  NEO_NPIX(15, b, 0)
    }
#undef NEO_NPIX
}

/* The narrow rows of a tile, loaded as rows16 does (word loads when the
 * tile data is aligned); an empty row (both words 0) draws nothing. */
static __inline__ __attribute__((always_inline)) void RENAME(rowsN)(unsigned short *br,
		int step, const neo_u32_ua *g, const char *l_y_skip, int zy, const Uint16 *pal,
		const char *dda, const int xf)
{
    int y;
    if (!((uintptr_t)g & 3)) {
        const Uint32 *ga = (const Uint32 *)g;
        for (y = 0; y < zy; y++) {
            unsigned int w0, w1;
            ga += l_y_skip[y] << 1;
            w0 = ga[0];
            w1 = ga[1];
            if (w0 | w1)
                RENAME(rowN)(br, pal, w0, w1, dda, xf);
            br += step;
        }
    } else {
        for (y = 0; y < zy; y++) {
            unsigned int w0, w1;
            g += l_y_skip[y] << 1;
            w0 = g[0];
            w1 = g[1];
            if (w0 | w1)
                RENAME(rowN)(br, pal, w0, w1, dda, xf);
            br += step;
        }
    }
}

/* GCC: noinline -- with one caller left it is inlined into draw_screen,
 * which has no free register (its loops then spill; see neo_yzoom_table) */
static __attribute__((noinline)) void RENAME(draw)(unsigned int tileno,int sx,int sy,int zx,int zy,
					 int color,int xflip,int yflip,unsigned char *bmp)
{
    neo_u32_ua *gfxdata;
    unsigned int myword;
    int y;
    unsigned char col;
    unsigned short *br;
    Uint16 *paldata=&current_pc_pal[16*color];
    char *l_y_skip;
    int l; // Line skipping counter
#ifdef DEBUG_VIDEO
    int buf_w=544-zx;
    int buf_w_yflip=544+zx;
#else
    int buf_w=(buffer->pitch>>1)-zx;
    int buf_w_yflip=(buffer->pitch>>1)+zx;
#endif
    if (!memory.rom.tiles.p || memory.nb_of_tiles == 0)
        return;
    tileno=tileno%memory.nb_of_tiles;
   
    gfxdata = (neo_u32_ua *)&memory.rom.tiles.p[ tileno<<7];

    /* y zoom table */
    if(zy==16)
        l_y_skip=full_y_skip;
    else
        l_y_skip=dda_y_skip;

    if (zx==16) {
        const int pitch16 = buffer->pitch >> 1;
        if (yflip)
            br = (unsigned short *)bmp + ((zy - 1) + sy) * pitch16 + sx;
        else
            br = (unsigned short *)bmp + sy * pitch16 + sx;
        if (xflip)
            RENAME(rows16)(br, yflip ? -pitch16 : pitch16, gfxdata, l_y_skip, zy, paldata, 1);
        else
            RENAME(rows16)(br, yflip ? -pitch16 : pitch16, gfxdata, l_y_skip, zy, paldata, 0);
    }else { // zx!=16
        const int pitch16 = buffer->pitch >> 1;

        if (yflip)
            br = (unsigned short *)bmp + ((zy - 1) + sy) * pitch16 + sx;
        else
            br = (unsigned short *)bmp + sy * pitch16 + sx;
        if (xflip)
            RENAME(rowsN)(br, yflip ? -pitch16 : pitch16, gfxdata, l_y_skip, zy, paldata, dda_x_skip, 1);
        else
            RENAME(rowsN)(br, yflip ? -pitch16 : pitch16, gfxdata, l_y_skip, zy, paldata, dda_x_skip, 0);
    }
}

static inline void RENAME(draw_scanline)(unsigned int tileno,int yoffs,int sx,int line,int zx,
					 int color,int xflip,unsigned char *bmp)
{
    unsigned int *gfxdata,myword;
    unsigned char col;
    unsigned short *br;
    Uint16 *paldata=&current_pc_pal[16*color];

    tileno=tileno%memory.nb_of_tiles;
    gfxdata = (unsigned int *)&memory.rom.tiles.p[ (tileno<<7)];
    gfxdata+=(yoffs<<1);
  
    if (gfxdata[1]+gfxdata[0]==0) return;

    if (zx==16) {
        if (xflip)
        {
#ifdef DEBUG_VIDEO
            br= (unsigned short *)bmp+(line)*(512+32)+sx;
#else
            br= (unsigned short *)bmp+(line)*(buffer->pitch>>1)+sx;
#endif

            myword = gfxdata[1];
            col=(myword>>0)&0xf; if (col) PUTPIXEL(*br,paldata[col]); br++;
            col=(myword>>4)&0xf; if (col) PUTPIXEL(*br,paldata[col]); br++;
            col=(myword>>8)&0xf; if (col) PUTPIXEL(*br,paldata[col]); br++;
            col=(myword>>12)&0xf; if (col) PUTPIXEL(*br,paldata[col]); br++;
            col=(myword>>16)&0xf; if (col) PUTPIXEL(*br,paldata[col]); br++;
            col=(myword>>20)&0xf; if (col) PUTPIXEL(*br,paldata[col]); br++;
            col=(myword>>24)&0xf; if (col) PUTPIXEL(*br,paldata[col]); br++;
            col=(myword>>28)&0xf; if (col) PUTPIXEL(*br,paldata[col]); br++;

            myword = gfxdata[0];
            col=(myword>>0)&0xf; if (col) PUTPIXEL(*br,paldata[col]); br++;
            col=(myword>>4)&0xf; if (col) PUTPIXEL(*br,paldata[col]); br++;
            col=(myword>>8)&0xf; if (col) PUTPIXEL(*br,paldata[col]); br++;
            col=(myword>>12)&0xf; if (col) PUTPIXEL(*br,paldata[col]); br++;
            col=(myword>>16)&0xf; if (col) PUTPIXEL(*br,paldata[col]); br++;
            col=(myword>>20)&0xf; if (col) PUTPIXEL(*br,paldata[col]); br++;
            col=(myword>>24)&0xf; if (col) PUTPIXEL(*br,paldata[col]); br++;
            col=(myword>>28)&0xf; if (col) PUTPIXEL(*br,paldata[col]); br++;
        }else {
#ifdef DEBUG_VIDEO
            br= (unsigned short *)bmp+(line)*(512+32)+sx;
#else
            br= (unsigned short *)bmp+(line)*(buffer->pitch>>1)+sx;
#endif
            myword = gfxdata[0];
            col=(myword>>28)&0xf; if (col) PUTPIXEL(*br,paldata[col]);br++; 
            col=(myword>>24)&0xf; if (col) PUTPIXEL(*br,paldata[col]);br++; 
            col=(myword>>20)&0xf; if (col) PUTPIXEL(*br,paldata[col]);br++; 
            col=(myword>>16)&0xf; if (col) PUTPIXEL(*br,paldata[col]);br++; 
            col=(myword>>12)&0xf; if (col) PUTPIXEL(*br,paldata[col]);br++; 
            col=(myword>>8)&0xf; if (col) PUTPIXEL(*br,paldata[col]);br++; 
            col=(myword>>4)&0xf; if (col) PUTPIXEL(*br,paldata[col]);br++; 
            col=(myword>>0)&0xf; if (col) PUTPIXEL(*br,paldata[col]);br++; 
	      
            myword = gfxdata[1];
            col=(myword>>28)&0xf; if (col) PUTPIXEL(*br,paldata[col]);br++; 
            col=(myword>>24)&0xf; if (col) PUTPIXEL(*br,paldata[col]);br++; 
            col=(myword>>20)&0xf; if (col) PUTPIXEL(*br,paldata[col]);br++; 
            col=(myword>>16)&0xf; if (col) PUTPIXEL(*br,paldata[col]);br++; 
            col=(myword>>12)&0xf; if (col) PUTPIXEL(*br,paldata[col]);br++; 
            col=(myword>>8)&0xf; if (col) PUTPIXEL(*br,paldata[col]);br++; 
            col=(myword>>4)&0xf; if (col) PUTPIXEL(*br,paldata[col]);br++; 
            col=(myword>>0)&0xf; if (col) PUTPIXEL(*br,paldata[col]);br++; 
        }
    }else { // zx!=16
        if (xflip) {
#ifdef DEBUG_VIDEO
            br= (unsigned short *)bmp+(line)*(512+32)+sx;
#else
            br= (unsigned short *)bmp+(line)*(buffer->pitch>>1)+sx;
#endif
            myword = gfxdata[1];
            if (dda_x_skip[ 0]) {if  ((col=((myword>>0)&0xf))) PUTPIXEL(*br,paldata[col]);br++;} 
            if (dda_x_skip[ 1]) {if  ((col=((myword>>4)&0xf))) PUTPIXEL(*br,paldata[col]);br++;} 
            if (dda_x_skip[ 2]) {if  ((col=((myword>>8)&0xf))) PUTPIXEL(*br,paldata[col]);br++;} 
            if (dda_x_skip[ 3]) {if  ((col=((myword>>12)&0xf))) PUTPIXEL(*br,paldata[col]);br++;} 
            if (dda_x_skip[ 4]) {if  ((col=((myword>>16)&0xf))) PUTPIXEL(*br,paldata[col]);br++;} 
            if (dda_x_skip[ 5]) {if  ((col=((myword>>20)&0xf))) PUTPIXEL(*br,paldata[col]);br++;} 
            if (dda_x_skip[ 6]) {if  ((col=((myword>>24)&0xf))) PUTPIXEL(*br,paldata[col]);br++;} 
            if (dda_x_skip[ 7]) {if  ((col=((myword>>28)&0xf))) PUTPIXEL(*br,paldata[col]);br++;} 

            myword = gfxdata[0];
            if (dda_x_skip[ 8]) {if  ((col=((myword>>0)&0xf))) PUTPIXEL(*br,paldata[col]);br++;} 
            if (dda_x_skip[ 9]) {if  ((col=((myword>>4)&0xf))) PUTPIXEL(*br,paldata[col]);br++;} 
            if (dda_x_skip[10]) {if  ((col=((myword>>8)&0xf))) PUTPIXEL(*br,paldata[col]);br++;} 
            if (dda_x_skip[11]) {if  ((col=((myword>>12)&0xf))) PUTPIXEL(*br,paldata[col]);br++;} 
            if (dda_x_skip[12]) {if  ((col=((myword>>16)&0xf))) PUTPIXEL(*br,paldata[col]);br++;} 
            if (dda_x_skip[13]) {if  ((col=((myword>>20)&0xf))) PUTPIXEL(*br,paldata[col]);br++;} 
            if (dda_x_skip[14]) {if  ((col=((myword>>24)&0xf))) PUTPIXEL(*br,paldata[col]);br++;} 
            if (dda_x_skip[15]) {if  ((col=((myword>>28)&0xf))) PUTPIXEL(*br,paldata[col]);br++;} 
	  
        }else {
#ifdef DEBUG_VIDEO
            br= (unsigned short *)bmp+(line)*(512+32)+sx;
#else
            br= (unsigned short *)bmp+(line)*(buffer->pitch>>1)+sx;
#endif

            myword = gfxdata[0];
            if (dda_x_skip[ 0]) {if ((col=((myword>>28)&0xf))) PUTPIXEL(*br,paldata[col]);br++;} 
            if (dda_x_skip[ 1]) {if ((col=((myword>>24)&0xf))) PUTPIXEL(*br,paldata[col]);br++;} 
            if (dda_x_skip[ 2]) {if ((col=((myword>>20)&0xf))) PUTPIXEL(*br,paldata[col]);br++;} 
            if (dda_x_skip[ 3]) {if ((col=((myword>>16)&0xf))) PUTPIXEL(*br,paldata[col]);br++;} 
            if (dda_x_skip[ 4]) {if ((col=((myword>>12)&0xf))) PUTPIXEL(*br,paldata[col]);br++;} 
            if (dda_x_skip[ 5]) {if ((col=((myword>>8)&0xf))) PUTPIXEL(*br,paldata[col]);br++;} 
            if (dda_x_skip[ 6]) {if ((col=((myword>>4)&0xf))) PUTPIXEL(*br,paldata[col]);br++;} 
            if (dda_x_skip[ 7]) {if ((col=((myword>>0)&0xf))) PUTPIXEL(*br,paldata[col]);br++;} 
	      
            myword = gfxdata[1];
            if (dda_x_skip[ 8]) {if ((col=((myword>>28)&0xf))) PUTPIXEL(*br,paldata[col]);br++;} 
            if (dda_x_skip[ 9]) {if ((col=((myword>>24)&0xf))) PUTPIXEL(*br,paldata[col]);br++;} 
            if (dda_x_skip[10]) {if ((col=((myword>>20)&0xf))) PUTPIXEL(*br,paldata[col]);br++;} 
            if (dda_x_skip[11]) {if ((col=((myword>>16)&0xf))) PUTPIXEL(*br,paldata[col]);br++;} 
            if (dda_x_skip[12]) {if ((col=((myword>>12)&0xf))) PUTPIXEL(*br,paldata[col]);br++;} 
            if (dda_x_skip[13]) {if ((col=((myword>>8)&0xf))) PUTPIXEL(*br,paldata[col]);br++;} 
            if (dda_x_skip[14]) {if ((col=((myword>>4)&0xf))) PUTPIXEL(*br,paldata[col]);br++;} 
            if (dda_x_skip[15]) {if ((col=((myword>>0)&0xf))) PUTPIXEL(*br,paldata[col]);br++;} 
	  
        }
    }
}


#undef RENAME
#undef PUTPIXEL
