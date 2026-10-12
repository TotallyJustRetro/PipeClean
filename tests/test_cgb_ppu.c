#include "gb.h"
#include <assert.h>

uint8_t io_if, io_ie;
void frame_hook(void) {}

/* These stubs let the standalone PPU test exercise Homeworld's special output
 * layout without linking the full emulator or cartridge-memory implementation. */
int wide_sml2_homeworld_active(int lcdc, int wx, int wy)
{
    return lcdc == 0xE3 && wx == 80 && wy == 0x88;
}

int wide_sml2_homeworld_tile_at(int world_x, int world_y, uint8_t *tile)
{
    if (!tile || world_x < 0 || world_x >= 512 ||
        world_y < 0 || world_y >= 256) return 0;
    *tile = (uint8_t)((world_y >= 128 ? 3 : 1) +
                      (world_x >= 256 ? 1 : 0));
    return 1;
}

int wide_sml2_homeworld_camera(int scx, int scy, int *camera_x, int *camera_y)
{
    (void)scx; (void)scy;
    if (!camera_x || !camera_y) return 0;
    *camera_x = 320; *camera_y = 32;
    return 1;
}

static void set_bg_color(unsigned index, unsigned rgb555)
{
    ppu_cgb_write(0x68, (uint8_t)(0x80 | (index * 2)));
    ppu_cgb_write(0x69, (uint8_t)rgb555);
    ppu_cgb_write(0x69, (uint8_t)(rgb555 >> 8));
}

static void set_obj_color(unsigned palette, unsigned index, unsigned rgb555)
{
    unsigned off = palette * 8u + index * 2u;
    ppu_cgb_write(0x6A, (uint8_t)(0x80 | off));
    ppu_cgb_write(0x6B, (uint8_t)rgb555);
    ppu_cgb_write(0x6B, (uint8_t)(rgb555 >> 8));
}

int main(void)
{
    ppu_reset();
    ppu_set_cgb_mode(1);
    ppu_vram_bank_write(0);
    ppu_vram_write(0x0000, 0xFF);
    ppu_vram_write(0x0001, 0x00);
    ppu_vram_write(0x1800, 0x00);
    ppu_vram_bank_write(1);
    ppu_vram_write(0x0000, 0x00);
    ppu_vram_write(0x0001, 0xFF);
    ppu_vram_write(0x1800, 0x08);
    uint8_t bank1_snapshot[0x1800];
    ppu_vram_bank1_copy(bank1_snapshot);
    assert(bank1_snapshot[0] == 0x00);
    assert(bank1_snapshot[1] == 0xFF);
    ppu_vram_bank_write(0);
    set_bg_color(1, 0x001F); /* red */
    set_bg_color(2, 0x03E0); /* green */
    set_obj_color(3, 1, 0x03E0); /* OBJ palette 3, color 1 = green */
    uint32_t obj_palette[32];
    ppu_cgb_obj_palette_copy(obj_palette);
    assert(obj_palette[3 * 4 + 1] == 0x00FF00u);
    ppu_write(0x40, 0x91);
    ppu_tick(252);
    assert(ppu_rgb[0][0] == 0x00FF00u);

    ppu_reset();
    ppu_set_cgb_mode(0);
    ppu_vram_bank_write(0);
    ppu_vram_write(0x0000, 0xFF);
    ppu_vram_write(0x0001, 0x00);
    ppu_vram_write(0x1800, 0x00);
    ppu_write(0x47, 0xE4);
    ppu_write(0x40, 0x91);
    ppu_tick(252);
    assert(ppu_shade[0][0] == 1);

    /* Homeworld renders the whole 512x256 source map into a widescreen
       overview while standard levels keep their normal margin geometry. */
    ppu_reset();
    ppu_set_cgb_mode(0);
    ppu_set_wide(49, 33, 0, 0, 0);
    ppu_write(0x43, 64);  /* SCX */
    ppu_write(0x4A, 136);  /* WY */
    ppu_write(0x4B, 80);   /* WX */
    ppu_write(0x40, 0xE3); /* Homeworld LCDC/window setup */
    ppu_tick(252);
    assert(ppu_w == 242);
    assert(ppu_xoff == 0);
    ppu_tick(11 * 456);
    assert(ppu_bgtile[11][0] == 257);
    assert(ppu_bgtile[11][241] == 258);
    ppu_tick((127 - 11) * 456);
    assert(ppu_bgtile[127][0] == 259);
    assert(ppu_bgtile[127][241] == 260);

    /* Returning to the normal SML2 window setup restores slider-controlled width. */
    ppu_write(0x4B, 7);
    ppu_tick(70224);
    assert(ppu_w == 242);
    assert(ppu_xoff == 49);
    return 0;
}
