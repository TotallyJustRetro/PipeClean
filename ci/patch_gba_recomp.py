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

# Four queued waveOut buffers are a compromise: enough tolerance for brief scheduling jitter
# without the long queue latency of six buffers. The output must still sustain real-time emulation.
p = root / "gba_win.c"
s = p.read_text(encoding="utf-8")
if "#define AUDIO_BUFS 6" in s:
    s = s.replace("#define AUDIO_BUFS 6", "#define AUDIO_BUFS 4", 1)
elif "#define AUDIO_BUFS 2" in s:
    s = s.replace("#define AUDIO_BUFS 2", "#define AUDIO_BUFS 4", 1)
elif "#define AUDIO_BUFS 4" not in s:
    raise SystemExit("Unrecognized AUDIO_BUFS definition in gba_win.c")
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

# Add approximate legacy PSG synthesis (square 1/2, wave and noise) to the
# Direct Sound FIFO mixer. This remains an HLE approximation, not cycle-accurate audio.
p = root / "gba_platform.c"
s = p.read_text(encoding="utf-8")
marker = 'static double aud_rd = 0.0;              /* read position (absolute, in source frames) */\n'
psg = r'''
/* Approximate GBA legacy PSG. Direct Sound remains the primary soundtrack path,
 * but some menu and effects sounds use channels 1-4. No exact sweep or RAM contention. */
typedef struct {
    double phase;
    uint16_t lfsr;
    unsigned length;
    int enabled, env_volume, env_period, env_counter, env_up;
} PsgVoice;
static PsgVoice psg_voice[4];
static uint64_t psg_sample_count;
static const uint16_t psg_freq_reg[4] = {0x064, 0x06C, 0x074, 0x07C};

static void psg_trigger(int ch) {
    if (ch < 0 || ch >= 4) return;
    PsgVoice *v = &psg_voice[ch];
    memset(v, 0, sizeof *v);
    v->phase = 0.0;
    v->lfsr = 0x7FFF;
    v->enabled = (IO16(0x084) & 0x0080) != 0;
    if (ch == 2 && !(IO16(0x070) & 0x0080)) v->enabled = 0;
    if (ch == 0 || ch == 1) {
        uint16_t c = IO16(ch == 0 ? 0x062 : 0x068);
        v->length = 64u - (c & 0x3Fu);
        v->env_volume = (c >> 12) & 15;
        v->env_up = (c >> 11) & 1;
        v->env_period = (c >> 8) & 7;
    } else if (ch == 2) {
        v->length = 256u - (IO16(0x070) & 0xFFu);
        v->env_volume = 15;
    } else {
        uint16_t c = IO16(0x078);
        v->length = 64u - (c & 0x3Fu);
        v->env_volume = (c >> 12) & 15;
        v->env_up = (c >> 11) & 1;
        v->env_period = (c >> 8) & 7;
    }
    if (!v->length) v->length = ch == 2 ? 256u : 64u;
    v->env_counter = v->env_period ? v->env_period : 8;
    if (v->enabled) IO16(0x084) |= (uint16_t)(1u << ch);
    else IO16(0x084) &= (uint16_t)~(1u << ch);
}

static void psg_clock(void) {
    psg_sample_count++;
    if ((psg_sample_count & 127u) == 0) {
        for (int ch = 0; ch < 4; ch++) {
            PsgVoice *v = &psg_voice[ch];
            if (v->enabled && (IO16(psg_freq_reg[ch]) & 0x4000) && v->length) {
                if (--v->length == 0) {
                    v->enabled = 0;
                    IO16(0x084) &= (uint16_t)~(1u << ch);
                }
            }
        }
    }
    if ((psg_sample_count & 511u) == 0) {
        for (int ch = 0; ch < 4; ch++) {
            PsgVoice *v = &psg_voice[ch];
            if (!v->enabled || ch == 2 || !v->env_period) continue;
            if (--v->env_counter <= 0) {
                v->env_counter = v->env_period;
                if (v->env_up) { if (v->env_volume < 15) v->env_volume++; }
                else if (v->env_volume > 0) v->env_volume--;
            }
        }
    }
}

static int psg_sample(int ch) {
    PsgVoice *v = &psg_voice[ch];
    if (!v->enabled || !(IO16(0x084) & 0x0080)) return 0;
    uint16_t freq = IO16(psg_freq_reg[ch]) & 0x07FF;
    double denom = (double)(2048u - freq);
    if (denom < 1.0) denom = 1.0;
    if (ch == 0 || ch == 1) {
        static const double duty[4] = {0.125, 0.25, 0.50, 0.75};
        uint16_t dutyreg = IO16(ch == 0 ? 0x062 : 0x068);
        double hz = 131072.0 / denom;
        v->phase += hz / (double)AUD_HZ;
        v->phase -= floor(v->phase);
        return (v->phase < duty[(dutyreg >> 6) & 3] ? 1 : -1) * v->env_volume * 300;
    }
    if (ch == 2) {
        unsigned level = (IO16(0x072) >> 13) & 3u;
        if (!level) return 0;
        v->phase += (65536.0 / denom) / (double)AUD_HZ;
        v->phase -= floor(v->phase);
        unsigned index = (unsigned)(v->phase * 32.0) & 31u;
        uint8_t packed = io[0x090 + index / 2];
        int sample = ((index & 1u) ? (packed & 15) : (packed >> 4)) - 8;
        if (level == 2) sample /= 2;
        else if (level == 3) sample /= 4;
        return sample * 450;
    }
    {
        static const double divisor[8] = {8.0, 16.0, 32.0, 48.0, 64.0, 80.0, 96.0, 112.0};
        uint16_t noise = IO16(0x07C);
        unsigned shift = (noise >> 4) & 15u;
        double hz = 524288.0 / divisor[noise & 7u] / (double)(1u << (shift > 12 ? 12 : shift));
        v->phase += hz / (double)AUD_HZ;
        while (v->phase >= 1.0) {
            v->phase -= 1.0;
            unsigned bit = (v->lfsr ^ (v->lfsr >> 1)) & 1u;
            v->lfsr = (uint16_t)((v->lfsr >> 1) | (bit << 14));
            if (noise & 8u) v->lfsr = (uint16_t)((v->lfsr & ~(1u << 6)) | (bit << 6));
        }
        return ((v->lfsr & 1u) ? -1 : 1) * v->env_volume * 300;
    }
}
'''
if "static void psg_trigger(int ch)" not in s:
    if marker not in s:
        raise SystemExit("Could not find audio insertion point in gba_platform.c")
    s = s.replace(marker, marker + psg + "\n", 1)

