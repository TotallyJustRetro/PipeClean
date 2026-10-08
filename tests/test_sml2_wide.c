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
    rom[0x8040] = 0x19;
    rom[0x8064] = 0x19;
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

    /* The wider loader also uses the +0x70 and +0xA0 horizontal bounds.
     * Each hook adds the same configured right-side extension (+0x21 here). */
    cpu.pc = 0x4040;
    cpu.h = 1;
    cpu.l = 0;
    cpu.f = 0;
    total_cycles = 0;
    if (!wide_intercept_sml2(0x19)) return 8;
    if (HL() != 0x0191 || cpu.pc != 0x4041 || total_cycles != 8) return 9;

    cpu.pc = 0x4064;
    cpu.h = 1;
    cpu.l = 0;
    cpu.f = 0;
    total_cycles = 0;
    if (!wide_intercept_sml2(0x19)) return 10;
    if (HL() != 0x01C1 || cpu.pc != 0x4065 || total_cycles != 8) return 11;

    cpu.pc = 0x401D;
    cpu.h = 1;
    cpu.l = 0;
    if (wide_intercept_sml2(0x19)) return 12;
    if (HL() != 0x0100) return 13;

    /* The same CPU opcode must not be intercepted while another bank is mapped. */
    cart_hi = rom + 0xC000;
    cpu.pc = 0x401C;
    cpu.h = 1;
    cpu.l = 0;
    cpu.d = 0;
    cpu.e = 0x60;
    total_cycles = 0;
    if (wide_intercept_sml2(0x19)) return 14;
    if (HL() != 0x0100 || total_cycles != 0) return 15;

    free(rom);
    puts("SML2 widescreen hook: PASS");
    return 0;
}
