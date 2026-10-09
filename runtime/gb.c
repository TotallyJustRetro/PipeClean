/* Memory map, timer, serial, joypad, DMA, interrupts. */
#include "sm83.h"
#include "cart.h"
#include "widescreen.h"

CPU cpu;
uint8_t io_if, io_ie;
uint64_t total_cycles;

static uint8_t wram[0x8000];
static uint8_t cgb_svbk = 1;
static uint8_t cgb_key1;
static uint8_t cgb_hdma[5];
static int cgb_mode;
static uint8_t hram[0x80];
static uint8_t io_misc[0x80];

/* timer */
static uint16_t div_counter;
static uint8_t tima, tma, tac;
/* serial */
static uint8_t sb, sc;
static int serial_cycles;
static void (*serial_cb)(uint8_t);
/* joypad */
static uint8_t joy_sel = 0x30, joy_buttons, joy_dpad;

volatile int gb_mp_vblank_watch;
volatile uint16_t gb_mp_vblank_return_pc;
static volatile int gb_mp_vblank_pending;

void gb_mp_vblank_arm(void)
{
    gb_mp_vblank_pending = 1;
    gb_mp_vblank_watch = 0;
}

void gb_serial_hook(void (*fn)(uint8_t)) { serial_cb = fn; }

/* ------------------------------------------------------------------ */
static uint8_t joyp_read(void)
{
    uint8_t lines = 0x0F;
    if (!(joy_sel & 0x20)) lines &= (uint8_t)~joy_buttons;
    if (!(joy_sel & 0x10)) lines &= (uint8_t)~joy_dpad;
    return 0xC0 | joy_sel | (lines & 0x0F);
}

void gb_set_input(uint8_t buttons, uint8_t dpad)
{
    uint8_t before = joyp_read() & 0x0F;
    joy_buttons = buttons & 0x0F;
    joy_dpad = dpad & 0x0F;
    uint8_t after = joyp_read() & 0x0F;
    if (before & ~after) io_if |= 0x10;
}

static uint8_t io_read(uint8_t r)
{
    switch (r) {
    case 0x00: return joyp_read();
    case 0x01: return sb;
    case 0x02: return sc | 0x7E;
    case 0x04: return div_counter >> 8;
    case 0x05: return tima;
    case 0x06: return tma;
    case 0x07: return tac | 0xF8;
    case 0x0F: return io_if | 0xE0;
    case 0x4D: return cgb_mode ? (uint8_t)(0x7E | cgb_key1) : 0xFF;
    case 0x4F: return cgb_mode ? ppu_vram_bank_read() : 0xFF;
    case 0x51: case 0x52: case 0x53: case 0x54: return cgb_mode ? cgb_hdma[r - 0x51] : 0xFF;
    case 0x55: return cgb_mode ? cgb_hdma[4] : 0xFF;
    case 0x70: return cgb_mode ? (uint8_t)(0xF8 | cgb_svbk) : 0xFF;
    default: break;
    }
    if (r >= 0x10 && r <= 0x3F) return apu_read(0xFF00 | r);
    if (r >= 0x40 && r <= 0x4B) return ppu_read(r);
    if (r >= 0x68 && r <= 0x6C) return cgb_mode ? ppu_cgb_read(r) : 0xFF;
    return 0xFF;
}

static void dma_start(uint8_t v)
{
    uint16_t src = (uint16_t)(v << 8);
    for (int i = 0; i < 0xA0; i++) oam[i] = rd8((uint16_t)(src + i));
}

static void timer_div_reset(void);

static size_t wram_offset(uint16_t a)
{
    if (a >= 0xE000) a = (uint16_t)(a - 0x2000);
    if (a < 0xD000) return (size_t)(a - 0xC000);
    return 0x1000u + (size_t)((cgb_mode ? cgb_svbk : 1u) - 1u) * 0x1000u + (size_t)(a - 0xD000);
}

