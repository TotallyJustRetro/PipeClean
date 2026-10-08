#pragma once
#include <stdint.h>
#include "emu.h"
#include "settings.h"
typedef struct { int m; uint32_t *px; } TexTile;
uint64_t tile_hash16(const uint8_t *t);
int texpack_load(const char *dir);
int texpack_count(void);
int texpack_scale(void);
const TexTile *texpack_find(uint64_t hash);
void tex_collect_begin(int game);
void tex_collect_frame(const Frame *f);
void tex_collect_save(void);
int tex_collected(void);
void tex_collect_clear(void);
int tex_export(const char *dir, const Palette *pal, int scale, char *msg, size_t n);