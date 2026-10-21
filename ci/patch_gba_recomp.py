#!/usr/bin/env python3
"""Apply PipeClean's timing/audio fixes to the private GBA recompiler source tree."""
from pathlib import Path
import sys

if len(sys.argv) != 2:
    raise SystemExit("usage: patch_gba_recomp.py <gba-recomp-source-directory>")
root = Path(sys.argv[1])
for name in ("gba_platform.c", "gba_win.c", "gba_emit.py", "gba_overlay.py", "gba_run.c", "build_recomp.sh", "build_win.sh"):
    if not (root / name).is_file():
        raise SystemExit(f"Expected {name} in {root}")

# Keep the cleaned source's six-buffer waveOut queue. The Windows frontend uses
# audio buffer availability as the frame pacing clock; shrinking this queue can make
# a CPU-heavy recompiled game underrun and produce audible gaps/buzzing.
p = root / "gba_win.c"
s = p.read_text(encoding="utf-8")
if "#define AUDIO_BUFS 2" in s:
    s = s.replace("#define AUDIO_BUFS 2", "#define AUDIO_BUFS 6", 1)
elif "#define AUDIO_BUFS 4" in s:
    s = s.replace("#define AUDIO_BUFS 4", "#define AUDIO_BUFS 6", 1)
elif "#define AUDIO_BUFS 6" not in s:
    raise SystemExit("Unrecognized AUDIO_BUFS definition in gba_win.c")
p.write_text(s, encoding="utf-8", newline="\\n")

# Add WAV capture to the Windows frontend so buzzing can be separated from Windows playback.
# The recording is taken from the exact PCM buffer passed to waveOutWrite.
p = root / "gba_win.c"
s = p.read_text(encoding="utf-8")
if "static void rec_header(DWORD data_bytes)" not in s:
    config_header = " * Settings come from <rom>.cfg next to the ROM, "
    if config_header in s and "record <path>" not in s:
        line_end = s.find("\n", s.find(config_header)) + 1
        if line_end <= 0:
            raise SystemExit("Could not find Windows frontend config help line.")
        s = s[:line_end] + " *   record <path>                 write the exact PCM submitted to waveOut (audio diagnostics)\n" + s[line_end:]
    decl = "static BITMAPINFO bmi;\nstatic Cpu cpu;\n"
    rec_decl = """static BITMAPINFO bmi;
static Cpu cpu;
static FILE *rec = 0; static DWORD rec_bytes = 0; static char rec_path[MAX_PATH] = "";

static void rec_header(DWORD data_bytes) {
    unsigned char h[44]; memset(h, 0, sizeof h);
    memcpy(h, "RIFF", 4); DWORD riff = 36 + data_bytes; memcpy(h + 4, &riff, 4);
    memcpy(h + 8, "WAVEfmt ", 8); DWORD fmt = 16; memcpy(h + 16, &fmt, 4);
    WORD tag = 1, ch = 2, bits = 16, align = 4; DWORD rate = AUDIO_RATE, bps = AUDIO_RATE * 4;
    memcpy(h + 20, &tag, 2); memcpy(h + 22, &ch, 2); memcpy(h + 24, &rate, 4); memcpy(h + 28, &bps, 4);
    memcpy(h + 32, &align, 2); memcpy(h + 34, &bits, 2);
    memcpy(h + 36, "data", 4); memcpy(h + 40, &data_bytes, 4);
    fseek(rec, 0, SEEK_SET); fwrite(h, 1, 44, rec);
}
"""
    if decl not in s:
        raise SystemExit("Could not find Windows frontend globals for audio recording.")
    s = s.replace(decl, rec_decl, 1)

    audio_silence = "    else { n = AUDIO_RATE / 60; if (n > AUDIO_MAX_FRAMES) n = AUDIO_MAX_FRAMES; memset(abuf[slot], 0, (size_t)n * 4); }\n"
    audio_record = audio_silence + "    if (rec) { fwrite(abuf[slot], 1, (size_t)n * 4, rec); rec_bytes += (DWORD)n * 4; }\n"
    if audio_silence not in s:
        raise SystemExit("Could not find audio buffer fill point for waveOut recording.")
    s = s.replace(audio_silence, audio_record, 1)

    cfg_old = '            if (!strcmp(key, "overlays")) snprintf(overlays, sizeof overlays, "%s", val);\n            else if (!strcmp(key, "defer_irq_while")) defer = dval;'
    cfg_new = '            if (!strcmp(key, "overlays")) snprintf(overlays, sizeof overlays, "%s", val);\n            else if (!strcmp(key, "record")) snprintf(rec_path, sizeof rec_path, "%s", val);\n            else if (!strcmp(key, "defer_irq_while")) defer = dval;'
    if cfg_old not in s:
        raise SystemExit("Could not find Windows ROM-config parser for audio recording.")
    s = s.replace(cfg_old, cfg_new, 1)

    open_old = '    if (audio_open() != 0) { MessageBoxA(hwnd, "No audio device; running silent.", "Mario & Luigi", MB_ICONWARNING); hwo = 0; }'
    open_new = '    if (rec_path[0]) { rec = fopen(rec_path, "wb"); if (rec) rec_header(0); }\n' + open_old
    if open_old not in s:
        raise SystemExit("Could not find audio device startup point for recording.")
    s = s.replace(open_old, open_new, 1)

    close_old = "    audio_close();\n    return 0;"
    close_new = "    audio_close();\n    if (rec) { rec_header(rec_bytes); fclose(rec); rec = 0; }\n    return 0;"
    if close_old not in s:
        raise SystemExit("Could not find Windows frontend audio shutdown point for recording.")
    s = s.replace(close_old, close_new, 1)
