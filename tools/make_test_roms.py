#!/usr/bin/env python3
"""Writes tiny original test ROMs for systems with no freely licensed test
ROM to download: each one just sets up its video chip and sound chip and then
loops, so a core can be smoke-tested (load, run N frames, save state round
trip) under qemu. They contain no third-party code.

  tools/make_test_roms.py tests/roms
"""

import os
import sys


def sms_program(game_gear):
    """Z80: mode 4 display on, a palette entry, a PSG tone, then loop."""
    code = bytearray()

    def op(*b):
        code.extend(b)

    def vdp_reg(reg, value):
        op(0x3E, value, 0xD3, 0xBF, 0x3E, 0x80 | reg, 0xD3, 0xBF)

    op(0xF3, 0xED, 0x56)                    # di, im 1
    op(0x31, 0xF0, 0xDF)                    # ld sp,0xdff0
    vdp_reg(0, 0x04)                        # mode 4
    vdp_reg(1, 0x40)                        # display on, no interrupts
    vdp_reg(2, 0xFF)                        # name table 0x3800
    vdp_reg(5, 0xFF)
    vdp_reg(7, 0x00)
    vdp_reg(8, 0x00)
    vdp_reg(9, 0x00)
    vdp_reg(10, 0xFF)
    # CRAM address 0, colour 0 then 1
    op(0x3E, 0x00, 0xD3, 0xBF, 0x3E, 0xC0, 0xD3, 0xBF)
    for value in ((0x2A, 0x0F) if game_gear else (0x2A,)):
        op(0x3E, value, 0xD3, 0xBE)
    # PSG: tone channel 0 at a mid pitch, loudest
    op(0x3E, 0x8E, 0xD3, 0x7F, 0x3E, 0x0F, 0xD3, 0x7F, 0x3E, 0x90, 0xD3, 0x7F)
    op(0x18, 0xFE)                          # jr $
    return bytes(code)


def sms_rom(game_gear):
    rom = bytearray(0x8000)
    program = sms_program(game_gear)
    rom[0:len(program)] = program
    rom[0x66:0x69] = bytes((0xED, 0x45, 0))  # retn at the NMI vector
    rom[0x7FF0:0x7FF8] = b"TMR SEGA"
    checksum = sum(rom[:0x7FF0]) & 0xFFFF
    rom[0x7FFA] = checksum & 0xFF
    rom[0x7FFB] = checksum >> 8
    rom[0x7FFF] = 0x6C if game_gear else 0x4C  # region (GG export / SMS export), 32 KB
    return bytes(rom)


def sg1000_rom():
    rom = bytearray(0x8000)
    program = bytes((0xF3, 0xED, 0x56, 0x31, 0xF0, 0xC3, 0x18, 0xFE))
    rom[0:len(program)] = program
    rom[0x66:0x69] = bytes((0xED, 0x45, 0))
    return bytes(rom)


def nes_rom():
    """The same mapper 0 ROM frontend/selftest.c builds: one infinite loop, 8 KB battery RAM."""
    rom = bytearray(16 + 0x4000 + 0x2000)
    rom[0:4] = b"NES\x1a"
    rom[4], rom[5], rom[6] = 1, 1, 0x02
    rom[16:24] = bytes((0x78, 0xD8, 0xA2, 0xFF, 0x9A, 0x4C, 0x05, 0xC0))
    for i in range(3):
        rom[16 + 0x4000 - 6 + i * 2] = 0x00
        rom[16 + 0x4000 - 6 + i * 2 + 1] = 0xC0
    return bytes(rom)


def wonderswan_rom():
    """128 KB: the reset vector (the last 16 bytes) jumps far to F000:0000, where `jmp $` loops."""
    rom = bytearray(b"\xff" * 0x20000)
    rom[0x10000:0x10002] = bytes((0xEB, 0xFE))
    footer = bytes((0xEA, 0x00, 0x00, 0x00, 0xF0,   # jmp far F000:0000
                    0x00, 0x00, 0x01, 0x00, 0x00,   # maintenance, publisher, colour, cart id, version
                    0x01, 0x00, 0x04, 0x00))         # ROM size code, save type none, flags, (checksum below)
    rom[0x1FFF0:0x1FFF0 + len(footer)] = footer
    checksum = sum(rom[:0x1FFFE]) & 0xFFFF
    rom[0x1FFFE] = checksum & 0xFF
    rom[0x1FFFF] = checksum >> 8
    return bytes(rom)


def pce_rom():
    """8 KB HuCard: the reset vector points at E000, where `sei; csl; bra $` loops."""
    rom = bytearray(0x2000)
    rom[0:4] = bytes((0x78, 0x54, 0x80, 0xFE))
    for vec in (0x1FF6, 0x1FF8, 0x1FFA, 0x1FFC, 0x1FFE):
        rom[vec:vec + 2] = bytes((0x00, 0xE0))
    return bytes(rom)