static void io_write(uint8_t r, uint8_t v)
{
    switch (r) {
    case 0x00: joy_sel = v & 0x30; return;
    case 0x01: sb = v; return;
    case 0x02:
        sc = v;
        if ((v & 0x81) == 0x81) {
            if (serial_cb) serial_cb(sb);
            serial_cycles = 4096;       /* 8 bits at 8192 Hz */
        }
        return;
    case 0x04: timer_div_reset(); return;
    case 0x05: tima = v; return;
    case 0x06: tma = v; return;
    case 0x07: tac = v & 7; return;
    case 0x0F: io_if = v & 0x1F; return;
    case 0x4D: if (cgb_mode) cgb_key1 = (uint8_t)((cgb_key1 & 0x80) | (v & 1)); return;
    case 0x4F: if (cgb_mode) ppu_vram_bank_write(v); return;
    case 0x51: case 0x52: case 0x53: case 0x54:
        if (cgb_mode) cgb_hdma[r - 0x51] = (r == 0x52 || r == 0x54) ? (v & 0xF0) : (r == 0x53 ? (v & 0x1F) : v);
        return;
    case 0x55:
        if (cgb_mode) {
            uint16_t src = (uint16_t)(((uint16_t)cgb_hdma[0] << 8) | cgb_hdma[1]);
            uint16_t dst = (uint16_t)(0x8000 | (((uint16_t)cgb_hdma[2] << 8 | cgb_hdma[3]) & 0x1FF0));
            unsigned blocks = (unsigned)(v & 0x7F) + 1u;
            /* Initial compatibility path: perform both GDMA and HDMA requests
             * immediately. Full HBlank scheduling is added with PPU mode hooks. */
            for (unsigned i = 0; i < blocks * 16u; i++) {
                uint8_t data = rd8((uint16_t)(src + i));
                ppu_vram_write((uint16_t)((dst + i) & 0x1FFF), data);
            }
            cgb_hdma[0] = (uint8_t)((src + blocks * 16u) >> 8);
            cgb_hdma[1] = (uint8_t)((src + blocks * 16u) & 0xF0);
            uint16_t end = (uint16_t)(dst + blocks * 16u);
            cgb_hdma[2] = (uint8_t)((end >> 8) & 0x1F);
            cgb_hdma[3] = (uint8_t)(end & 0xF0);
            cgb_hdma[4] = 0xFF;
        }
        return;
    case 0x70: if (cgb_mode) { cgb_svbk = v & 7; if (!cgb_svbk) cgb_svbk = 1; } return;
    default: break;
    }
    if (r >= 0x10 && r <= 0x3F) { apu_write(0xFF00 | r, v); return; }
    if (r == 0x46) { io_misc[r] = v; dma_start(v); return; }
    if (r >= 0x40 && r <= 0x4B) { ppu_write(r, v); return; }
    if (r >= 0x68 && r <= 0x6C) { if (cgb_mode) ppu_cgb_write(r, v); return; }
    io_misc[r] = v;
}

/* ------------------------------------------------------------------ */
uint8_t rd8(uint16_t a)
{
    switch (a >> 12) {
    case 0: case 1: case 2: case 3:
    case 4: case 5: case 6: case 7: return a < 0x4000 ? cart_lo[a] : cart_hi[a - 0x4000];
    case 8: case 9: return ppu_vram_read(a & 0x1FFF);
    case 0xA: case 0xB: { uint8_t v = cart_ram_read(a); return wide_read_sml2(a, v); }
    case 0xC: case 0xD: case 0xE: return wram[wram_offset(a)];
    default:
        if (a < 0xFE00) return wram[wram_offset(a)];
        if (a < 0xFEA0) return oam[a - 0xFE00];
        if (a < 0xFF00) return 0xFF;
        if (a < 0xFF80) return io_read((uint8_t)(a & 0x7F));
        if (a == 0xFFFF) return io_ie;
        return hram[a & 0x7F];
    }
}

/* memory-write watchers (game event detection) */
#define MAX_WATCH 16
static uint16_t watch_lo[MAX_WATCH], watch_hi[MAX_WATCH];
static int watch_n;
static void (*watch_cb)(uint16_t, uint8_t, uint8_t);

void gb_watch_set(const uint16_t *addrs, int n, void (*cb)(uint16_t addr, uint8_t old_v, uint8_t new_v))
{
    watch_n = n > MAX_WATCH ? MAX_WATCH : n;
    for (int i = 0; i < watch_n; i++) watch_lo[i] = watch_hi[i] = addrs[i];
    watch_cb = cb;
    if (!cb) watch_n = 0;
}