p.write_text(s, encoding="utf-8", newline="\n")

# Compile the standalone frontend, PPU, and hardware runtime at the same optimization
# level as lifted game code; the original script hard-coded these three files to -O1.
p = root / "build_win.sh"
s = p.read_text(encoding="utf-8")
for source in ("gba_platform", "gba_ppu", "gba_win"):
    s = s.replace(f"gcc -O1 -w -c -I \"$HERE\" \"$HERE/{source}.c\"", f"gcc $CC_OPT -w -c -I \"$HERE\" \"$HERE/{source}.c\"")
p.write_text(s, encoding="utf-8", newline="\n")

# Retain the recompiler's 4096-cycle ceiling. Lowering this to 512 caused many
# more RT_TICK calls and repeated interrupt/timing checks in tight loops, hurting frame rate.
p = root / "gba_emit.py"
s = p.read_text(encoding="utf-8")
if ")), 512)" in s:
    s = s.replace(")), 512)", ")), 4096)", 1)
elif ")), 4096)" not in s:
    raise SystemExit("Unrecognized loop-backedge cycle cap in gba_emit.py")
p.write_text(s, encoding="utf-8", newline="\n")

# Carry forward the cleaned source's RAM-entry mapping for functions copied into EWRAM/IWRAM.
# Without this mapping, the runtime can locate the copied region but emitted entry aliases may
# jump to labels at the wrong ROM address, contributing to missing/corrupted overlay code.
p = root / "gba_overlay.py"
s = p.read_text(encoding="utf-8")
if "'ram_entries':" not in s:
    old = "        'entries': [[a, isa] for (a, isa) in sorted(entries)],\n        'unmapped_ram_targets':"
    new = "        'entries': [[a, isa] for (a, isa) in sorted(entries)],\n        'ram_entries': [[ram, rom] for (rom, isa), ram in sorted(entries.items()) if (ram >> 24) in (2, 3)],\n        'unmapped_ram_targets':"
    if old not in s:
        raise SystemExit("Unrecognized overlay JSON output block in gba_overlay.py")
    s = s.replace(old, new, 1)
p.write_text(s, encoding="utf-8", newline="\n")