def md_music_rom():
    """32 KB Mega Drive ROM that plays a looping tune on the YM2612 (a lead and a bass) and the SN76489 (a
    harmony tone and a hi-hat on the noise channel), one 8-frame step at a time, for audio tests. The 68000
    program is a small interpreter over a command table: 3-byte entries (kind, a, b) with kind 0 = YM2612
    port 0 write (register a, value b), 1 = port 1 write, 2 = two PSG bytes, 3 = wait a frames, 4 = loop here,
    anything else = jump back to the loop point."""
    code = bytearray()
    labels = {}
    fixups = []  # (offset of the 8-bit displacement byte, label)

    def w(*words):
        for x in words:
            code.extend(((x >> 8) & 0xFF, x & 0xFF))

    def l(x):
        w(x >> 16, x & 0xFFFF)

    def label(name):
        labels[name] = len(code)

    def branch(opcode, name):
        code.append(opcode)
        fixups.append((len(code), name))
        code.append(0)

    def vdp_reg(reg, value):
        w(0x33FC, 0x8000 | (reg << 8) | value); l(0x00C00004)

    # ---- program, at 0x206 (0x200 is a BRA to it, 0x204 an RTE every vector points at)
    label("main")
    w(0x33FC, 0x0100); l(0x00A11100)               # Z80 bus request
    w(0x33FC, 0x0100); l(0x00A11200)               # Z80 reset released
    label("grant")
    w(0x0839, 0x0000); l(0x00A11100)               # btst #0,$A11100
    branch(0x66, "grant")                          # bne grant
    vdp_reg(0, 0x04); vdp_reg(1, 0x44); vdp_reg(7, 0x00); vdp_reg(12, 0x81); vdp_reg(15, 0x02)
    w(0x23FC); l(0xC0000000); l(0x00C00004)        # CRAM write address 0
    w(0x33FC, 0x0620); l(0x00C00000)               # backdrop colour
    w(0x43F9); l(0x00A04000)                       # lea $A04000,a1 (YM2612)
    w(0x45F9); l(0x00C00011)                       # lea $C00011,a2 (PSG)
    w(0x47F9); l(0x00C00004)                       # lea $C00004,a3 (VDP control)
    w(0x41FA); fixups.append((len(code), "data16")); w(0)  # lea data(pc),a0 (displacement fixed below)
    label("next")
    w(0x1018, 0x1218, 0x1418)                      # move.b (a0)+,d0 / d1 / d2
    w(0x4A00); branch(0x67, "ym1")                 # tst.b d0 ; beq
    w(0x5300); branch(0x67, "ym2")                 # subq.b #1,d0 ; beq
    w(0x5300); branch(0x67, "psg")
    w(0x5300); branch(0x67, "wait")
    w(0x5300); branch(0x67, "setloop")
    w(0x204C); branch(0x60, "next")                # movea.l a4,a0 ; bra next
    label("ym1")
    w(0x1281, 0x1342, 0x0001); branch(0x60, "next")  # move.b d1,(a1) ; move.b d2,1(a1)
    label("ym2")
    w(0x1341, 0x0002, 0x1342, 0x0003); branch(0x60, "next")
    label("psg")
    w(0x1481, 0x1482); branch(0x60, "next")        # move.b d1,(a2) ; move.b d2,(a2)
    label("setloop")
    w(0x2848); branch(0x60, "next")                # movea.l a0,a4
    label("wait")
    label("w1")
    w(0x082B, 0x0003, 0x0005); branch(0x66, "w1")  # btst #3,5(a3) ; bne (still in the last vblank)
    label("w2")
    w(0x082B, 0x0003, 0x0005); branch(0x67, "w2")  # btst #3,5(a3) ; beq (wait for the next one)
    w(0x5301); branch(0x66, "w1")                  # subq.b #1,d1 ; bne w1
    branch(0x60, "next")
    for off, name in fixups:
        if name == "data16":
            continue
        disp = labels[name] - (off + 1)
        assert -128 <= disp <= 127 and disp != 0, name
        code[off] = disp & 0xFF

    # ---- music data
    data = bytearray()

    def ym(port, reg, val):
        data.extend((port, reg, val))

    def psg(a, b=None):
        data.extend((2, a, a if b is None else b))

    def wait(frames):
        data.extend((3, frames, 0))

    # Two FM channels (0 lead, 1 bass), algorithm 7: operator 1 and 2 audible, 3 and 4 muted.
    for ch in (0, 1):
        for op_off, tl, mul in ((0x00, 0x06, 1), (0x08, 0x20, 2), (0x04, 0x7F, 1), (0x0C, 0x7F, 1)):
            base = op_off + ch
            ym(0, 0x30 + base, mul)
            ym(0, 0x40 + base, tl)
            ym(0, 0x50 + base, 0x1F)
            ym(0, 0x60 + base, 0x0A)
            ym(0, 0x70 + base, 0x03)
            ym(0, 0x80 + base, 0x2A)
        ym(0, 0xB0 + ch, 0x07)
        ym(0, 0xB4 + ch, 0xC0)
    data.extend((4, 0, 0))  # loop point

    ym_clock, psg_clock = 7670454.0, 3579545.0

    def fm_pitch(freq):
        # fnum = 144 * f * 2^20 / clock / 2^(block - 1); the smallest block that fits 11 bits
        for block in range(8):
            fnum = 144 * freq * (1 << 21) / ym_clock / (1 << block)
            if fnum < 2048:
                break
        fnum = int(round(fnum))
        return (block << 3) | (fnum >> 8), fnum & 0xFF

    def note(midi):
        return 440.0 * 2 ** ((midi - 69) / 12)

    def fm_note(ch, midi):
        hi, lo = fm_pitch(note(midi))
        ym(0, 0x28, ch)           # key off
        ym(0, 0xA4 + ch, hi)
        ym(0, 0xA0 + ch, lo)
        ym(0, 0x28, 0xF0 | ch)    # key on, all operators

    def psg_tone(midi, atten):
        n = max(1, min(1023, int(round(psg_clock / (32 * note(midi))))))
        psg(0x80 | (n & 0x0F), (n >> 4) & 0x3F)  # latch+low bits, then high bits (a second byte, not a volume)
        psg(0x90 | atten)

    melody = [69, 72, 76, 72, 69, 72, 77, 74, 67, 71, 74, 71, 67, 71, 76, 72,
              69, 72, 76, 81, 79, 76, 72, 76, 74, 77, 74, 71, 72, 69, 67, 64]
    bass = [45, 45, 52, 45, 41, 41, 48, 41, 43, 43, 50, 43, 45, 45, 52, 45]
    for step in range(64):
        m = melody[step % len(melody)]
        fm_note(0, m)
        if step % 2 == 0:
            fm_note(1, bass[(step // 2) % len(bass)] - 12)
        psg_tone(m - 12 if step % 4 else m - 24, 6)
        psg(0xE4)                  # noise: white, medium rate
        psg(0xF2)                  # noise volume: loud
        wait(2)
        psg(0xF6)
        wait(2)
        psg(0xFF if step % 2 else 0xF9)
        wait(4)
    data.extend((0xFF, 0, 0))      # back to the loop point

    # ---- ROM image
    rom = bytearray(0x8000)
    stub = 0x204
    vectors = [0x00FFFE00, 0x200] + [stub] * 62
    for i, v in enumerate(vectors):
        rom[i * 4:i * 4 + 4] = v.to_bytes(4, "big")
    header = (b"SEGA MEGA DRIVE " + b"(C)COMPAS 2026".ljust(16) + b"COMPAS AUDIO TEST".ljust(48) +
              b"COMPAS AUDIO TEST".ljust(48) + b"GM 00000000-00" + b"\x00\x00" + b"J".ljust(16) +
              (0).to_bytes(4, "big") + (0x7FFF).to_bytes(4, "big") + (0xFF0000).to_bytes(4, "big") +
              (0xFFFFFF).to_bytes(4, "big") + b" " * 12 + b" " * 12 + b" " * 40 + b"JUE".ljust(16))
    assert len(header) == 0x100, len(header)
    rom[0x100:0x200] = header
    start = 0x206
    rom[0x200:0x204] = bytes((0x60, 0x00)) + (start - 0x202).to_bytes(2, "big")  # bra.w main
    rom[0x204:0x206] = bytes((0x4E, 0x73))                                         # rte
    rom[start:start + len(code)] = code
    data_at = start + len(code)
    # lea data(pc),a0: the displacement is relative to the extension word
    for off, name in fixups:
        if name == "data16":
            disp = data_at - (start + off)
            rom[start + off:start + off + 2] = disp.to_bytes(2, "big", signed=True)
    rom[data_at:data_at + len(data)] = data
    assert data_at + len(data) < 0x8000
    checksum = 0
    for i in range(0x200, 0x8000, 2):
        checksum = (checksum + (rom[i] << 8 | rom[i + 1])) & 0xFFFF
    rom[0x18E:0x190] = checksum.to_bytes(2, "big")
    return bytes(rom)


def main():
    out = sys.argv[1] if len(sys.argv) == 2 else "tests/roms"
    os.makedirs(out, exist_ok=True)
    for name, data in (("test.nes", nes_rom()), ("test.sms", sms_rom(False)), ("test.gg", sms_rom(True)), ("test.sg", sg1000_rom()), ("test.ws", wonderswan_rom()), ("test.wsc", wonderswan_rom()), ("test.pce", pce_rom()), ("md_music.md", md_music_rom())):
        with open(os.path.join(out, name), "wb") as f:
            f.write(data)
        print(f"wrote {out}/{name}")


if __name__ == "__main__":
    main()