void gb_watch_range(uint16_t lo, uint16_t hi, void (*cb)(uint16_t addr, uint8_t old_v, uint8_t new_v))
{
    watch_lo[0] = lo; watch_hi[0] = hi; watch_n = cb ? 1 : 0; watch_cb = cb;
}

static void watch_check(uint16_t a, uint8_t v)
{
    for (int i = 0; i < watch_n; i++)
        if (a >= watch_lo[i] && a <= watch_hi[i]) { watch_cb(a, rd8(a), v); return; }
}

void wr8(uint16_t a, uint8_t v)
{
    if (UNLIKELY(watch_n)) watch_check(a, v);
    switch (a >> 12) {
    case 0: case 1: case 2: case 3:
    case 4: case 5: case 6: case 7: cart_write_ctrl(a, v); return;     /* mapper registers */
    case 8: case 9: ppu_vram_write(a & 0x1FFF, v); return;
    case 0xA: case 0xB: cart_ram_write(a, v); return;
    case 0xC: case 0xD: case 0xE: wram[wram_offset(a)] = v; return;
    default:
        if (a < 0xFE00) { wram[wram_offset(a)] = v; return; }
        if (a < 0xFEA0) { oam[a - 0xFE00] = v; return; }
        if (a < 0xFF00) return;
        if (a < 0xFF80) { io_write((uint8_t)(a & 0x7F), v); return; }
        if (a == 0xFFFF) { io_ie = v; return; }
        hram[a & 0x7F] = v;
    }
}

/* ------------------------------------------------------------------ */
static inline unsigned timer_mask(void)
{
    static const unsigned m[4] = {1u << 9, 1u << 3, 1u << 5, 1u << 7};
    return m[tac & 3];
}

static void tima_inc(void)
{
    if (++tima == 0) { tima = tma; io_if |= 0x04; }
}

static void timer_div_reset(void)
{
    if ((tac & 4) && (div_counter & timer_mask())) tima_inc();
    div_counter = 0;
}

/* STOP switches CPU clock speed only when CGB KEY1 has been prepared. */
void gb_stop(void)
{
    if (!cgb_mode || !(cgb_key1 & 0x01)) return;
    div_counter = 0;
    cgb_key1 = (uint8_t)((cgb_key1 ^ 0x80) & 0x80);
}

static void timer_tick(int n)
{
    for (int i = 0; i < n; i += 4) {
        uint16_t old = div_counter;
        div_counter += 4;
        if ((tac & 4) && (old & timer_mask()) && !(div_counter & timer_mask())) tima_inc();
    }
}

void hw_tick(int n)
{
    total_cycles += (uint64_t)n;
    /* Timer/CPU clocks double in CGB double-speed mode; LCD and APU stay at
     * the normal base clock. CPU instruction timing remains expressed in CPU
     * T-cycles, so only the slower peripherals receive half as many cycles. */
    timer_tick(n);
    int base_cycles = (cgb_mode && (cgb_key1 & 0x80)) ? n / 2 : n;
    ppu_tick(base_cycles);
    apu_tick(base_cycles);
    if (serial_cycles > 0 && (serial_cycles -= n) <= 0) {
        serial_cycles = 0;
        sc &= 0x7F;
        sb = 0xFF;
        io_if |= 0x08;
    }
}

/* ------------------------------------------------------------------ */
void cpu_halt(void)
{
    while (!(io_if & io_ie & 0x1F)) hw_tick(4);
}

void cpu_service_irq(void)
{
    uint8_t pend = io_if & io_ie & 0x1F;
    uint16_t return_pc = cpu.pc;
    cpu.ime = 0;
    cpu.ei_pending = 0;
    cpu.sp--; wr8(cpu.sp, cpu.pc >> 8);
    cpu.sp--; wr8(cpu.sp, (uint8_t)cpu.pc);
    for (int i = 0; i < 5; i++) {
        if (pend & (1 << i)) {
            io_if &= (uint8_t)~(1 << i);
            if (i == 0 && gb_mp_vblank_pending) {
                gb_mp_vblank_pending = 0;
                gb_mp_vblank_return_pc = return_pc;
                gb_mp_vblank_watch = 1;
            }
            cpu.pc = (uint16_t)(0x40 + i * 8);
            break;
        }
    }
    hw_tick(20);
}

