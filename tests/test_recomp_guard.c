#include "recomp_guard.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    assert(argc == 2);
    FILE *f = fopen(argv[1], "rb");
    assert(f != NULL);
    assert(fseek(f, 0, SEEK_END) == 0);
    long size = ftell(f);
    assert(size >= 0x150);
    assert(fseek(f, 0, SEEK_SET) == 0);
    uint8_t *rom = (uint8_t *)malloc((size_t)size);
    assert(rom != NULL);
    assert(fread(rom, 1, (size_t)size, f) == (size_t)size);
    fclose(f);

    /* The exact ROM used by recomp.py must be accepted. */
    assert(recomp_rom_matches_build(rom, (size_t)size));

    /* Same title, one changed byte: never execute the stale native code. */
    uint8_t *changed = (uint8_t *)malloc((size_t)size);
    assert(changed != NULL);
    memcpy(changed, rom, (size_t)size);
    changed[0x150] ^= 0x01;
    assert(!recomp_rom_matches_build(changed, (size_t)size));

    /* A different title is also rejected. */
    changed[0x150] = rom[0x150];
    changed[0x134] ^= 0x01;
    assert(!recomp_rom_matches_build(changed, (size_t)size));

    free(changed);
    free(rom);
    return 0;
}
