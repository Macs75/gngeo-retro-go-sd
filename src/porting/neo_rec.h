#ifndef NEO_REC_H
#define NEO_REC_H

#include <stdint.h>

/* input record / replay (src/porting/neo_rec.c) */
enum { NEO_REC_OFF = 0, NEO_REC_RECORD, NEO_REC_REPLAY };

int neo_rec_mode(void);
/* opens "<rom path without extension>.rec" and powers the machine on;
 * 0 (logged) if the file cannot be used */
int neo_rec_start(int mode, const char *rom_path);
/* closes the file (a recording gets its frame count); the game goes on */
void neo_rec_close(void);
/* the input word the emulated frame gets: recorded, or replayed in place
 * of the live one */
uint16_t neo_rec_input(uint16_t live);
/* after each emulated frame, with its audio (before the volume) */
void neo_rec_frame_done(const int16_t *audio, int n);
/* every piece of machine state as at power-on */
void neo_rec_power_on(void);

#endif
