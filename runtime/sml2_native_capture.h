#pragma once

#include "sml2_native_render.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    SML2_NATIVE_CAPTURE_OK = 0,
    SML2_NATIVE_CAPTURE_INVALID = -1,
    SML2_NATIVE_CAPTURE_WRONG_SCENE = -2,
    SML2_NATIVE_CAPTURE_BAD_STAGE = -3,
    SML2_NATIVE_CAPTURE_VIEW_MISMATCH = -4
};

/* A read-only snapshot of one live, ordinary SML2 gameplay scene. The map,
 * block definitions, and VRAM are copied into owned storage so later emulator
 * ticks cannot mutate a capture while the native renderer is consuming it.
 *
 * collision_flags may optionally override the built-in SML2 metatile-ID
 * classifications. Passing NULL uses the verified global ID ranges for
 * solid ground, one-way slopes/platforms, coins, exits, water, hazards and
 * low-gravity tiles. Exact slope geometry and interactions remain separate. */
typedef struct {
    uint8_t cells[SML2_NATIVE_SML2_MAP_BYTES];
    uint8_t block_definitions[SML2_NATIVE_SML2_BLOCKDEF_BYTES];
    Sml2NativeStageTile tiles[SML2_NATIVE_SML2_BLOCK_COUNT];
    uint8_t vram[0x2000];
    uint8_t valid;
    int32_t world_origin_x;
    int32_t world_origin_y;
    uint8_t lcdc;
    uint8_t bgp;
    uint8_t scx;
    uint8_t scy;
    unsigned validation_samples;
    unsigned validation_matches;
} Sml2NativeSml2Capture;

/* Capture only during the ordinary SML2 gameplay play/pause/death scene.
 * ppu_vram is bank 0 (the original SML2 cartridge is DMG); the current
 * decompressed world map and block definitions are read through read_memory.
 *
 * The capture is accepted only if at least min_samples native-screen samples
 * agree with the live PPU tilemap and the match percentage reaches
 * min_match_percent. The game's HUD/window configuration and bonus-room
 * state are checked before reading stage data. Camera coordinates are read
 * from the game's verified $FFCA/$FFC8 camera words. */
/* Rebind the internal caller-owned arrays after a successful capture. These
 * helpers avoid storing self-referential pointers inside the capture object,
 * so the snapshot may safely be copied or moved by its owner. */
int sml2_native_capture_get_stage(const Sml2NativeSml2Capture *capture,
                                  Sml2NativeStage *stage);
int sml2_native_capture_get_graphics(const Sml2NativeSml2Capture *capture,
                                     Sml2NativeGbGraphics *graphics);

/* Build a clamped integer-pixel viewport for the native stage renderer from
 * a verified live capture. left is the logical display's left inset relative
 * to the captured 160px view. sprite offsets preserve alignment when the
 * viewport is clamped at a physical level edge. */
int sml2_native_capture_make_view(const Sml2NativeSml2Capture *capture,
                                  int32_t left, int32_t width, int32_t height,
                                  Sml2NativeWorld *world,
                                  int32_t *sprite_offset_x,
                                  int32_t *sprite_offset_y);

int sml2_native_capture_live_sml2(
    Sml2NativeSml2Capture *capture,
    Sml2NativeMemoryRead read_memory, void *memory_userdata,
    const uint8_t ppu_vram[0x2000],
    uint8_t lcdc, uint8_t scx, uint8_t scy,
    uint8_t wx, uint8_t wy, uint8_t bgp,
    const uint16_t *collision_flags, size_t collision_flags_count,
    unsigned min_samples, unsigned min_match_percent);

/* Convert raw 2-bit Game Boy tile color indices to the active DMG BGP shade
 * indices in-place after sml2_native_render_stage(). */
int sml2_native_apply_dmg_palette(uint8_t *pixels, size_t width, size_t height,
                                  size_t pitch, size_t capacity, uint8_t bgp);

#ifdef __cplusplus
}
#endif