void cpu_lockup(uint8_t op, uint16_t pc)
{
    fprintf(stderr, "CPU locked up: illegal opcode %02X at %04X\n", op, pc);
    fflush(stdout);
    exit(2);
}

void cpu_step_checked(void)
{
    uint16_t pc_before = cpu.pc;
    uint8_t opcode = rd8(cpu.pc);
    cpu_step();
    (void)pc_before;
    (void)opcode;
    if (gb_mp_vblank_watch && cpu.pc == gb_mp_vblank_return_pc)
        gb_mp_vblank_done();
    if (cpu_irq_check()) cpu_service_irq();
}

void run_interpreter(void)
{
    for (;;) cpu_step_checked();
}

/* ---- coverage of ROM addresses that were executed but not recompiled ---- */
static uint8_t miss_seen[0x8000];
static unsigned long miss_ram_steps;

void recomp_miss(uint16_t pc)
{
    if (pc < 0x8000) miss_seen[pc] = 1;
    else miss_ram_steps++;
}

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t ppu_n, apu_n, cart_n, cart_ram_n;
    CPU cpu;
    uint8_t io_if, io_ie;
    uint64_t total_cycles;
    uint8_t wram[0x8000], hram[0x80], io_misc[0x80], cart_state[64];
    uint8_t cgb_svbk, cgb_key1;
    uint16_t div_counter;
    uint8_t tima, tma, tac;
    uint8_t sb, sc;
    int serial_cycles;
    uint8_t joy_sel, joy_buttons, joy_dpad;
} CoreState;

#define GB_STATE_MAGIC 0x50434D50u
#define GB_STATE_VERSION 3u

size_t gb_state_size(void) { return GB_STATE_BYTES; }
size_t gb_state_data_size(void)
{
    return sizeof(CoreState) + ppu_state_size() + apu_state_size() + cart_ram_state_size();
}

int gb_state_save(void *dst, size_t n)
{
    if (!dst || n < gb_state_data_size()) return -1;
    uint8_t *p = (uint8_t *)dst;
    CoreState s;
    memset(&s, 0, sizeof s);
    s.magic = GB_STATE_MAGIC; s.version = GB_STATE_VERSION;
    s.ppu_n = (uint32_t)ppu_state_size();
    s.apu_n = (uint32_t)apu_state_size();
    s.cart_n = (uint32_t)cart_state_size();
    s.cart_ram_n = (uint32_t)cart_ram_state_size();
    s.cpu = cpu; s.io_if = io_if; s.io_ie = io_ie; s.total_cycles = total_cycles;
    memcpy(s.wram,wram,sizeof wram); memcpy(s.hram,hram,sizeof hram); memcpy(s.io_misc,io_misc,sizeof io_misc);
    s.div_counter=div_counter; s.tima=tima; s.tma=tma; s.tac=tac; s.sb=sb; s.sc=sc; s.serial_cycles=serial_cycles;
    s.joy_sel=joy_sel; s.joy_buttons=joy_buttons; s.joy_dpad=joy_dpad;
    s.cgb_svbk=cgb_svbk; s.cgb_key1=cgb_key1;
    size_t off = sizeof s;
    if (s.cart_n > sizeof s.cart_state || s.cart_ram_n != cart_ram_state_size() ||
        gb_state_data_size() > n || off + s.ppu_n + s.apu_n + s.cart_ram_n > n) return -1;
    if (cart_state_save(s.cart_state, s.cart_n)) return -1;
    memcpy(p, &s, sizeof s);
    if (ppu_state_save(p + sizeof s, s.ppu_n)) return -1;
    if (apu_state_save(p + sizeof s + s.ppu_n, s.apu_n)) return -1;
    if (cart_ram_state_save(p + sizeof s + s.ppu_n + s.apu_n, s.cart_ram_n)) return -1;
    return 0;
}

