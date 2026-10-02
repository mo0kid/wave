# Waldorf Wave Firmware: Reverse-Engineering Status and Findings

_Status: 2 October 2026. Supersedes the 23 August 2026 research note, which was written before the firmware was mapped._

## Executive summary

The Waldorf **Wave** runs two 68000-family programs, and **both public images have now been fully mapped**:

- **`w2sys.bin`** (main OS, OS 1.700, 302,768 B, loaded at **0x1000**, cold entry 0x100C): user interface, MIDI/SysEx, disk and FAT12 file system, wavetable editor, voice allocation, and the loader that starts the voice boards.
- **`wdv.sys`** (12,840 B = a 32-byte a.out-style header + a 12,808-byte body): the program each WDV voice board runs. The board copies it into private RAM at address 0 (SP 0x8FFE, reset vector 0x400).

Every code unit in both images was read: **2,135 ledger rows** (w2sys 2,078, wdv 57), each with purpose, RAM, device addresses and callees. About 217 of the w2sys rows are Ghidra fragments of larger functions, not separate functions. The map was then run against the real images in a 68000 core (Musashi, the same core this repository links): **15 gate files, 760,050 checks, 0 failures**, each with a mutation self-test that proves it can fail. The oracle also reproduced about 30 real firmware bugs (§6).

What this does and does not buy for emulation:

- **Firmware is now a specification, not a mystery.** The memory maps, shared-RAM protocol, boot/load sequence, file and SysEx formats, and the voice-board write sequences are known and checked against the real code.
- **The analogue/ASIC side is still open.** The two Waldorf oscillator ASICs, the CV DACs, sample-and-hold stages, CEM3387 filters/VCAs and the output path are not in the ROM. The firmware only reveals the byte stream it writes to them; register *roles* beyond the write patterns are inference. Nothing here is hardware-verified (no real unit was measured).

The complete map, the per-routine table and the gates live in a separate research folder, deliberately **outside this repository** (it sits next to a local copy of the firmware, which must not be committed):

```text
/Users/click/Desktop/Move/ideas/Waldorf Wave firmware/
  spec/firmware-map.md   whole-firmware synthesis (start here)
  spec/README.md         layers, specs, gates
  map/firmware_map.csv   one row per routine (2,135)
  oracle/                68000 oracle + 15 gates
  tools/run_gates.sh     runs everything
```

## 1. Provenance and file identities

The images come from Waldorf's public Legacy Wave page (`System.zip`: OS 1.668, 1.671, 1.680, 1.700). The `wdv.sys` image is byte-identical in all four releases; only `w2sys.bin` changes.

| OS | `w2sys.bin` size | `w2sys.bin` SHA-256 | `wdv.sys` size | `wdv.sys` SHA-256 |
|---|---:|---|---:|---|
| 1.668 | 296,376 | `42319929e0ec55fd84d2d247e57ccd5321fde610ba936adf114db48ee999d3e3` | 12,840 | `bdf379b07785af313068191961ee5ef408e267740c1b598290732477bf9d3f68` |
| 1.671 | 296,408 | `e429bfe8d3e1f00425a5c86d06d219d650644d212cb6dda4349e9b2f19aed0ed` | 12,840 | `bdf379b07785af313068191961ee5ef408e267740c1b598290732477bf9d3f68` |
| 1.680 | 296,092 | `fa1d5ea2d090206246ca4df33cecfb812854e25bb72018b48e673f4552f24494` | 12,840 | `bdf379b07785af313068191961ee5ef408e267740c1b598290732477bf9d3f68` |
| 1.700 | 302,768 | `4282457d9bf7d70da2e2aa4d6a1e69467c178d0d8d8be0a27f524ab8d62e3286` | 12,840 | `bdf379b07785af313068191961ee5ef408e267740c1b598290732477bf9d3f68` |

Only OS 1.700 has been mapped. 1.668, 1.671 and 1.680 have **not** been diffed yet. The later community 1.8xx/1.9xx releases are a separate provenance and licensing case and are not covered.

The firmware banner reads `Version No. 1.260-1.202-1.700`. The 1.260 and 1.202 are the kernel and voice-board code versions (a version page table in the OS shows them). The MIDI identity reply still reports "1400".

