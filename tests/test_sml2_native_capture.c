#include "sml2_native_capture.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

static uint8_t memory[0x10000];

static uint8_t read_memory(void *userdata, uint16_t address)
{
    (void)userdata;
    return memory[address];
}

static void set_word(uint16_t address, uint16_t value)
{
    memory[address] = (uint8_t)value;
    memory[(uint16_t)(address + 1u)] = (uint8_t)(value >> 8);
}

int main(void)
{
    static Sml2NativeSml2Capture capture;
    static uint8_t vram[0x2000];
    unsigned i;

    memset(memory, 0, sizeof(memory));
    memset(vram, 0, sizeof(vram));
    memory[0xFF9B] = 0x04; /* gameplay */
    memory[0xA28B] = 0;
    set_word(0xFFCA, 80);
    set_word(0xFFC8, 72);

    /* An all-zero stage uses block 0 and graphics tile 0 everywhere. */
    memory[0xA600] = 0;
    memory[0xA601] = 0;
    memory[0xA602] = 0;
    memory[0xA603] = 0;

    assert(sml2_native_capture_live_sml2(
        &capture, read_memory, NULL, vram,
        0xE3, 0, 0, 7, 0x88, 0xE4,
        NULL, 0, 100, 95) == SML2_NATIVE_CAPTURE_OK);
    assert(capture.stage.width_blocks == 256);
    assert(capture.stage.height_blocks == 48);
    assert(capture.stage.cells[0] == 0);
    assert(capture.tiles[0].graphics[0] == 0);
    assert(capture.graphics.vram == capture.vram);
    assert(capture.world_origin_x == 0 && capture.world_origin_y == 0);
    assert(capture.validation_samples >= 100);
    assert(capture.validation_matches == capture.validation_samples);

    /* The DMG background palette is applied after raw tile decoding. */
    {
        uint8_t pixels[8 * 2] = {0, 1, 2, 3, 0, 1, 2, 3};
        assert(sml2_native_apply_dmg_palette(pixels, 8, 1, 8,
                                             sizeof(pixels), 0xE4) == 0);
        assert(pixels[0] == 0 && pixels[1] == 1 &&
               pixels[2] == 2 && pixels[3] == 3);
        pixels[7] = 4;
        assert(sml2_native_apply_dmg_palette(pixels, 8, 1, 8,
                                             sizeof(pixels), 0xE4) == -1);
    }

    /* The Homeworld uses a different map profile and must not be decoded as
       a normal gameplay room. */
    assert(sml2_native_capture_live_sml2(
        &capture, read_memory, NULL, vram,
        0xE3, 0, 0, 80, 0x88, 0xE4,
        NULL, 0, 100, 95) == SML2_NATIVE_CAPTURE_WRONG_SCENE);

    /* A mismatched live PPU tilemap is rejected instead of displaying stale
       or unrelated stage data in the expanded viewport. */
    for (i = 0x1800; i < 0x1C00; ++i) vram[i] = 1;
    assert(sml2_native_capture_live_sml2(
        &capture, read_memory, NULL, vram,
        0xE3, 0, 0, 7, 0x88, 0xE4,
        NULL, 0, 100, 95) == SML2_NATIVE_CAPTURE_VIEW_MISMATCH);

    /* Wrong game mode and malformed collision tables fail closed. */
    memory[0xFF9B] = 0x02;
    assert(sml2_native_capture_live_sml2(
        &capture, read_memory, NULL, vram,
        0xE3, 0, 0, 7, 0x88, 0xE4,
        NULL, 0, 100, 95) == SML2_NATIVE_CAPTURE_WRONG_SCENE);
    memory[0xFF9B] = 0x04;
    assert(sml2_native_capture_live_sml2(
        &capture, read_memory, NULL, vram,
        0xE3, 0, 0, 7, 0x88, 0xE4,
        (const uint8_t *)memory, 1, 100, 95) == SML2_NATIVE_CAPTURE_INVALID);
    return 0;
}