p = root / "gba_emit.py"
s = p.read_text(encoding="utf-8")
if "RAM_MAP = {}" not in s:
    old = 'def fname(a): return f"f_{a:08X}"\n'
    if old not in s:
        raise SystemExit("Could not find function-name insertion point in gba_emit.py")
    s = s.replace(old, old + "RAM_MAP = {}   # IWRAM/EWRAM entry address -> ROM address its code was copied from (overlay copies)\n", 1)

if "for a, src in RAM_MAP.items()" not in s:
    old = "    def _collect_entries(s):\n        L = s.L\n"
    new = old + "        for a, src in RAM_MAP.items():\n            if src in L.ins:\n                o = L.owner[src]\n                s.alias[a] = o; s.extra[o].add(a); s.extra[o].add(src)\n"
    if old not in s:
        raise SystemExit("Could not find _collect_entries insertion point in gba_emit.py")
    s = s.replace(old, new, 1)

old_case = 'for e in extras: out.append(f"    case {hx(e)}: goto L_{e:08X};")'
new_case = 'for e in extras: out.append(f"    case {hx(e)}: goto L_{RAM_MAP.get(e, e):08X};")'
if new_case not in s:
    if old_case not in s:
        raise SystemExit("Could not find emitted alias label switch in gba_emit.py")
    s = s.replace(old_case, new_case, 1)

if "RAM_MAP[ram] = romaddr" not in s:
    old = "        roots = [(a, isa) for a, isa in ov['entries']]\n"
    new = old + "        for ram, romaddr in ov.get('ram_entries', []):\n            RAM_MAP[ram] = romaddr  # RAM entry (IWRAM/EWRAM) -> ROM code it was copied from\n"
    if old not in s:
        raise SystemExit("Could not find overlay roots in gba_emit.py")
    s = s.replace(old, new, 1)

if "EXTRA_ROOTS" not in s:
    old = "    else: regions = []\n    L = G.discover(rom, log=lambda *a: None, extra_roots=roots, overlay_regions=regions)\n"
    new = "    else: regions = []\n    roots += [(int(x, 0) & ~1, 'T') for x in os.environ.get('EXTRA_ROOTS', '').split(',') if x.strip()]\n    L = G.discover(rom, log=lambda *a: None, extra_roots=roots, overlay_regions=regions)\n"
    if old not in s:
        raise SystemExit("Could not find final discovery call in gba_emit.py")
    s = s.replace(old, new, 1)
p.write_text(s, encoding="utf-8", newline="\n")

# Keep long scripted profiling sequences from silently truncating after 32 input intervals.
p = root / "gba_run.c"
s = p.read_text(encoding="utf-8")
s = s.replace("keysched[32]", "keysched[8192]", 1)
s = s.replace("nkeys < 32", "nkeys < 8192", 1)
p.write_text(s, encoding="utf-8", newline="\n")

# Strip old per-event timing wrappers that were useful while profiling but added
# counters and conditional stderr output to the real-time runtime. This is safe on
# already-cleaned versions of the source archive as each replacement is conditional.
p = root / "gba_platform.c"
s = p.read_text(encoding="utf-8")
old = """static uint64_t dbg_vb_raise, dbg_vb_merged;
static uint64_t dbg_swi_cyc, dbg_halt_cyc, dbg_dma_cyc;
static void raise_irq(uint32_t bits) { if (bits & 1) { dbg_vb_raise++; if (IO16(IF_) & 1) dbg_vb_merged++; } IO16(IF_) |= bits; }"""
s = s.replace(old, "static void raise_irq(uint32_t bits) { IO16(IF_) |= bits; }", 1)

old = """static void dma_run_body(int n);
static void dma_run(int n) {
    uint64_t t0 = g_cyc; dma_run_body(n); dbg_dma_cyc += g_cyc - t0;
    if (g_cyc - t0 > 100000) fprintf(stderr, "[slow-dma] ch%d %llu cyc frame=%llu ctl=%04x cnt=%u\\n", n, (unsigned long long)(g_cyc - t0), (unsigned long long)plat_frames, dma[n].ctl, dma[n].icnt);
}
static void dma_run_body(int n) {"""
s = s.replace(old, "static void dma_run(int n) {", 1)