int gb_state_load(const void *src, size_t n)
{
    if (!src || n < sizeof(CoreState)) return -1;
    const uint8_t *p = (const uint8_t *)src;
    CoreState s;
    memcpy(&s,p,sizeof s);
    if (s.magic != GB_STATE_MAGIC || s.version != GB_STATE_VERSION) return -1;
    size_t off = sizeof s;
    if (s.ppu_n != ppu_state_size() || s.apu_n != apu_state_size() ||
        s.cart_n != cart_state_size() || s.cart_n > sizeof s.cart_state ||
        s.cart_ram_n != cart_ram_state_size() ||
        off + s.ppu_n + s.apu_n + s.cart_ram_n > n) return -1;
    cpu=s.cpu; io_if=s.io_if; io_ie=s.io_ie; total_cycles=s.total_cycles;
    memcpy(wram,s.wram,sizeof wram); memcpy(hram,s.hram,sizeof hram); memcpy(io_misc,s.io_misc,sizeof io_misc);
    div_counter=s.div_counter; tima=s.tima; tma=s.tma; tac=s.tac; sb=s.sb; sc=s.sc; serial_cycles=s.serial_cycles;
    joy_sel=s.joy_sel; joy_buttons=s.joy_buttons; joy_dpad=s.joy_dpad;
    cgb_svbk=s.cgb_svbk ? (s.cgb_svbk & 7) : 1; cgb_key1=s.cgb_key1;
    if (ppu_state_load(p + off, s.ppu_n)) return -1;
    off += s.ppu_n;
    if (apu_state_load(p + off, s.apu_n)) return -1;
    off += s.apu_n;
    if (cart_state_load(s.cart_state, s.cart_n)) return -1;
    if (cart_ram_state_load(p + off, s.cart_ram_n)) return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
void gb_dump_misses(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) return;
    int n = 0;
    for (int i = 0; i < 0x8000; i++)
        if (miss_seen[i]) { fprintf(f, "%04X\n", i); n++; }
    fclose(f);
    fprintf(stderr, "[recomp] %d ROM addresses ran in the interpreter (wrote %s); %lu RAM/HRAM steps\n",
            n, path, miss_ram_steps);
}

/* ------------------------------------------------------------------ */
void gb_reset(void)
{
    memset(&cpu, 0, sizeof cpu);
    cart_reset();
    /* Model the CPU state left by the boot ROM before entry at 0100.
     * CGB-only cartridges need the Color boot state; the DMG values make
     * some of them deliberately stop with a "Game Boy Color only" message. */
    const CartInfo *ci = cart_info();
    cgb_mode = ci && (ci->cgb_flag & 0x80) != 0;
    cgb_svbk = 1; cgb_key1 = 0; memset(cgb_hdma, 0xFF, sizeof cgb_hdma);
    if (ci && (ci->cgb_flag & 0x80) != 0) {
        cpu.a = 0x11; cpu.f = 0x80; cpu.b = 0x00; cpu.c = 0x00;
        cpu.d = 0xFF; cpu.e = 0x56; cpu.h = 0x00; cpu.l = 0x0D;
        div_counter = 0x1EA0;
    } else {
        cpu.a = 0x01; cpu.f = 0xB0; cpu.b = 0x00; cpu.c = 0x13;
        cpu.d = 0x00; cpu.e = 0xD8; cpu.h = 0x01; cpu.l = 0x4D;
        div_counter = 0xABCC;
    }
    cpu.sp = 0xFFFE; cpu.pc = 0x0100;
    io_if = 0x01; io_ie = 0;
    tima = tma = tac = 0;
    sb = 0; sc = 0x7E; serial_cycles = 0;
    joy_sel = 0x30; joy_buttons = joy_dpad = 0;
    gb_mp_vblank_watch = 0;
    gb_mp_vblank_pending = 0;
    gb_mp_vblank_return_pc = 0;
    memset(wram, 0, sizeof wram);
    memset(hram, 0, sizeof hram);
    memset(io_misc, 0xFF, sizeof io_misc);
    total_cycles = 0;
    ppu_reset();
    ppu_set_cgb_mode(cgb_mode);
    apu_reset();
}