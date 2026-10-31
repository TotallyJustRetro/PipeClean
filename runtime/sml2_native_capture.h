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
 * collision_flags is supplied by the caller: graphics IDs do not describe
 * collision behavior. Passing NULL creates a visual-only snapshot whose
 * collision map is intentionally non-solid. */
typedef struct {
    Sml2NativeStage stage;
    uint8_t cells[SML2_NATIVE_SML2_MAP_BYTES];
    uint8_t block_definitions[SML2_NATIVE_SML2_BLOCKDEF_BYTES];
    Sml2NativeStageTile tiles[SML2_NATIVE_SML2_BLOCK_COUNT];
    uint8_t vram[0x2000];
    Sml2NativeGbGraphics graphics;
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
int sml2_native_capture_live_sml2(
    Sml2NativeSml2Capture *capture,
    Sml2NativeMemoryRead read_memory, void *memory_userdata,
    const uint8_t ppu_vram[0x2000],
    uint8_t lcdc, uint8_t scx, uint8_t scy,
    uint8_t wx, uint8_t wy, uint8_t bgp,
    const uint8_t *collision_flags, size_t collision_flags_count,
    unsigned min_samples, unsigned min_match_percent);

/* Convert raw 2-bit Game Boy tile color indices to the active DMG BGP shade
 * indices in-place after sml2_native_render_stage(). */
int sml2_native_apply_dmg_palette(uint8_t *pixels, size_t width, size_t height,
                                  size_t pitch, size_t capacity, uint8_t bgp);

#ifdef __cplusplus
}
#endif