old = '''static void audio_mix(void) {
    int32_t l = 0, r = 0;
    uint16_t sc = IO16(SOUNDCNT_H);
    static int mute = -2; if (mute == -2) { const char *m = getenv("GBA_AUDIO_MUTE"); mute = m ? atoi(m) : -1; }   /* diagnostic */
    if (IO16(0x84) & 0x80) {                                     /* SOUNDCNT_X master enable */
        for (int f = 0; f < 2; f++) {
            int32_t s = fifo[f].cur * (((sc >> (2 + f)) & 1) ? 128 : 64);    /* volume bit: 100% or 50%; A+B at 100% spans int16 */
            if (mute == f) s = 0;
            if ((sc >> (f ? 12 : 8)) & 1) r += s;                             /* A: bits 8/9, B: bits 12/13 */
            if ((sc >> (f ? 13 : 9)) & 1) l += s;
        }
    }
    if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
    if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
    aud_ring[aud_wr & (AUD_RING - 1)][0] = (int16_t)l;
    aud_ring[aud_wr & (AUD_RING - 1)][1] = (int16_t)r;
    aud_wr++;
}'''
new = '''static void audio_mix(void) {
    int32_t l = 0, r = 0;
    uint16_t sc = IO16(SOUNDCNT_H);
    static int mute = -2; if (mute == -2) { const char *m = getenv("GBA_AUDIO_MUTE"); mute = m ? atoi(m) : -1; }   /* diagnostic */
    if (IO16(0x084) & 0x80) {
        for (int f = 0; f < 2; f++) {
            int32_t sample = fifo[f].cur * (((sc >> (2 + f)) & 1) ? 128 : 64);
            if (mute == f) sample = 0;
            if ((sc >> (f ? 12 : 8)) & 1) r += sample;
            if ((sc >> (f ? 13 : 9)) & 1) l += sample;
        }
        uint16_t sl = IO16(0x080);
        int master_r = (sl & 7) + 1, master_l = ((sl >> 4) & 7) + 1;
        int ratio = (sc & 3u) == 0 ? 1 : ((sc & 3u) == 1 ? 2 : 4);
        for (int ch = 0; ch < 4; ch++) {
            int32_t sample = psg_sample(ch) * ratio / 4;
            if (sl & (1u << (8 + ch))) r += sample * master_r / 8;
            if (sl & (1u << (12 + ch))) l += sample * master_l / 8;
        }
        psg_clock();
    }
    if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
    if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
    aud_ring[aud_wr & (AUD_RING - 1)][0] = (int16_t)l;
    aud_ring[aud_wr & (AUD_RING - 1)][1] = (int16_t)r;
    aud_wr++;
}'''
if old in s:
    s = s.replace(old, new, 1)
elif "psg_sample(ch) * ratio / 4" not in s:
    raise SystemExit("Could not find original audio_mix function in gba_platform.c")

needle = '    if (o == SOUNDCNT_H) { if (v & 0x0800) fifo_reset(0); if (v & 0x8000) fifo_reset(1); v &= 0x770F; }\n    IO16(o) = v;'
patch = '''    if (o == 0x084) {
        if (!(v & 0x0080)) {
            for (int ch = 0; ch < 4; ch++) psg_voice[ch].enabled = 0;
            IO16(o) = 0;
        } else IO16(o) = (uint16_t)((v & 0x0080) | (IO16(o) & 0x000F));
        return;
    }
    if (o == 0x064 || o == 0x06C || o == 0x074 || o == 0x07C) {
        int ch = o == 0x064 ? 0 : (o == 0x06C ? 1 : (o == 0x074 ? 2 : 3));
        IO16(o) = v;
        if (v & 0x8000) psg_trigger(ch);
        return;
    }
    if (o == SOUNDCNT_H) { if (v & 0x0800) fifo_reset(0); if (v & 0x8000) fifo_reset(1); v &= 0x770F; }
    IO16(o) = v;'''
if needle in s:
    s = s.replace(needle, patch, 1)
elif "psg_trigger(ch);" not in s:
    raise SystemExit("Could not find sound register write insertion point in gba_platform.c")
p.write_text(s, encoding="utf-8", newline="\n")

print("Applied PipeClean fixes: RAM overlay entry mapping, extra roots, optimized timing/audio, approximate legacy PSG synthesis.")
