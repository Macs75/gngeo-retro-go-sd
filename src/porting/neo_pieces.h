#ifndef NEO_PIECES_H
#define NEO_PIECES_H

#include <stdint.h>

/*
 * The sprite tiles (C) and the YM2610 samples (V) are cached in flash as
 * pieces of NEO_PIECE bytes (the last one shorter), each a blob of its
 * own: the flash cache stores a blob contiguously, and a large set leaves
 * no run long enough for a whole region (preisle2: a 48 MiB C and then a
 * 6 MiB V in 60 MiB). A piece fits any free run of NEO_PIECE bytes
 * (gw-arcade-core's rom_xform_chunks, the same reason).
 *
 * 512 KiB: a run too short for the next piece stays empty, so the piece
 * bounds the waste. tools/neo_flash_fit.py, the firmware's placement for
 * every .ro position and write pointer, preisle2 (59.35 of 59.9 MiB):
 * fails in 84% of cases with 4 MiB pieces, 43% with 1 MiB, 0.0% with
 * 512 KiB (program first). ~110 of the cache's 256 entries for it.
 *
 * Readers find a byte through a table of piece addresses; two pieces are
 * never assumed adjacent. A region cached whole (a .gno) fills the table
 * with addresses NEO_PIECE apart.
 */
#define NEO_PIECE_SHIFT 19u                     /* 512 KiB */
#define NEO_PIECE (1u << NEO_PIECE_SHIFT)
#define NEO_TILE_PIECES 128u                    /* 64 MiB of tiles */
#define NEO_PCM_PIECES 32u                      /* the YM2610's 16 MiB */

/* sprite tile tileno (< memory.nb_of_tiles): 128 bytes */
extern const uint8_t *neo_tile_piece[NEO_TILE_PIECES];
/* sample byte addr (24 bits): every entry set, mirrored past the region;
 * ADPCM-A, and ADPCM-B (the same region unless a .gno has its own) */
extern const uint8_t *neo_pcm_piece[NEO_PCM_PIECES];
extern const uint8_t *neo_pcmb_piece[NEO_PCM_PIECES];

static inline const uint8_t *neo_tile_data(uint32_t tileno)
{
    return neo_tile_piece[tileno >> (NEO_PIECE_SHIFT - 7u)] +
           ((tileno << 7) & (NEO_PIECE - 1u));
}

static inline uint8_t neo_pcm_byte(uint32_t addr)
{
    return neo_pcm_piece[(addr >> NEO_PIECE_SHIFT) & (NEO_PCM_PIECES - 1u)]
                        [addr & (NEO_PIECE - 1u)];
}

static inline uint8_t neo_pcmb_byte(uint32_t addr)
{
    return neo_pcmb_piece[(addr >> NEO_PIECE_SHIFT) & (NEO_PCM_PIECES - 1u)]
                         [addr & (NEO_PIECE - 1u)];
}

#endif
