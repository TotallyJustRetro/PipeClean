#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cart.h"

static int fail(int code, const char *msg)
{
    fprintf(stderr, "MBC1 test failed: %s\n", msg);
    return code;
}

int main(void)
{
    const size_t banks = 64;
    const size_t rom_size = banks * 0x4000;
    uint8_t *img = (uint8_t *)calloc(rom_size, 1);
    if (!img) return fail(2, "out of memory");

    memcpy(img + 0x134, "MBC1 TEST", 9);
    img[0x143] = 0xC0; /* CGB-only header flag; metadata must survive parsing */
    img[0x147] = 0x01; /* MBC1, no RAM */
    img[0x148] = 0x05; /* 1 MiB */
    img[0x149] = 0x02; /* 8 KiB external SRAM */

    for (size_t b = 0; b < banks; b++)
        img[b * 0x4000] = (uint8_t)b;

    if (cart_install(img, rom_size) != 0) {
        free(img);
        return fail(3, "cart_install rejected a valid MBC1 image");
    }

    const CartInfo *ci = cart_info();
    if (!ci || ci->cgb_flag != 0xC0)
        return fail(15, "CGB-only cartridge header flag was not detected");

    if (cart_hi[0] != 1)
        return fail(4, "reset did not map bank 1");

    cart_write_ctrl(0x2000, 7);
    if (cart_hi[0] != 7)
        return fail(5, "lower bank register did not select bank 7");

    cart_write_ctrl(0x2000, 0);
    if (cart_hi[0] != 1)
        return fail(6, "bank 0 alias did not map to bank 1");

    cart_write_ctrl(0x2000, 1);
    cart_write_ctrl(0x4000, 1);
    if (cart_hi[0] != 33)
        return fail(7, "upper bank register did not extend the switchable bank");

    cart_write_ctrl(0x6000, 1); /* MBC1 RAM-bank mode */
    if (cart_lo[0] != 32 || cart_hi[0] != 33)
        return fail(8, "MBC1 mode 1 did not remap the lower/upper banks");

    cart_write_ctrl(0x6000, 0);
    if (cart_lo[0] != 0 || cart_hi[0] != 33)
        return fail(9, "MBC1 mode 0 did not restore bank 0 at the low window");

    if (cart_ram_state_size() != 0x2000)
        return fail(10, "expected 8 KiB cartridge RAM state");

    cart_write_ctrl(0x0000, 0x0A); /* enable external RAM */
    cart_ram_write(0xA000, 0x12);
    cart_ram_write(0xA123, 0x34);
    cart_ram_write(0xBFFF, 0x56);
    uint8_t *ram_state = (uint8_t *)malloc(cart_ram_state_size());
    if (!ram_state) return fail(11, "out of memory for RAM state");
    if (cart_ram_state_save(ram_state, cart_ram_state_size()) != 0)
        return fail(12, "cartridge RAM state save failed");

    cart_ram_write(0xA000, 0x99);
    cart_ram_write(0xA123, 0x88);
    cart_ram_write(0xBFFF, 0x77);
    if (cart_ram_state_load(ram_state, cart_ram_state_size()) != 0) {
        free(ram_state);
        return fail(13, "cartridge RAM state load failed");
    }
    free(ram_state);
    if (cart_ram_read(0xA000) != 0x12 || cart_ram_read(0xA123) != 0x34 || cart_ram_read(0xBFFF) != 0x56)
        return fail(14, "cartridge RAM contents were not restored");

    free(img);
    puts("MBC1 mapper and CGB header detection: PASS");
    return 0;
}