s = s.replace("static uint64_t dbg_irq_ime, dbg_irq_i, dbg_irq_depth;\n", "", 1)
old = """    if (IO16(IE) & IO16(IF_)) {                                                    /* diagnostic: why is a pending IRQ not taken? */
        if (!(IO16(IME) & 1)) dbg_irq_ime++; else if (c->psr_rest & 0x80) dbg_irq_i++; else if (irq_depth > 3) dbg_irq_depth++;
    }
"""
s = s.replace(old, "", 1)

old = """    { static uint64_t last_tick = 0; static uint32_t last_site = 0; static int reports = 0; static uint64_t gap_hist[8];   /* diagnostic: gaps between back-edge ticks */
      uint64_t gap = g_cyc - last_tick;
      static uint64_t p_swi, p_halt, p_dma; if (gap > 150000 && reports < 2000) { reports++; fprintf(stderr, "[gap] %llu cyc frame=%llu from=%08x to=%08x swi=%llu halt=%llu dma=%llu\\n", (unsigned long long)gap, (unsigned long long)plat_frames, last_site, site, (unsigned long long)(dbg_swi_cyc-p_swi), (unsigned long long)(dbg_halt_cyc-p_halt), (unsigned long long)(dbg_dma_cyc-p_dma)); } p_swi = dbg_swi_cyc; p_halt = dbg_halt_cyc; p_dma = dbg_dma_cyc;
      last_tick = g_cyc; last_site = site; (void)gap_hist; }
"""
s = s.replace(old, "", 1)
old = """    fprintf(stderr, "[irq-block] ime=%llu i-bit=%llu depth=%llu vblank_raised=%llu merged_while_pending=%llu DISPSTAT=%04x\\n", (unsigned long long)dbg_irq_ime, (unsigned long long)dbg_irq_i, (unsigned long long)dbg_irq_depth, (unsigned long long)dbg_vb_raise, (unsigned long long)dbg_vb_merged, IO16(DISPSTAT));
"""
s = s.replace(old, "", 1)

s = s.replace("    uint64_t start = plat_frames, c0 = g_cyc; uint64_t irq0 = plat_stats.irqs;\n", "    uint64_t start = plat_frames;\n", 1)
s = s.replace("    uint64_t waited = g_cyc - c0; dbg_halt_cyc += waited;\n", "", 1)
old = """    { static int rep = 0; if (waited > 1000000 && rep < 10) { rep++; fprintf(stderr, "[halt] waited %llu cyc frame=%llu psr=%08x IME=%u IE=%04x IF=%04x delivered=%llu\\n", (unsigned long long)waited, (unsigned long long)plat_frames, c->psr_rest, IO16(IME)&1, IO16(IE), IO16(IF_), (unsigned long long)(plat_stats.irqs - irq0)); } }
"""
s = s.replace(old, "", 1)

old = """void rt_swi_body(Cpu *c, uint32_t n);
void rt_swi(Cpu *c, uint32_t n) {
    uint64_t t0 = g_cyc; rt_swi_body(c, n); dbg_swi_cyc += g_cyc - t0;
    if (g_cyc - t0 > 100000) fprintf(stderr, "[slow-swi] %02x %llu cyc frame=%llu\\n", n, (unsigned long long)(g_cyc - t0), (unsigned long long)plat_frames);
}
void rt_swi_body(Cpu *c, uint32_t n) {"""
s = s.replace(old, "void rt_swi(Cpu *c, uint32_t n) {", 1)
p.write_text(s, encoding="utf-8", newline="\n")

print("Applied PipeClean fixes: stable audio queue, RAM overlay entry mapping, extra roots, runtime optimization, debug cleanup, WAV capture.")
