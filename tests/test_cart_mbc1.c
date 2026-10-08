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
    img[0x147] = 0x01; /* MBC1, no RAM */
    img[0x148] = 0x05; /* 1 MiB */

    for (size_t b = 0; b < banks; b++)
        img[b * 0x4000] = (uint8_t)b;

    if (cart_install(img, rom_size) != 0) {
        free(img);
        return fail(3, "cart_install rejected a valid MBC1 image");
    }

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

    free(img);
    puts("MBC1 mapper: PASS");
    return 0;
}