## 2. System architecture (as found in the code)

```text
MIDI / keybed / panel / disk / RS-232
              |
              v
    Main 68000 (w2sys.bin, 1 MB DRAM, runs from RAM)
              |
   shared SRAM 0x100000-0x17FFFF   (one RAM, seen at the same addresses by master and boards)
              |
     +--------+--------+--------+
     |        |        |
   WDV 1    WDV 2    WDV 3        (up to 3 by the 3-bit slot strap)
   68000    16 voices each
     |
 CV DACs + S/H, two 8-voice ASICs (0x980000 / 0x980100), wave RAM A/B, routing latch
```

Key facts:

- **The OS runs from RAM.** It is loaded at 0x1000 by a loader that is not in this image (an Atari-ST-style floppy boot sector). Evidence: the DOS layer stores its "current drive" in the first opcode word of `fs_init`, and the line-draw code patches opcodes in itself.
- **Boot (0x100C):** reset the three serial chips, copy a 1 KB exception-vector template to address 0, boot self-tests (cookies 0xBABEFACE / 0xFEEDFEED skip them on a warm start), heap init, load the voice boards, start the UI. Holding both panel boot switches (0xFE980020 bits 7/6) jumps into diagnostics.
- **Interrupts:** VIA timer = level 2 (system tick, key/encoder scan, LED PWM), MIDI ACIAs = level 4, console ACIA = level 5, floppy controller = level 6. Exceptions drop into the built-in System Monitor.
- **The OS also contains** a 27-command System Monitor, a diagnostics menu, a complete DOS/FAT12 file system with a group-file "database" layer, a TIFF screenshot writer, an FFT-based sample-to-wavetable analyser, FM and formant wavetable synthesis, and Microwave bulk-dump import.

### Main-CPU devices (24-bit bus: firmware address 0xFEB80001 is 0xB80001)

| address | device |
|---|---|
| 0x000000-0x0FFFFF | DRAM, 1 MB (OS image at 0x1000, heap 0x5AA80-0xFDFEA) |
| 0x100000-0x17FFFF | shared SRAM, 512 KB, battery-backed |
| 0xFEA00000-0xFEA03FFF | LCD video RAM: 1 bpp, 64 bytes/row, 4 pages of 0x1000 (480 x 64 visible) |
| 0xFEA80000 | floppy controller (uPD765/82077-type), data moved by CPU polling |
| 0xFEAA0000 | real-time clock (RP5C01-type) |
| 0xFEB80001/3, 0xFEBA0001/3 | 6850 ACIAs: MIDI A and B |
| 0xFEBC0001/3 | 6850 ACIA: RS-232 console / monitor |
| 0xFEBE0001-0x1D | 6522 VIA: tick timer, WDV reset line, board-present strap, LCD display page, FDC DRQ |
| 0xFE90..0xFE9E | keybed, buttons, encoders, LED matrix, analogue mux and ADC |

### Voice board (WDV)

| address (board bus) | device |
|---|---|
| 0x0000-0xFFFF | private RAM (program copy at 0, per-voice workspaces from 0x9000) |
| 0x100000-0x17FFFF | the same shared SRAM as the master |
| 0x040000 / 0x050000 / 0x060000 | wave RAM A / B / write-both alias; the firmware writes 8-bit samples to odd bytes at 0x60001 + (bank << 13) |
| 0x800001 | board control/status byte (bit 1 bus claim, bit 3 wave-RAM write, slot strap in bits 0-2 on read) |
| 0x880000 | CV DAC data latch (12-bit values) |
| 0x8A0000 + 16*voice + channel | sample-and-hold strobe |
| 0x8C0000 | output routing latch (write-only; 64-bit image of one cold bit per voice, from part byte +0xC "Audio Out", plus four "Analog in" bits for voices 0-3; the emulator logs the writes) |
| 0x980000 / 0x980100 | ASIC A (voices 0-7) / ASIC B (voices 8-15) |

## 3. The master-to-voice-board protocol

There is **no command parser on the board.** The master drives it through shared RAM and the board polls:

1. **Download:** the master opens `WDV.SYS`, reads the 32-byte header (accepted type words 0x107/0x108/0x10B/0x111 at +2), and copies the body to 0x100000. It then pulses the board reset via VIA port B bit 0.
2. **Board start (0x400):** the board copies 0x2000 words from 0x100000 to its own address 0, reads its slot id from the one-cold strap in `0x800001` bits 0-2, marks itself present at `0x105086 + id`, then waits for the master's go flag at `0x10508A + id`.
3. **Voice update:** the master takes the bus mutex (`0x1050E2`, test-and-set), writes a 0x100-byte voice record at `0x100000 + (16*board + voice) * 0x100`, sets that voice's bit in the per-board update mask at `0x105092`, and releases. Each voice has a semaphore at `0x1050A2 + voice`.
4. **Board scan (0x79C):** per voice, the board runs ramp, LFOs, mixer, envelopes, pitch, filter, VCA and pan stages, then writes the CV DACs, the sample-and-hold strobes and the ASIC registers.
5. **Wavetable load:** the master renders 64 waves of 64 bytes into a staging area at 0x104000, writes the bank to `0x105081` and a command to `0x105080`; each board writes the samples to wave RAM and acknowledges. The only command byte on the board is this one.
6. **Fault report:** a board fault writes a record at 0x105100 (type at +1, board id word at +2) and blinks a code on `0x800001`. The master's CA2 interrupt path reads it, prints "VOICE BOARD n" and enters the monitor.

### Modulation depth (the part the old note called "table applied once or twice")

- The WDV holds a 128-entry signed-word modulation table. It follows `clamp(sign(n) * 8 * n^2, +-32767)` with `n = index - 64`; **125 of 128 entries match**, and the three real deviations are index 6 (-29912), index 126 (31000) and index 127 (32767). Index 0 (-32767) is the clamp.
- One routine applies the table once (linear depth); the pitch routines apply it **twice**, giving a fourth-order depth curve. The two oscillator callers then `asr.l #3` before adding to the pitch accumulator, at 0xE14 and 0xECA. Pitch accumulates at 256 units per semitone, so full depth is 4095 units, about 16 semitones.
- The pitch-to-increment table has 257 words with a constant of about 19.584, not 19.58.

## 4. Data formats established

The map documents these in `spec/firmware-map.md` §4: the sound record (256 B), multi/performance record (512 B), voice record (0x100 B), global parameter block (0x180 B at 0x148800), tuning tables (4 x 128 x {coarse, fine}), velocity curves (128 B, 8 breakpoints), wavetable records (0x8A B: a 9-character name, a 0x55 marker, 64 wave numbers) and the 300 factory waves of 64 bytes, the Wave's own SysEx format (`F0 3E 03 <device> <cmd> ... <checksum> F7`), the Microwave bulk-import format, the MIDI Tuning Standard and Sample Dump handlers, the FAT12 disk layout, and the file types `.SND .PFM .SBK .PBK .ARR .TTB .VTB .GLB .WTB .GSX .SET .WVS` plus the TIFF screenshot.

## 5. Corrections to the 23 August note and to the existing emulator constants

Checked against the bytes:

- **Not a Microwave-style VIA/FDC layout at FEB8/FEBA/FEBC:** those are three 6850 serial chips (two MIDI, one RS-232 console). The VIA is at FEBE, the floppy controller at FEA8, the RTC at FEAA.
- **Waveform RAM** is at board 0x040000 / 0x050000 with a write-both alias at 0x060000, written via `0x60001 + (bank << 13)`. The emulator's `VoiceFirmwareRuntime.h` constant `waveformRamBase = 0x600000` does not match the code.
- **Shared SRAM** is one RAM seen at 0x100000 by master and boards, not a window onto board address 0. The offsets 0x5092 / 0x50A2 / 0x50E2 used in the emulator are offsets from 0x100000 and are confirmed.
- **The modulation-table `asr.l #3` sites** are 0xE14 and 0xECA, not 0x0E34 and 0x0EEA. The table has three real deviations, not four (index 0 is the clamp).
- **The master/loader details** are confirmed: `w2sys` base 0x1000, `wdv.sys` header words, SP 0x8FFE, reset 0x400, board reset on VIA port B bit 0, board presence on VIA port A bits 0-2, board id from the strap on `0x800001`.
- The old note's recommended acquisition and "do not commit firmware" policy stands.

