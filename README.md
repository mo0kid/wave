# Wave Emulation

![Wave Emulation standalone interface](media/wave-emulation-screenshot.png)

A JUCE C++ research instrument that recreates the documented Waldorf Wave signal path: 250 kHz 8-bit/time-multiplexed wavetable voices, the ES2 ASIC's signed mixer overflow, the ASIC's 12 dB digital high-pass, CEM3387 three-pole reconstruction and separately saturating nonlinear resonant four-pole low-pass sections, 12-bit control-voltage stepping, VCA/panning, and the 480 x 64 monochrome graphic LCD.

## Downloads

| Platform | Download | Included formats |
| --- | --- | --- |
| Windows x64 — 0.1.10 | [Windows ZIP](https://github.com/mo0kid/wave/releases/download/v0.1.10/Wave-Emulation-0.1.10-Windows-x64.zip) | Standalone EXE and VST3 |
| macOS 13.0 or later, Apple Silicon and Intel — 0.1.11 | [macOS installer DMG](https://github.com/mo0kid/wave/releases/download/v0.1.11/DJW.Wave.Emulation.0.1.11.dmg) | Standalone app, AU, VST3 and AAX |

See the [Windows 0.1.10 release page](https://github.com/mo0kid/wave/releases/tag/v0.1.10)
and [macOS 0.1.11 release page](https://github.com/mo0kid/wave/releases/tag/v0.1.11)
for release notes. The Windows build's
[corresponding source](https://github.com/mo0kid/wave/releases/download/v0.1.10/Wave-Emulation-0.1.10-Source.zip)
includes its modified dependencies; use that archive to reproduce the released
Windows binaries. The accompanying
[SHA-256 checksums](https://github.com/mo0kid/wave/releases/download/v0.1.10/SHA256SUMS.txt)
cover the Windows ZIP, its source archive and Windows release notes.

Supply your own Wave OS 1.700 firmware and sound disks. Neither platform's
release includes firmware, ROM archives or Wave factory sound SETs.

Enjoying Wave Emulation? [Leave a tip on Ko-fi](https://ko-fi.com/djw_audio) to support its development.

<a href="https://ko-fi.com/djw_audio"><img src="https://storage.ko-fi.com/cdn/kofi5.png?v=3" height="36" alt="Support development with a tip on Ko-fi"></a>

The public build includes the decoded PPG V6 wavetable sample bank, with
procedural fallback for the remaining tables. It contains no executable firmware,
ROM archives, Wave factory sound sets, or upper Wave factory wavetable payload.
See [data provenance and format](data/README.md). You can load your own
Wave OS 1.700 firmware (`w2sys.bin` and `wdv.sys`) and wavetable images locally.
The loader checks the firmware hashes. Waldorf publishes system downloads on
its [legacy Wave page](https://waldorfmusic.com/legacy-wave/).

The editor embeds `media/WaldorfWaveUI_NOLOGO.svg` as its 2338 x 1042 source artwork. JUCE controls are transparent hit regions aligned to the artwork coordinates, and the live framebuffer is rendered only inside the LCD rectangle at `(939, 237, 448, 70)`. Additional panel switch, rotary, and fader regions are derived at runtime from the circles and rounded rectangles already in that SVG. The eight Performance faders use relative mouse dragging, retain their physical positions across Performance changes, and apply the destination/parameter assignments stored in each native factory Performance record only after a fader is moved. A playable 61-note keyboard spans C2 through C7 in the lower black keyboard bed, with pressed-key feedback and drag glissando.

Use **Cmd/Ctrl + =** and **Cmd/Ctrl + -** to zoom the editor in and out, **Cmd/Ctrl + 0** for actual size, and **Cmd/Ctrl + K** to show or hide the lower keyboard and controller area. The same actions are in the System menu. Hiding the lower area shortens the window while keeping the upper panel at the same scale.

Hover over any knob or fader to see its tooltip. Knob names follow the selected LFO and envelope page; hold Shift while dragging a knob for finer adjustment. Fader tips identify the numbered control and whether it uses a Performance assignment or the current LCD parameter.

To use your own artwork, choose **System → Panel Skin → Load Alternative SVG Skin…**. Choose **Original** to return to the bundled panel. The selected file is remembered for future instances; keep it at the same path. Missing or invalid files fall back to the original skin.

Create a larger-label skin by copying [the original panel SVG](media/WaldorfWaveUI_NOLOGO.svg) and editing its labels, colours, or decoration. Keep `width="2338"`, `height="1042"`, and `viewBox="0 0 2338 1042"`, and leave all control, LCD, LED, and keyboard positions unchanged. Only panel artwork is replaced; the moving controls, keyboard, LEDs, LCD, and interaction geometry retain their original layout. Export text as paths for consistent rendering across machines. Reload the SVG from the menu after editing it.

## Installing the Windows release

1. Download and extract the Windows ZIP. Run
   `Standalone/Wave Emulation.exe` to use the portable standalone application.
2. To install the VST3, quit your DAW and copy the entire
   `VST3/Wave Emulation.vst3` folder into
   `C:\Program Files\Common Files\VST3`. Copying into Program Files may require
   administrator permission. Reopen your DAW and rescan plug-ins, then add
   **Wave Emulation** as a virtual instrument.
3. Load your firmware folder and sound disk in the standalone or plug-in using
   the [firmware and sound-disk instructions](#before-you-start-system-and-sound-floppies).

The Windows binaries are unsigned and do not require a separate Visual C++
redistributable installation. The ZIP includes installation instructions and
licenses. Windows builds include VST3 and standalone formats; AU and AAX are
available in the macOS release.

## Installing the macOS release

Builds target macOS Ventura 13.0 or later on Apple Silicon and Intel Macs.
Plugin use also requires a host compatible with your macOS version. CPU and RAM
minimums have not yet been established by testing. Earlier release binaries may
have a higher macOS minimum; they must be rebuilt to support Ventura.

Download the macOS DMG linked above and run the installer inside it.
Starting with version 0.1.2, it installs
**Wave Emulation.app** in `/Applications` as well as the AU, VST3, and AAX plugins.
Quit any older standalone instance, then launch `/Applications/Wave Emulation.app`.
Older development copies named **Wave Emulation Sample.app** are separate files
and are not updated by this installer. Versions 0.1.0 and 0.1.1 installed only
the plugins, so they did not update a standalone app you already had open.

## What's new in 0.1.11

[Version 0.1.11](https://github.com/mo0kid/wave/releases/tag/v0.1.11) restores complete Performance and Sound banks when reopening, including native Store names containing NUL bytes. It fixes Total Recall after cancelling optional machine-specific adjustments, mode lamps after Store, repeated + browsing after Store, and pending writes when remounting the same disk image.

The macOS installer is signed, notarized, and contains universal Apple Silicon and Intel builds of the standalone app, AU, VST3, and PACE-wrapped AAX. The [macOS source archive](https://github.com/mo0kid/wave/releases/download/v0.1.11/Wave-Emulation-0.1.11-Source.zip) includes the modified dependencies used for the build. [Checksums](https://github.com/mo0kid/wave/releases/download/v0.1.11/SHA256SUMS.txt) cover the installer, source archive, and release notes.

## What's new in 0.1.10

[Version 0.1.10](https://github.com/mo0kid/wave/releases/tag/v0.1.10) is a Windows
maintenance release. It restores embedded Common Controls v6 manifests in the
standalone EXE and VST3 DLL, addressing the **Ordinal 345 not found** startup
error reported on Windows 10 in [issue #2](https://github.com/mo0kid/wave/issues/2).
The build workflow now checks both manifests before running tests.

Windows 10 runtime confirmation is pending. The native MSVC CI build also
reports CPU-test crashes that remain under investigation. The macOS download
remains version 0.1.8.

## Version history

### Version 0.1.8

Version 0.1.8 is available for Windows x64 as a standalone EXE and VST3, as well
as the macOS formats above. The Windows build improves window startup,
shutdown and UI refresh when display refresh notifications are unavailable.

[Version 0.1.8](https://github.com/mo0kid/wave/releases/tag/v0.1.8) preserves saved Sound parameters, including oscillator octave,
when switching Performances and layers. It also prevents a display refresh
from replacing a Performance name during Store, clears inactive destination
layers when overwriting a Performance, and handles brief Store Cancel clicks.
Store also keeps mode buttons and their LEDs in sync with the firmware page.
Instrument Edit faders follow the firmware's current page so a tuning change
cannot write to Transpose or another page's parameter. The TuneTable slider's
choices now follow the displayed positions, and HMT changes preserve the pitch
of held and newly played notes. Window zoom and keyboard visibility shortcuts
are available in the System menu.

### Earlier releases

[Version 0.1.7](https://github.com/mo0kid/wave/releases/tag/v0.1.7) fixes repeated or missed front-panel actions in Disk, Store, and
Instrument Edit, including name cursors and Load-menu stepping. It also keeps
Performance program names and layered sounds consistent when switching modes.

[Version 0.1.6](https://github.com/mo0kid/wave/releases/tag/v0.1.6) targets
macOS Ventura 13.0 and later in universal Apple Silicon/Intel builds of the
standalone app, AU, VST3 and AAX plugins. The installer checks the minimum OS,
and packaging verifies both architectures target the expected macOS version.
The binaries and automated tests have been checked on the development Mac;
runtime testing on Ventura is still pending.

[Version 0.1.5](https://github.com/mo0kid/wave/releases/tag/v0.1.5) synchronizes
host preset save/restore with rendering so firmware reload cannot replace shared
memory while the emulated CPUs are using it. It also retires leftover Edit-page
+/- events when selecting a Performance, preventing delayed presses or retries
from restarting patch stepping after release. Performance-page +/- remains one
patch per click. Preset save/recall and patch stepping were validated by the
project owner in Logic Pro before publication.

The release remembers the firmware folder for new instances, keeps separate
instances' firmware-emulator execution state independent, and connects voice-card
workers to the host's macOS audio workgroup when provided. Version 0.1.3 remains
withdrawn; users of that version should update to 0.1.5.

## CPU load in Logic Pro

Enable **System → Eco Mode (applies to new notes)** to reduce voice CPU usage.
Eco runs the oscillator, digital high-pass, and reconstruction stages at
62.5 kHz instead of 250 kHz. The resonant CEM filter retains its existing
integration rate, and firmware timing is unchanged. Eco is a sound-quality
tradeoff: bright sounds, high notes, and noise can differ from the full-rate
model. Full quality is the default, and the choice is saved with the project.
Held notes and their release tails keep the quality they started with; changing
the setting affects newly triggered notes.

Wave can split voice processing across three threads at higher polyphony.
Light loads and very short audio segments run serially to avoid worker overhead;
the firmware timeline also remains sequential. A single busy bar in Logic's
meter therefore does not imply that all work can be evenly spread across cores.

For multiple live instrument channel strips, try **Settings > Audio > Devices >
Multithreading > Playback & Live Tracks**. This lets Logic distribute eligible
live tracks across processing threads; it does not automatically divide one
plugin's processing. See [Apple's multithreading guide](https://support.apple.com/en-ae/101975).

## Before you start: system and sound floppies

**You must obtain your own Waldorf Wave system floppy files to run the original
operating system in this emulation.** The release packages do not contain the
Wave system ROM, voice firmware, factory sound disks, or floppy images. The
included PPG wavetables are sound data, not the Wave operating system.

1. Find your Wave OS 1.700 system floppy or a copy of its files. You need both
   `w2sys.bin` and `wdv.sys` together in a folder on your computer. The
   [Waldorf legacy Wave page](https://waldorfmusic.com/legacy-wave/) is the
   starting point for the system download; extract its archive before use.
2. Open the emulation's **System** menu and choose **Load System Firmware
   Folder...**, then select that folder. The loader checks the system files
   against the supported firmware. From version 0.1.3, a successful selection is
   remembered for new plugin and standalone instances, including after restarting
   your DAW. Saved projects retain their own folder reference, with the remembered
   folder as a fallback. Keep the files in place; if you move them, select their
   new folder once. Only the location is remembered, not a copy of the firmware.
3. To use original sounds and performances, obtain your own Wave sound/setup
   floppies or disk images. For a physical disk, first make a raw MS-DOS floppy
   image with suitable disk-imaging hardware/software. In the **System** menu,
   choose **Mount Disk Image...** and select an `.img`, `.ima`, `.dsk`, or `.st`
   image. These extensions must contain a supported raw floppy image, not a ZIP.
   Use the Wave panel's Disk controls to load its contents.
4. If you have a Wave `.set` file instead, choose **Create Disk Image from Wave
   Setup...** to create and mount a 720 KB DD image, then confirm the SET import
   with the Wave panel's **OK** button. Creating a blank disk instead provides
   no system firmware or factory sounds.

Until a sound SET is imported, the public build starts with one INIT
performance. Performance -/+ cannot select another patch from that empty bank.
Each plug-in instance has its own sound-bank state; loading a disk in the
standalone does not load it
into a DAW instance. Saving the DAW project preserves that instance's bank.

The app can open without firmware and provide its behavioural synthesis
fallback, but that does not run the original Wave operating system. Mounting a
sound disk alone does not supply the system firmware. This implementation does
not emulate the complete original floppy boot sequence; system firmware is
loaded separately as described above.

## Accuracy boundary

This sample is honest about a hard distinction:

- MIDI events, envelopes, the behavioural oscillator chip's 250 kHz sample-and-hold updates, CV quantisation, filters, VCAs, and panning run sample-accurately within the current model.
- The three physical 16-voice cards are rendered as independent jobs: the host audio thread renders card 1 while two persistent real-time workers render cards 2 and 3. All worker buffers are allocated during `prepareToPlay`, card state has single-thread ownership, and results are summed in deterministic hardware-voice order. Short MIDI fragments, light polyphony, machines with fewer than three CPU cores, or an explicitly disabled threaded renderer use the same serial path.
- The original binaries are loaded, identified, parsed, and retained as the authoritative OS/voice-driver revision. `w2sys.bin` is installed at its linked `$001000` address and enters at `$00100C`; this matters because later OS calls use absolute addresses.
- The master 68000 runs continuously on a 16 MHz emulated clock with a 1 kHz timer source. The model exposes the documented UART, timer/mode, RTC/latch, front-panel switch/LED, ADC-selection/readback, shared-memory, and LCD windows used by the authenticated boot path.
- The WDV voice image's 32-byte loader header is decoded and its genuine 68000 body publishes at shared mailbox `$105086`, waits for the main CPU at `$10508A`, and reaches its service state after that externally visible acknowledgement. Production code no longer inserts a synthetic acknowledgement.
- Both CPU runtimes use the same shared program/work SRAM object. The genuine master `w2sys.bin` executes its cold-start hardware writes and its OS 1.700 post-copy loader handoff, writing the observed acknowledgement value `1` to `$10508A`; the WDV CPU then publishes service at `$10508E`.
- The decoded WDV service transport uses a per-board 16-voice update mask at `$105092`, per-voice TAS semaphores at `$1050A2`, and 256-byte voice records beginning at `$100000`. The WDV runtime maps its two wave RAMs at `$040000`/`$050000` (with a write-both alias at `$060000`), CV banks at `$880000/$8A0000`, and two oscillator-chip register pages at `$980000/$980100`. All writes are timestamped on the WDV clock for correlation with captures.
- The current bridge supplies `INIT.SND` and `INIT.PFM` through a narrowly scoped virtual system-disk service, and the genuine OS 1.700 loader opens, reads, and closes both files. Until original factory files are available, their contents are byte-exact copies of the genuine safe sound and performance records embedded in OS 1.700; they are conservative defaults, not claimed factory banks. The same records are preloaded as a defensive fallback. The CPU board's DP8473 floppy controller is modelled for mounted raw MS-DOS disk images; the separate boot-time `WDV.SYS` copy remains bypassed because the authenticated WDV body is installed directly into shared SRAM before the genuine master instructions resume at the verified post-copy handoff. This is not presented as a complete main-OS boot.
- The CPU-board LCD is represented as two byte-wide 5563 SRAMs forming a 16-bit word, four 4 KiB pages, two parallel 74LS166 shifters, and the firmware page latch at `$BE0001`. The genuine OS renderer proves a 64-byte hardware scanline: 60 visible bytes for 480 pixels followed by four blanking bytes. Writes in the `$FEA00000-$FEA03FFF` video window automatically replace the model diagnostics with correctly decoded firmware scanout.
- The oscillator ASIC is an undocumented black box. Its internal algorithms cannot be recovered or claimed from the available documentation. The DSP is explicitly an external behavioural proxy, using Waldorf's documented 250 kHz rate and ES2 eight-bit numerical mix-overflow boundary; its exact accumulator ordering and truncation remain to be validated against real-hardware measurements. Firmware register traces reveal only the external contract.
- The included procedural wavetable bank is a neutral fallback. The loader accepts a user-supplied exact 64 or 128 x 64 x 128 signed-eight-bit dump, a 32 x 64 x 128 first-table dump, or a raw PPG Wave 2.2/2.3 EPROM image. Raw PPG images are decoded from their 768-byte sparse table directory and 256 stored 64-sample half waves. The V6 firmware's recursive midpoint averaging, complemented half-cycle, calculated tables 28/29, and byte-exact triangle/pulse/square/saw tail are reproduced, yielding all 30 original lower tables. Native Wave SET images also install all 64 user Wavetables and their 1000-Wave pool. The lower 30 PPG tables are included as decoded samples. Remaining tables retain procedural fallback data until a user imports a bank.
- The filter path implements all four sound-record modes: analogue 24 dB low-pass, digital 12 dB high-pass, linked serial band-pass with width, and Dual mode with independently modulated HP/LP cutoffs. The WDV frequency table establishes semitone-spaced cutoff values beginning at 20 Hz. The fixed CEM3387 three-pole 1 dB Chebyshev reconstruction section and nonlinear resonant four-pole low-pass are separate from the pre-analogue ASIC high-pass. Envelope selection, velocity, key tracking, both modulation routes, resonance modulation, and maximum-resonance self-oscillation are active. Unknown ASIC arithmetic and unmeasured component tolerances remain explicit calibration hypotheses.
- The mixed-signal path explicitly models eight-bit multiplexed conversion, signed ES2 mixer wrap, sample-and-hold behaviour, three-pole reconstruction and AC coupling, the distinct VCF input saturation around 70% mixer output, AD7545-style 12-bit CV quantisation, nonlinear four-pole CEM3387 filtering, VCA bleed/noise, panning, output coupling, slew, and rails. Values without measurements remain documented calibration hypotheses.
- MIDI bytes are delivered to the master UART at their host-sample offsets while the behavioural audio engine receives the same event timeline. This preserves a useful audible instrument during protocol research without falsely claiming that every high-level WDV voice-record field has already been named.
- `ReferenceComparator` and `reference-captures/README.md` provide the real-hardware validation path: alignment, fitted gain, normalized correlation, RMS error, and peak error. Set `WAVE_REFERENCE_CAPTURE` when running tests to compare a capture.
- The live LCD scanout is 480 x 64 at one bit per pixel. Before genuine firmware produces video writes, the same framebuffer shows the sound and firmware diagnostic pages.

That makes this a useful, buildable hardware-model foundation rather than a false claim of bit-perfect emulation. A real-Wave capture set, factory wavetable data supplied by its owner, and oscillator-chip bus/audio measurements are still required before the words “sample accurate” or “circuit accurate” would be defensible.

## Build

Requirements: CMake 3.25+, Ninja, Xcode or Visual Studio 2022, and a C++20 compiler. JUCE 8.0.15 is fetched automatically.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

For private development only, place your own images in
`Firmware/wave_sys1_700`, an optional factory set at `wave.set`, and an optional
private upper-table header at `Firmware/private/FactoryUpperWavetables.h`.
Configure a separate build with `-DWAVE_EMBED_PRIVATE_ASSETS=ON` to embed them.
Do not distribute artifacts from that build. The installer explicitly disables
private embedding. Firmware-backed integration/editor tests are enabled only
when all three private ROM inputs are present and embedding is enabled.

The private build also provides `WavePerformanceBenchmark`. Its arguments are
block size, voice count, DSP resonance (0–1), and Eco mode (0 or 1):

```sh
build/Tests/WavePerformanceBenchmark 512 48 0.18 0
build/Tests/WavePerformanceBenchmark 512 48 0.18 1
```

Compare repeated Release runs on the same machine. The benchmark reports
whether firmware is loaded and how many voices actually rendered. The resonance
argument controls the standalone DSP fixture; the full-processor fixture uses
its selected Performance.

For the authenticated continuous-firmware core tests:

```sh
WAVE_FIRMWARE_DIR=/path/to/wave_sys1_700 ctest --test-dir build --output-on-failure
```

For a real-instrument comparison:

```sh
WAVE_REFERENCE_CAPTURE=/path/to/wave-c4.wav \
WAVE_FIRMWARE_DIR=/path/to/wave_sys1_700 \
ctest --test-dir build --output-on-failure
```

For validation against a legally obtained PPG Wave 2.2/2.3 wavetable EPROM image:

```sh
PPG_WAVETABLE_ROM=/path/to/ppg-eprom.bin \
ctest --test-dir build --output-on-failure
```

### macOS bundles and installer DMG

On macOS, the Standalone, Audio Unit, VST3, and AAX artifacts are written below
`build/WaveEmulation_artefacts`. macOS Release bundles are universal
`arm64`/`x86_64` binaries and are unsigned by default. To sign a local release, explicitly set
`WAVE_SIGN_RELEASE_ARTIFACTS=ON` and `WAVE_CODESIGN_IDENTITY` when configuring CMake. Distribution to normal Pro Tools systems additionally
requires PACE wrapping with a Wave-specific WCGUID; the CMake AAX bundle is
deliberately left unwrapped for use as the input to `wraptool`.

To build the distributable macOS installer and DMG, including PACE wrapping
with the Wave product WCGUID, Developer ID signing, notarization and stapling:

```sh
./Installer/build_installer.sh
```

Set `TEAM_ID`, `PACE_ACCOUNT`, and `PACE_WCGUID` for your release accounts.
You can store shell assignments in `Installer/.env.local`, which is loaded
automatically and ignored by Git. Use `${VARIABLE:-default}` assignments to
preserve command-line environment overrides. Keep passwords in the keychain.
The script uses the `wave-notary` keychain profile by default and accepts
environment overrides documented at its top. For a local packaging check that
does not contact PACE or Apple, use `SKIP_WRAP=1 SKIP_SIGN=1
SKIP_NOTARIZE=1 SKIP_DMG=1`.

### Windows x64 build

Install Visual Studio 2022 with Desktop development with C++ and CMake 3.25 or
newer. From a PowerShell terminal, build the standalone app and VST3 plugin:

```powershell
cmake -S . -B build-windows -G "Visual Studio 17 2022" -A x64 -DWAVE_EMBED_PRIVATE_ASSETS=OFF
cmake --build build-windows --config Release --parallel 3
ctest --test-dir build-windows -C Release --output-on-failure
```

The `.exe` and `.vst3` bundle are under
`build-windows/WaveEmulation_artefacts/Release`. Copy the VST3 bundle to
`C:\Program Files\Common Files\VST3` to use it in a VST3 host. The standalone
app can run directly from its build folder. Windows builds do not include AU or
AAX. The default Windows build links the C++ runtime statically, so these
downloads do not require a separate Visual C++ redistributable installer.

### Testing Windows without a Windows computer

For local testing on macOS, `cmake/WindowsLLVMToolchain.cmake` builds Windows
x64 binaries with `clang-cl`, `lld-link`, and Microsoft's headers/libraries
downloaded with [xwin](https://github.com/Jake-Shadle/xwin). Accept Microsoft's
license before downloading the SDK. JUCE 8 does not support MinGW compilation;
use the MSVC target provided by this toolchain. The build applies a small JUCE
VBlank-thread fallback so unsupported DXGI refresh calls in Wine still trigger
repaints and cannot deadlock window creation or teardown.

With CrossOver installed and a Windows 10 64-bit bottle named
`Wave Windows Testing`, configure a Release build:

```sh
cmake -S . -B build-windows-local/msvc -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/WindowsLLVMToolchain.cmake" \
  -DWAVE_WINDOWS_SDK_ROOT="$PWD/build-windows-local/sdk" \
  -DWAVE_CLANG_CL=/opt/homebrew/opt/llvm/bin/clang-cl \
  -DWAVE_LLD_LINK="$PWD/build-windows-local/toolchain/bin/lld-link" \
  -DCMAKE_CROSSCOMPILING_EMULATOR="/usr/bin/python3;$PWD/scripts/run-windows-crossover.py" \
  -DWAVE_EMBED_PRIVATE_ASSETS=OFF
cmake --build build-windows-local/msvc --parallel 4
WAVE_TEST_NATIVE_WINDOW=1 WAVE_TEST_ARTIFACT_DIR="$PWD/build-windows-local/evidence" \
  ctest --test-dir build-windows-local/msvc --output-on-failure
```

Supply the paths to your installed tools and xwin SDK. The SDK must include
the x64 release CRT. Cross builds omit the optional VST3 `moduleinfo.json`;
hosts can scan the plugin DLL directly. The runner uses CrossOver's configured
bottle directory, or `CX_BOTTLE_PATH`, and accepts an alternate bottle name
through `WAVE_CROSSOVER_BOTTLE`. It also runs the standalone executable:

```sh
WAVE_CROSSOVER_GUI=1 python3 scripts/run-windows-crossover.py \
  'build-windows-local/msvc/WaveEmulation_artefacts/Release/Standalone/Wave Emulation.exe'
```

Install Windows REAPER in the same bottle and copy the complete VST3 bundle to
that bottle's `C:\Program Files\Common Files\VST3` directory for DAW testing.
Load your locally obtained firmware through Wave's System menu. CrossOver
testing covers Windows binaries on Wine; native Windows audio/MIDI drivers
still require a Windows computer or VM.

`WAVE_TEST_NATIVE_WINDOW=1` also opens, recreates and closes a real editor window
in the public editor test. `WAVE_CROSSOVER_GUI=1` uses Windows ShellExecute for
interactive launches; omit it for test executables and build generators.

The **Public source build** GitHub Actions workflow uses a Windows x64 runner
to build the standalone app and VST3. It runs on pushes and pull requests and
can also be started from the workflow's **Run workflow** button. Its Windows
job checks:

- DSP/core tests, concurrent 68000 instances, and host state save/recall.
- Editor creation, rendering, zoom, keyboard visibility and reopening, using
  public assets without embedded firmware.
- The actual standalone executable opens a responsive window and closes cleanly.
- The built VST3 passes pluginval 1.0.4 at strictness level 5.

After a successful job, download **wave-emulation-windows-x64** from the run's
Artifacts section. The ZIP contains `Standalone/Wave Emulation.exe`, the
`VST3/Wave Emulation.vst3` bundle, licenses, and the source revision in `BUILD.txt`.
**wave-emulation-windows-test-results** contains editor PNGs, CTest logs and
the pluginval log, including available diagnostics from failed jobs.

For example, download a particular run's build from your Mac using GitHub CLI:

```sh
gh run download RUN_ID --repo mo0kid/wave --name wave-emulation-windows-x64
```

CI verifies Windows execution and plugin API behaviour without an audio device.
It does not establish audio-driver latency, MIDI hardware compatibility, DAW
compatibility or firmware-backed operation. Before calling the Windows build
a tested release, use a Windows VM or a Windows tester to check a VST3 DAW,
audio/MIDI devices, loading locally obtained firmware, and preset save/recall.
Review the uploaded editor PNGs for rendering problems as well.

## MIDI and wavetable loading

The Standalone automatically enables present and newly connected MIDI inputs. The on-screen keyboard plays MIDI notes 36–96, and the computer keyboard mapping `A W S E D F T G Y H U J K` plays C4 through C5. Host MIDI supports note velocity, sustain pedal, pitch bend, mod wheel, channel pressure, all-notes-off, and all-sound-off.

If a legal PPG EPROM or expanded wavetable image is placed beside the selected firmware files with a `.bin` or `.rom` extension, it is detected automatically. Names containing `ppg` or `wavetable` are tried first. The image path is retained in plug-in state without modifying the supplied SVG interface.

For the original seven-file PPG Wave 2.3 V6 EPROM archive, run `scripts/import-ppg-wave-23-v6-rom.sh`. It combines the physical `w23_64`/`w23_66` wavetable pair and the adjacent V6 program-ROM bytes exposed by the original out-of-range wavetable-13 references, then places the resulting private image beside the Waldorf firmware for automatic loading.

The imported V6 research image has SHA-256 `ff9393d3649a402eab07d2d763bbdb6161def53f74101b7cb08969d436b9741a`. Its byte-exact reconstructed 30 x 64 x 128 lower-table bank is regression-checked as `1e573f91e6dbd7b331e8287f6cf82b5b8c82c08f29c3f72cd3ce8386761cd105`.

## Genuine firmware

Either run `scripts/fetch-official-firmware.sh`, or download `System.zip` from Waldorf and select the directory containing both files from the plug-in. Known OS 1.700 hashes:

```text
w2sys.bin  4282457d9bf7d70da2e2aa4d6a1e69467c178d0d8d8be0a27f524ab8d62e3286
wdv.sys    bdf379b07785af313068191961ee5ef408e267740c1b598290732477bf9d3f68
```

## Modelled topology

The service schematics show a 16-voice WDV board with two eight-voice Waldorf ASICs, a 32 MHz board clock, PD508 multiplexed waveform conversion, AD7545 12-bit CV DACs, TL064 signal conditioning, individual CEM3387 voice cards, and a discrete LCD controller/RAM/shifter section. The engine mirrors those boundaries so measured circuit or chip data can replace individual approximations without rewriting the instrument. The LCD likewise exposes byte reads and writes rather than being a decorative text widget.

Waldorf and Wave are trademarks of their respective owner. This independent research sample is not affiliated with or endorsed by Waldorf Music.

## License and contributions

Copyright (c) 2026 Dave Whiting.

Project-authored source and artwork are licensed under **GPL-3.0-or-later**.
You may redistribute and modify them under version 3 of the GNU General Public
License, or any later version. This software comes without warranty. See
[LICENSE](LICENSE) for the full terms.

JUCE 8 is separately licensed under AGPLv3 or a commercial JUCE license.
GPLv3 section 13 permits combining GPLv3 and AGPLv3 code; the AGPL network
interaction requirements apply to the combination when using that licensing
route. See [third-party notices](THIRD_PARTY_NOTICES.md) and [LICENSES](LICENSES).
The project license does not relicense third-party firmware, the included PPG-derived sample data, or trademarks.

See [CONTRIBUTING.md](CONTRIBUTING.md) for development guidance and
[PUBLIC_RELEASE.md](PUBLIC_RELEASE.md) for preparing a clean public repository.
