#include "gb.h"
#include <assert.h>

uint8_t io_if, io_ie;
void frame_hook(void) {}

static void set_bg_color(unsigned index, unsigned rgb555)
{
    ppu_cgb_write(0x68, (uint8_t)(0x80 | (index * 2)));
    ppu_cgb_write(0x69, (uint8_t)rgb555);
    ppu_cgb_write(0x69, (uint8_t)(rgb555 >> 8));
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
    ppu_vram_bank_write(0);
    set_bg_color(1, 0x001F); /* red */
    set_bg_color(2, 0x03E0); /* green */
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
    return 0;
}