## 6. Firmware bugs the oracle reproduced

Behaviours that the real ROM exhibits under the 68000 core and that the gates now pin: the SysEx program-change-map dump converts only 8 or 9 of 128 words; the Sample Dump receiver answers a bad checksum with ACK; the MIDI Tuning dump encoder is one semitone flat and reads its names from the wrong address; the single-note-change receiver is off by one and negative cents give an invalid data byte; a multi request always answers multi 247; `strncpy_pad_d` pads using the destination address as the count; several FAT12 directory-scan bugs (slot-15 re-scan, a sub-directory find-next that restarts at the first cluster, signed drive-letter checks); the heap `shrink` is a no-op on the first 20 blocks; `printf` mishandles `%d` of -32768; the velocity-curve loader overruns its slot; the voice-board boot pre-init uses the wrong stride for boards other than 0; and the global-parameter SysEx receiver copies 0x160 of 0x180 bytes. The full table (about 40 suspected bugs, with evidence level) is in `spec/firmware-map.md` §6.

## 7. What is still open

- **The reported OS 1.700 voice-allocation bug** (missing notes, fixed in 1.8x) is **not localised.** The voice-board image is identical to 1.680, so the cause is on the master. Candidate areas are the note-on allocation code (0x26C4-0x3162) and the key scheduler (0x35B8). This needs a diff of the 1.680 and 1.700 `w2sys.bin` plus oracle runs with the same polyphonic MIDI sequence.
- **ASIC semantics:** the *write patterns* are decoded (`spec/firmware-map.md` §4): reg 6 carries the 6-bit wave position every scan, reg 4 three bits each of Wave 1/Wave 2/Noise volume, reg 0xE commits with bit 0 = oscillator, bits 1-3 = voice, bit 4 = parameters/phase load and bits 5-7 = wave RAM bank (one of 8 resident wavetables), and the once-per-key-on write through regs 2/0/4 is the 7-bit **start phase** ("Startphase", 0 = free start, nothing written), not a wave number. What the chips do with them, the CV channel meanings (cutoff, resonance and pan are guesses) and the physical meaning of the routing bits (main / sub 1 / sub 2 by UI label only) need hardware.
- **Hardware details:** VIA port bits beyond those listed, the panel boot-switch bits, LED strobe rows (7 vs 8), the physical meaning of the analogue channels.
- **Maximum board count: resolved, 3.** The presence mask and the WDV slot strap are 3 bits wide; the "up to 4" and 64-entry tables are sizing slack (`spec/firmware-map.md` §7.2). `gate_e` runs boards 0, 1 and 2.
- **Emulator assumptions that disagree with the map (found 2026-10-02, not yet changed, hardware-unvalidated):**
  1. *Audio Out byte.* The firmware's instrument page 1 table (0x14BCA = `04 05 07 0C 09 0A 02 03`) puts "Audio Out" at part byte **+0xC**, and WDV 0xBC6 routes from that byte; the plugin reads and edits **+8** (`PluginProcessor.cpp` `pageOneRecordOffsets`, `byte(8)`), a byte nothing consumes and which is 0 in every factory set. +0xC is 1 in every factory set (enum labels at 0x29D24: Aux only / main / sub 1 / sub 2), so the plugin's `audioOutput == 0` test is right only by accident. A fix has to read +0xC and map 1 -> main; sets with +0xC = 0 (three parts in `pg.set`/`ultimate.set`) would then leave the main mix.
  2. *Start modulation.* The firmware adds the route in sound bytes 28/29 (44/45) to **Startphase** (byte 27/43, written once at key-on, 0 = free start) and clamps it to 1-127; `WaldorfEngine` applies `wave1StartMod`/`wave2StartMod` as a wave-position offset sampled at note-on instead. The engine's Startphase itself (`wavePhases`, 0 = free start) matches the write pattern.
  3. *Level resolution.* reg 4 carries only 3 bits per level (value 0-0x70 >> 4); the engine mixes 7-bit level codes (0-112) from the ES2 description. Whether the chip has another level path is open.
  Single wavetable per voice (one bank slot for both oscillators), 64 waves of 64 stored samples per table, 8 resident tables per board and 128 table ids (64 factory + 64 user) all match `WavetableBank` and `WaldorfEngine`; the mirrored second half of each wave is an inference that no write shows.
