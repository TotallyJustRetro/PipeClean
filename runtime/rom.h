#pragma once
#include <stdint.h>
#include <stddef.h>
#include "games.h"
typedef struct {
    int ok;
    int hack_loaded;
    int changed_bytes;
    int code_changed;
    int exact;
    char msg[256];
} RomStatus;
int rom_identify(const uint8_t *img, size_t n);
int rom_identify_file(const char *path);
int rom_probe(int game, const char *path, RomStatus *st);
int rom_load(int game, const char *path, RomStatus *st);
int rom_apply_hack(const char *path, RomStatus *st);
void rom_clear_hack(RomStatus *st);
int rom_needs_interpreter(void);
int rom_loaded_game(void);
int rom_hack_active(void);
void rom_scan(char paths[N_GAMES][512]);
int rom_load_raw(const char *path);
extern const uint8_t recomp_code_mask[0x1000];