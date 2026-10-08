#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "gb.h"
#include "sm83.h"
#include "widescreen.h"
#include "cart.h"

CPU cpu;
uint8_t *rom;
const uint8_t *cart_lo, *cart_hi;
uint64_t total_cycles;

void hw_tick(int tcycles)
{
    total_cycles += (uint64_t)tcycles;
}

int main(void)
{
    int l, r;
    rom = (uint8_t *)calloc(0x10000, 1);
    if (!rom) return 2;

    rom[0x801C] = 0x19;
    cart_hi = rom + 0x8000;

    wide_dims(GAME_SML2, 100, &l, &r);
    if (l != 49 || r != 33) return 3;
    if (!wide_install(GAME_SML2, l, r)) return 4;

    cpu.pc = 0x401C;
    cpu.h = 1;
    cpu.l = 0;
    cpu.d = 0;
    cpu.e = 0x60;
    cpu.f = 0;
    total_cycles = 0;

    if (!wide_intercept_sml2(0x19)) return 5;
    if (HL() != 0x0181) return 6;
    if (cpu.pc != 0x401D || total_cycles != 8) return 7;

    cpu.pc = 0x401D;
    cpu.h = 1;
    cpu.l = 0;
    if (wide_intercept_sml2(0x19)) return 8;
    if (HL() != 0x0100) return 9;

    free(rom);
    puts("SML2 widescreen hook: PASS");
    return 0;
}