- **Static only:** the UI pages, the Wave Editor, the LFO, env3/env4 and glide stages on the board, and anything timing-dependent.

## 8. What this means for sample-accurate emulation

"Sample-accurate" still requires the same things the earlier note listed: one deterministic emulated timeline, timestamped side effects independent of host block size, a digital voice model for the ASIC, and a measured mixed-signal model for the converters, filters and output stage. The mapping removes the guesswork from the *control* side: the order and content of every write the firmware makes to the ASICs, DACs and strobes is now documented and gated. The remaining work is on the other side of that boundary, where only measurements of a real Wave can settle the open points in §7.

Practical next steps:

- [ ] Check the emulator's constants and behaviours against `spec/firmware-map.md` §3-§5 (starting with the waveform-RAM address).
- [ ] Diff OS 1.668, 1.671, 1.680 and 1.700 `w2sys.bin` and localise the voice-allocation change.
- [x] Capture ASIC/CV write traces for one note from the oracle and compare them with the emulator's (2026-10-02, `oracle/gate_e_wdv_note_trace.py` + `Tests/WaveVoiceTrace.cpp`). The same script (boot, seeded voice records, an idle scan, key-on of voices 3 and 9, three more scans) runs on the Musashi oracle and on `VoiceFirmwareRuntime`; all 11,146 logged writes (CV latch, CV strobes, ASIC, incl. tick-IRQ acknowledges) match in order, value and cycle stamp. Both sides use the same 68000 core, so this checks the emulator's bus decode, shared RAM and IRQ plumbing, not the ROM. Findings: `WaldorfAsic.cpp` never sees these writes (it is a behavioural proxy driven by the engine); `WaldorfEngine::applyFirmwareHardwareWrite` only shadows the bytes and nothing reads the shadow yet; at that time the emulator did not log the routing latch (0x8C0000, 128 byte-writes per scan) or the board-control latch (0x800001). Since then the routing latch is logged and diffed too, and the gate runs on all three boards (35,004 checks, 0 failures).
- [ ] Plan the hardware captures (bus, ASIC select, DAC timing, calibrated audio) needed to pin the ASIC and analogue model.

## 9. Legal and practical caveats

- Waldorf publicly hosts the archive, which supports legitimate research use. It is not a licence to redistribute. **Do not commit `System.zip`, `w2sys.bin`, `wdv.sys`, the factory wavetables or derived bulk firmware data** to this or any repository. Keep hashes, sizes, addresses, annotations and independently written code only.
- Verify both images together; a mismatched pair can boot yet invalidate results. Later community OS images need separate provenance.
- Hardware revisions matter (the OS detects old and new voice hardware); machine-specific calibration data in the shared SRAM is not firmware and should not be shared between instruments.
- Firmware does not guarantee bit-perfect sound. The defensible goal is deterministic, traceable digital/control behaviour plus a measured analogue model with documented tolerances.

## References

- [Waldorf Music: Legacy Wave](https://waldorfmusic.com/legacy-wave/)
- [Waldorf-hosted Wave `System.zip` share](https://downloads.waldorfmusic.com/cloud/index.php/s/7zzTnmzK2pYY9Ks)
- [Waldorf Wave Service Manual](https://www.waldorf-wave.de/Waldorf-WAVE-Service-Manual.pdf)
- [Unofficial Wave boot and OS notes](https://unofficial.waldorf-wave.de/waveif.html)
- [Unofficial Wave hardware overview](https://www.waldorf-wave.de/wavetech.html)
- [Wave System Exclusive description](https://unofficial.waldorf-wave.de/os/OS_1-900_WAVE_Sysex_manual.pdf)
- [Wave OS 1.911 manual](https://unofficial.waldorf-wave.de/os/OS_1-911.pdf)
