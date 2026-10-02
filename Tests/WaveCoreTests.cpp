#include "Dsp/Cem3387.h"
#include "Dsp/PpgWaveRom.h"
#include "Dsp/ReferenceComparator.h"
#include "Dsp/WaldorfAsic.h"
#include "Dsp/WaldorfEngine.h"
#include "Dsp/WdvEnvelope.h"
#include "Dsp/WaveEnvelope.h"
#include "Dsp/WaveLfo.h"
#include "Dsp/DspMath.h"
#include "Firmware/DosFloppyImage.h"
#include "Firmware/FirmwareBundle.h"
#include "Firmware/Dp8473.h"
#include "Firmware/M68000.h"
#include "Firmware/MasterFirmwareRuntime.h"
#include "Firmware/Via6522.h"
#include "Firmware/VoiceFirmwareRuntime.h"
#include "Firmware/VoiceBoardProtocol.h"
#include "PanelWiring.h"
#include "Presets/WaveFactorySet.h"
#include "UI/WaveLcdModel.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_cryptography/juce_cryptography.h>

#include <cmath>
#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <vector>

namespace
{
void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void testDosFloppyImageCreation()
{
    const auto temporaryDirectory = juce::File::getSpecialLocation(
                                        juce::File::tempDirectory)
                                        .getNonexistentChildFile(
                                            "wave-dd-image-test", {}, true);
    require(temporaryDirectory.createDirectory().wasOk(),
            "Could not create the temporary DD-image test directory");
    const auto setupFile = temporaryDirectory.getChildFile("Factory Bank.set");
    const auto imageFile = temporaryDirectory.getChildFile("Factory Bank.img");
    const auto blankImageFile = temporaryDirectory.getChildFile("Blank Disk.img");
    std::vector<uint8_t> setup(503419u);
    for (size_t index = 0; index < setup.size(); ++index)
        setup[index] = static_cast<uint8_t>((index * 37u + 11u) & 0xffu);
    require(setupFile.replaceWithData(setup.data(), setup.size()),
            "Could not create the temporary Wave Setup");

    require(wave::firmware::DosFloppyImage::createEmpty(blankImageFile).wasOk(),
            "Could not create a blank canonical DD Wave disk image");
    juce::MemoryBlock blankImage;
    require(blankImageFile.loadFileAsData(blankImage)
                && blankImage.getSize() == 720u * 1024u,
            "Blank Wave disk is not exactly 720 KB");
    const auto* blankBytes = static_cast<const uint8_t*>(blankImage.getData());
    require(blankBytes[510] == 0x55u && blankBytes[511] == 0xaau
                && blankBytes[512] == 0xf9u && blankBytes[513] == 0xffu
                && blankBytes[514] == 0xffu && blankBytes[3584] == 0u,
            "Blank Wave disk is not an empty formatted FAT12 medium");

    const auto created = wave::firmware::DosFloppyImage::createWithWaveSetup(
        imageFile, setupFile);
    require(created.wasOk(), "Could not create a canonical DD Wave disk image");
    juce::MemoryBlock image;
    require(imageFile.loadFileAsData(image) && image.getSize() == 720u * 1024u,
            "Created Wave disk is not exactly 720 KB");
    const auto* bytes = static_cast<const uint8_t*>(image.getData());
    const auto get16 = [bytes](size_t offset) {
        return static_cast<uint16_t>(bytes[offset]
                                     | static_cast<uint16_t>(bytes[offset + 1u])
                                           << 8u);
    };
    require(get16(11) == 512 && bytes[13] == 2 && get16(19) == 1440
                && bytes[21] == 0xf9 && get16(22) == 3
                && get16(24) == 9 && get16(26) == 2,
            "Created Wave disk does not use canonical 720 KB DD geometry");
    require(std::equal(bytes + 512u, bytes + 2048u, bytes + 2048u),
            "Created Wave disk FAT copies differ");
    require(std::equal(bytes + 3584u, bytes + 3595u,
                       reinterpret_cast<const uint8_t*>("FACTORYBSET")),
            "Created Wave disk has an incorrect DOS 8.3 directory name");
    require(get16(3584u + 26u) == 2,
            "Created Wave Setup does not start at FAT cluster 2");
    const auto fileSize = static_cast<uint32_t>(bytes[3584u + 28u])
                          | static_cast<uint32_t>(bytes[3584u + 29u]) << 8u
                          | static_cast<uint32_t>(bytes[3584u + 30u]) << 16u
                          | static_cast<uint32_t>(bytes[3584u + 31u]) << 24u;
    require(fileSize == setup.size(),
            "Created Wave disk directory records the wrong Setup size");
    require(std::equal(setup.begin(), setup.end(), bytes + 7168u),
            "Created Wave disk does not preserve the Setup byte-for-byte");
    wave::firmware::DosFloppyImage::SetupFile extracted;
    require(wave::firmware::DosFloppyImage::readWaveSetup(imageFile, extracted).wasOk()
                && extracted.data.getSize() == setup.size()
                && std::equal(setup.begin(), setup.end(),
                              static_cast<const uint8_t*>(extracted.data.getData())),
            "Wave disk reader did not recover the created Setup byte-for-byte");
    require(temporaryDirectory.deleteRecursively(),
            "Could not remove the temporary DD-image test directory");
}

void testDp8473MountedDiskImage()
{
    const auto temporary = juce::File::getSpecialLocation(juce::File::tempDirectory)
                               .getNonexistentChildFile("wave-dp8473-test", ".img", false);
    std::vector<uint8_t> image(720u * 1024u, 0);
    image[11] = 0x00;
    image[12] = 0x02; // 512-byte sectors.
    image[19] = 0xa0;
    image[20] = 0x05; // 1440 sectors.
    image[24] = 9;
    image[26] = 2;
    std::fill_n(image.begin() + 512, 512, 0x5a);
    require(temporary.replaceWithData(image.data(), image.size()),
            "Could not create the temporary DP8473 disk image");

    wave::firmware::Dp8473 controller;
    const auto mounted = controller.mount(temporary);
    require(mounted.wasOk() && controller.isMounted(),
            "DP8473 rejected a valid 720 KB DOS image");
    require((controller.read(0xa8000f) & 0x80u) == 0,
            "DP8473 left the disk-change input asserted after insertion");
    controller.write(0xa80005, 0x1c); // Drive 0, motor/DMA on, reset released.

    const auto send = [&controller](std::initializer_list<uint8_t> bytes) {
        for (const auto byte : bytes)
            controller.write(0xa8000b, byte);
    };
    send({ 0x46, 0x00, 0x00, 0x00, 0x02, 0x02, 0x02, 0x2a, 0xff });
    require(controller.drqAsserted(), "DP8473 did not assert DRQ for Read Data");
    for (int byte = 0; byte < 512; ++byte)
        require(controller.read(0xa8001b) == 0x5a,
                "DP8473 returned incorrect mounted-sector data");
    (void) controller.read(0xa8002b); // Wave GAL terminal-count alias.
    require(controller.interruptPending(),
            "DP8473 did not complete Read Data after terminal count");
    for (int byte = 0; byte < 7; ++byte)
        (void) controller.read(0xa8000b);

    send({ 0x45, 0x00, 0x00, 0x00, 0x03, 0x02, 0x03, 0x2a, 0xff });
    require(controller.drqAsserted(), "DP8473 did not assert DRQ for Write Data");
    for (int byte = 0; byte < 512; ++byte)
        controller.write(0xa8001b, 0xa5);
    (void) controller.read(0xa8002b);
    for (int byte = 0; byte < 7; ++byte)
        (void) controller.read(0xa8000b);
    require(controller.isDirty() && controller.flush().wasOk(),
            "DP8473 did not persist a written sector");

    juce::MemoryBlock persisted;
    require(temporary.loadFileAsData(persisted) && persisted.getSize() == image.size(),
            "Could not reload the DP8473 disk image");
    const auto* persistedBytes = static_cast<const uint8_t*>(persisted.getData());
    require(std::all_of(persistedBytes + 1024, persistedBytes + 1536,
                        [](uint8_t value) { return value == 0xa5; }),
            "DP8473 sector writes were not saved at the correct CHS offset");
    // Reopening the same path must flush pending writes before reading it.
    send({ 0x45, 0x00, 0x00, 0x00, 0x04, 0x02, 0x04, 0x2a, 0xff });
    for (int byte = 0; byte < 512; ++byte)
        controller.write(0xa8001b, 0x3c);
    (void) controller.read(0xa8002b);
    for (int byte = 0; byte < 7; ++byte)
        (void) controller.read(0xa8000b);
    require(controller.isDirty() && controller.mount(temporary).wasOk(),
            "DP8473 could not reopen its dirty mounted image");
    controller.write(0xa80005, 0x1c);
    send({ 0x46, 0x00, 0x00, 0x00, 0x04, 0x02, 0x04, 0x2a, 0xff });
    for (int byte = 0; byte < 512; ++byte)
        require(controller.read(0xa8001b) == 0x3c,
                "Reopening the mounted image discarded pending sector changes");
    (void) controller.read(0xa8002b);
    for (int byte = 0; byte < 7; ++byte)
        (void) controller.read(0xa8000b);
    require(controller.eject().wasOk(), "DP8473 could not eject its mounted image");
    require((controller.read(0xa8000f) & 0x80u) != 0,
            "DP8473 did not report an empty drive after ejecting its image");
    require(temporary.deleteFile(), "Could not remove the temporary DP8473 image");
}

void testCutoffControlLaw()
{
    require(std::abs(wave::parameters::cutoffFrequencyForStep(0.0f) - 20.0f) < 1.0e-5f
                && std::abs(wave::parameters::cutoffFrequencyForStep(12.0f) - 40.0f) < 1.0e-4f
                && std::abs(wave::parameters::cutoffFrequencyForStep(60.0f) - 640.0f) < 1.0e-3f,
            "Cutoff control does not advance by one octave per twelve Wave steps");
    for (const auto step : { 0.0f, 31.75f, 63.5f, 95.25f, 127.0f })
        require(std::abs(wave::parameters::cutoffStepForFrequency(
                             wave::parameters::cutoffFrequencyForStep(step)) - step)
                    < 1.0e-3f,
                "Cutoff frequency and panel-control mappings are not inverse");
    require(wave::parameters::cutoffFrequencyForStep(127.0f) > 30000.0f,
            "Cutoff control no longer reaches the CEM3387's full-open range");
}

void testQuickEditFastAccessControls()
{
    wave::parameters::Snapshot sound;
    sound.attackSeconds = 0.1f;
    sound.filterAttackSeconds = 0.2f;
    sound.decaySeconds = 0.3f;
    sound.filterDecaySeconds = 0.4f;
    sound.sustainLevel = 0.4f;
    sound.filterSustainLevel = 0.5f;
    sound.releaseSeconds = 0.5f;
    sound.filterReleaseSeconds = 0.6f;
    sound.waveEnvelopeKeyOffPoint = 2;
    sound.waveEnvelopeTimes = { 10.0f, 20.0f, 30.0f, 40.0f,
                                50.0f, 60.0f, 70.0f, 80.0f };
    sound.waveEnvelopeLevels[2] = 48.0f;
    sound.freeEnvelopeTimes = { 10.0f, 20.0f, 30.0f, 40.0f };
    sound.freeEnvelopeLevels[2] = 10.0f;
    sound.wavePosition = 20.0f;
    sound.wavePosition2 = 24.0f;
    sound.cutoffHz = wave::parameters::cutoffFrequencyForStep(60.0f);
    sound.resonanceAmount = 0.3f;
    sound.waveScan = 20.0f;
    sound.waveScan2 = -16.0f;
    sound.modulationRoutes[wave::parameters::osc1PitchMod1].amount = 12.0f;
    sound.modulationRoutes[wave::parameters::wave1Mod1].amount = 10.0f;
    sound.quickEditAmounts = { 0.5f, 0.5f, 0.5f, 0.5f,
                               0.5f, 0.5f, 0.5f, 0.5f };

    wave::parameters::applyQuickEdit(sound);
    require(sound.attackSeconds > 0.1f && sound.filterAttackSeconds > 0.2f
                && sound.waveEnvelopeTimes[0] > 10.0f
                && sound.freeEnvelopeTimes[0] > 10.0f,
            "Quick Attack did not control all four envelope families");
    require(sound.decaySeconds > 0.3f && sound.filterDecaySeconds > 0.4f
                && sound.waveEnvelopeTimes[1] > 20.0f
                && sound.freeEnvelopeTimes[1] > 20.0f,
            "Quick Decay did not control all four envelope families");
    require(sound.sustainLevel > 0.4f && sound.filterSustainLevel > 0.5f
                && sound.waveEnvelopeLevels[2] > 48.0f
                && sound.freeEnvelopeLevels[2] > 10.0f,
            "Quick Sustain did not control all four envelope families");
    require(sound.releaseSeconds > 0.5f && sound.filterReleaseSeconds > 0.6f
                && sound.waveEnvelopeTimes[3] > 40.0f
                && sound.freeEnvelopeTimes[3] > 40.0f,
            "Quick Release did not control all four envelope families");
    require(sound.modulationRoutes[wave::parameters::osc1PitchMod1].amount > 12.0f,
            "Quick Pitch Mod did not scale oscillator modulation");
    require(sound.wavePosition > 20.0f && sound.wavePosition2 > 24.0f
                && sound.cutoffHz > wave::parameters::cutoffFrequencyForStep(60.0f)
                && sound.resonanceAmount > 0.3f,
            "Quick Timbre did not move both Waves and the filter");
    require(sound.modulationRoutes[wave::parameters::wave1Mod1].amount > 10.0f,
            "Quick Timbre Mod did not scale Wave/filter modulation");
    require(sound.waveScan > 20.0f && sound.waveScan2 < -16.0f,
            "Quick Wavescan did not scale both Wave-envelope amounts");
}

void testWavetableQuantisation()
{
    wave::dsp::WavetableBank bank;
    // Authentic generated tables use the complete signed-byte range,
    // including -128; the oscillator normalises that value by 128 below.

    const auto exact = bank.sample(7, 17.0f, 47.0 / 128.0, false);
    const auto raw = static_cast<float>(bank.rawSample(7, 17, 47)) / 128.0f;
    require(std::abs(exact - raw) < 1.0e-7f,
            "Oscillator-chip proxy lookup is not exact at integer coordinates");

    auto changingSamples = 0;
    auto absoluteDifference = 0;
    for (int sample = 0; sample < wave::dsp::WavetableBank::samplesPerWave; ++sample)
    {
        const auto difference = std::abs(
            static_cast<int>(bank.rawSample(32, 0, sample))
            - static_cast<int>(bank.rawSample(32, 31, sample)));
        changingSamples += difference != 0 ? 1 : 0;
        absoluteDifference += difference;
    }
    require(changingSamples > 100 && absoluteDifference > 1000,
            "Wavetable is effectively static across its sweep");

#if WAVE_HAS_PRIVATE_UPPER_TABLES
    const auto countCrossings = [&bank](int position) {
        auto crossings = 0;
        auto previous = bank.rawSample(32, position, 0);
        for (int sample = 1; sample < wave::dsp::WavetableBank::samplesPerWave; ++sample)
        {
            const auto current = bank.rawSample(32, position, sample);
            crossings += (previous < 0 && current >= 0) || (previous >= 0 && current < 0)
                             ? 1
                             : 0;
            previous = current;
        }
        return crossings;
    };
    require(countCrossings(60) > countCrossings(0),
            "SawSync 1 does not increase its slave ratio across the table");
#endif
}

void testAsicClockMixOverflowAndVcfSaturation()
{
    require(wave::dsp::OscillatorChipProxy::modelClockRate() == 250000.0,
            "ASIC proxy no longer runs at the original 250 kHz synthesis rate");

    using Mixer = wave::dsp::AsicOutputMixer;
    require(Mixer::mixOscillatorCodes(127, 127, 64, 65) == 127,
            "ES2 mixer wraps before the documented combined-level boundary");
    require(Mixer::mixOscillatorCodes(127, 127, 65, 65) == -128,
            "Positive ES2 numerical overflow clips instead of wrapping negative");
    require(Mixer::mixOscillatorCodes(-128, -128, 64, 64) == -128,
            "Negative ES2 mixer boundary changed its valid endpoint");
    require(Mixer::mixOscillatorCodes(-128, -128, 64, 65) == 127,
            "Negative ES2 numerical overflow clips instead of wrapping positive");

    const auto small = wave::dsp::Cem3387::saturateVcfInput(0.1f);
    const auto knee = wave::dsp::Cem3387::saturateVcfInput(0.7f);
    const auto full = wave::dsp::Cem3387::saturateVcfInput(1.0f);
    require(std::abs(small - 0.1f) < 0.001f
                && knee > 0.60f && knee < 0.67f
                && full > knee && full < 0.90f
                && std::abs(wave::dsp::Cem3387::saturateVcfInput(-0.7f) + knee)
                       < 1.0e-6f,
            "VCF input saturation is not a mild symmetric compression near 70% level");
}

void testAsicResampling()
{
    constexpr auto clockRate = wave::dsp::OscillatorChipProxy::modelClockRate();
    for (const auto hostRate : { 32000.0, 44100.0, 48000.0, 88200.0,
                                 96000.0, 192000.0, 250000.0, 384000.0 })
    {
        wave::dsp::AsicResampler resampler;
        resampler.prepare(hostRate);
        auto clockPhase = 0.0;
        auto tick = int64_t { 0 };
        const auto render = [&](double frequency, int samples) {
            auto energy = 0.0;
            for (int sample = 0; sample < samples; ++sample)
            {
                clockPhase += clockRate / hostRate;
                while (clockPhase >= 1.0)
                {
                    clockPhase -= 1.0;
                    ++tick;
                    resampler.push(static_cast<float>(std::cos(
                        juce::MathConstants<double>::twoPi * frequency
                        * static_cast<double>(tick) / clockRate)));
                }
                const auto value = resampler.read(clockPhase);
                energy += static_cast<double>(value) * value;
            }
            return std::sqrt(energy / samples);
        };
        const auto measure = [&](double frequency) {
            resampler.reset();
            clockPhase = 0.0;
            tick = 0;
            static_cast<void>(render(frequency, 4096));
            return render(frequency, 8192);
        };
        require(std::abs(measure(0.0) - 1.0) < 2.0e-6,
                "ASIC resampler changes DC gain");
        const auto passband = measure(0.40 * std::min(hostRate, clockRate));
        require(std::abs(passband - std::sqrt(0.5)) < 0.002,
                "ASIC resampler attenuates the audible passband");
        if (hostRate < clockRate)
            for (const auto fraction : { 0.501, 0.55, 0.73, 1.13, 1.91 })
            {
                const auto frequency = fraction * hostRate;
                if (frequency < clockRate * 0.5)
                    require(measure(frequency) < 0.0002,
                            "ASIC resampler folds ultrasonic harmonics into the host band");
            }

        // Check the actual fractional sampling time, including hosts faster
        // than the ASIC (some output frames contain no new internal tick).
        resampler.reset();
        clockPhase = 0.0;
        tick = 0;
        constexpr auto frequency = 1000.0;
        const auto delayTicks = 2.0 * std::ceil(24.0 * clockRate / std::min(hostRate, clockRate));
        static_cast<void>(render(frequency, 4096));
        for (int sample = 0; sample < 1024; ++sample)
        {
            static_cast<void>(render(frequency, 1));
            const auto expected = std::cos(juce::MathConstants<double>::twoPi * frequency
                                          * (static_cast<double>(tick) + clockPhase
                                             - delayTicks) / clockRate);
            require(std::abs(resampler.read(clockPhase) - expected) < 0.0001,
                    "ASIC resampling has incorrect fractional timing");
        }
        resampler.reset();
        require(resampler.read(0.37) == 0.0f,
                "ASIC resampler retains history after reset");
    }
}

void testHighRegisterOscillatorResampling()
{
    // A synthetic bright wave has a 10 kHz fundamental and a 30 kHz third
    // harmonic. Point sampling at 48 kHz folds the third down to 18 kHz.
    using Bank = wave::dsp::WavetableBank;
    std::vector<int8_t> rom(static_cast<size_t>(Bank::factoryTableCount)
                            * Bank::wavesPerTable * Bank::samplesPerWave);
    for (size_t sample = 0; sample < rom.size(); ++sample)
    {
        const auto phase = juce::MathConstants<double>::twoPi
                           * static_cast<double>(sample % Bank::samplesPerWave)
                           / Bank::samplesPerWave;
        rom[sample] = static_cast<int8_t>(std::lround(48.0 * (std::cos(phase)
                                                            + std::cos(3.0 * phase))));
    }
    Bank bank;
    require(bank.loadSigned8BitRom(rom.data(), rom.size()),
            "Synthetic high-register wavetable did not load");
    wave::dsp::OscillatorChipProxy oscillator;
    oscillator.prepare(48000.0);
    oscillator.setFrequency(10000.0f);
    for (int sample = 0; sample < 4096; ++sample)
        static_cast<void>(oscillator.process(bank, 0, 0.0f));
    std::array<double, 2> real {};
    std::array<double, 2> imaginary {};
    constexpr std::array<double, 2> frequencies { 10000.0, 18000.0 };
    constexpr int sampleCount = 4800;
    for (int sample = 0; sample < sampleCount; ++sample)
    {
        const auto output = oscillator.process(bank, 0, 0.0f);
        for (size_t bin = 0; bin < frequencies.size(); ++bin)
        {
            const auto angle = juce::MathConstants<double>::twoPi * frequencies[bin]
                               * sample / 48000.0;
            real[bin] += output * std::cos(angle);
            imaginary[bin] += output * std::sin(angle);
        }
    }
    const auto fundamental = 2.0 * std::hypot(real[0], imaginary[0]) / sampleCount;
    const auto alias = 2.0 * std::hypot(real[1], imaginary[1]) / sampleCount;
    require(fundamental > 0.35 && fundamental < 0.40,
            "High-register oscillator loses its in-band fundamental");
    require(alias < 0.0001,
            "High-register oscillator aliases its ultrasonic third harmonic");
}

void testWaveEnvelopeTraversal()
{
    wave::parameters::Snapshot parameters;
    parameters.waveEnvelopeTimes.fill(0.0f);
    parameters.waveEnvelopeLevels.fill(0.0f);
    parameters.waveEnvelopeTimes[0] = 8.0f;
    parameters.waveEnvelopeLevels[0] = 127.0f;
    parameters.waveEnvelopeTimes[1] = 8.0f;
    parameters.waveEnvelopeLevels[1] = 0.0f;
    parameters.waveEnvelopeKeyOffPoint = 1;

    wave::dsp::WaveEnvelope envelope;
    envelope.prepare(1000.0);
    envelope.noteOn();
    const auto first = envelope.process(parameters);
    auto peak = first;
    for (int sample = 0; sample < 30; ++sample)
        peak = juce::jmax(peak, envelope.process(parameters));
    require(peak > first && peak > 0.9f,
            "Eight-stage Wave envelope did not traverse its first level");

    envelope.noteOff();
    for (int sample = 0; sample < 30; ++sample)
        static_cast<void>(envelope.process(parameters));
    require(envelope.currentValue() < 0.1f,
            "Wave envelope did not traverse its post-key-off segment");

    parameters.waveEnvelopeLoop = true;
    parameters.waveEnvelopeLoopStartPoint = 0;
    envelope.noteOn();
    auto minimum = 1.0f;
    auto maximum = 0.0f;
    for (int sample = 0; sample < 80; ++sample)
    {
        const auto value = envelope.process(parameters);
        minimum = juce::jmin(minimum, value);
        maximum = juce::jmax(maximum, value);
    }
    require(minimum < 0.1f && maximum > 0.9f,
            "Wave envelope sustain loop did not revisit its wavetable range");
}

void testWaveLfo()
{
    require(std::abs(wave::dsp::WaveLfo::rateHz(0.0f) - 0.09) < 1.0e-9
                && std::abs(wave::dsp::WaveLfo::rateHz(127.0f) - 24.0) < 1.0e-9,
            "Wave LFO rate endpoints are incorrect");

    wave::dsp::WaveLfo lfo;
    lfo.prepare(1000.0, 0x12345678u);
    lfo.noteOn(2, 0.0f);
    auto minimum = 1.0f;
    auto maximum = -1.0f;
    for (int sample = 0; sample < 1000; ++sample)
    {
        const auto value = lfo.process(90.0f, 0, 0.0f, 0);
        minimum = juce::jmin(minimum, value);
        maximum = juce::jmax(maximum, value);
    }
    require(minimum < -0.95f && maximum > 0.95f,
            "Wave sine LFO did not traverse its bipolar range");

    lfo.noteOn(2, 90.0f);
    const auto retriggered = lfo.process(40.0f, 5, 0.0f, 0);
    for (int sample = 0; sample < 100; ++sample)
        require(std::abs(lfo.process(40.0f, 5, 0.0f, 0) - retriggered) < 1.0e-7f,
                "Wave sample-and-hold LFO changed within a cycle");
}

void testDspMathTables()
{
    for (int note = 0; note < 128; ++note)
        require(std::abs(wave::dsp::math::midiFrequency(note)
                         - static_cast<float>(juce::MidiMessage::getMidiNoteInHertz(note)))
                    <= wave::dsp::math::midiFrequency(note) * 1.0e-6f,
                "MIDI pitch lookup table exceeded its error bound");
}

void testPerformanceTuningTables()
{
    wave::dsp::WaldorfEngine engine;
    engine.prepare(48000.0, 32);

    wave::dsp::WaldorfEngine::PerformanceSnapshot performance;
    auto& layer = performance.layers[0];
    layer.enabled = true;
    layer.source = 2;
    layer.sound.attackSeconds = 0.001f;

    juce::AudioBuffer<float> audio(2, 32);
    juce::MidiBuffer noteOn;
    noteOn.addEvent(juce::MidiMessage::noteOn(1, 60, 0.9f), 0);
    engine.render(audio, noteOn, performance);

    const auto heldPitch = [&engine](int triggerNote) {
        for (const auto& state : engine.voiceStates())
            if (state.active && state.keyDown && state.triggerNote == triggerNote)
                return state.glidePitch;
        throw std::runtime_error("Expected tuning-table test voice is not active");
    };

    require(std::abs(heldPitch(60) - 60.0f) < 1.0e-5f,
            "Linear+ temperament changed equal-tempered pitch");

    // A live Instrument edit must reach already sounding voices. Linear- is
    // mirrored around MIDI note 64, so note 60 becomes note 68.
    layer.tuningTable = 3;
    audio.clear();
    engine.render(audio, {}, performance);
    require(std::abs(heldPitch(60) - 68.0f) < 1.0e-5f,
            "Instrument TuneTable changed state but not oscillator pitch");

    // Native SET user-table data consists of note/detune pairs. Install a
    // deliberately conspicuous G+25c mapping for middle C in User table 1.
    juce::MemoryBlock setImage(0x4327cu, true);
    auto* bytes = static_cast<uint8_t*>(setImage.getData());
    for (size_t table = 0; table < 4u; ++table)
        for (size_t key = 0; key < 128u; ++key)
        {
            const auto offset = 0x42e7cu + table * 256u + key * 2u;
            bytes[offset] = static_cast<uint8_t>(key);
            bytes[offset + 1u] = 64u;
        }
    bytes[0x42e7cu + 60u * 2u] = 67u;
    bytes[0x42e7cu + 60u * 2u + 1u] = 89u;
    juce::ignoreUnused(engine.loadWaveSetUserTables(setImage));

    layer.tuningTable = 0;
    audio.clear();
    engine.render(audio, {}, performance);
    require(std::abs(heldPitch(60) - 67.25f) < 1.0e-5f,
            "Wave SET user Tuning Table note/detune pair was not decoded through Global");

    layer.tuningTable = 8;
    audio.clear();
    engine.render(audio, {}, performance);
    require(std::abs(heldPitch(60) - 67.25f) < 1.0e-5f,
            "Wave SET User table 1 could not be selected directly");

    // HMT retunes a held chord as notes arrive. The oscillator must keep that
    // pitch after the next voice-board control update, including when changing
    // back to HMT while the chord is already sounding.
    wave::dsp::WaldorfEngine hmtEngine;
    hmtEngine.prepare(48000.0, 32);
    wave::dsp::WaldorfEngine::PerformanceSnapshot hmtPerformance;
    auto& hmtLayer = hmtPerformance.layers[0];
    hmtLayer.enabled = true;
    hmtLayer.source = 2;
    hmtLayer.tuningTable = 2;
    hmtLayer.sound.attackSeconds = 0.001f;
    hmtLayer.sound.detuneCents = 0.0f;

    const auto playHmtNote = [&](int midiNote) {
        juce::MidiBuffer event;
        event.addEvent(juce::MidiMessage::noteOn(1, midiNote, 0.9f), 0);
        audio.clear();
        hmtEngine.render(audio, event, hmtPerformance);
    };
    const auto oscillatorPitch = [&](int triggerNote) {
        for (const auto& state : hmtEngine.voiceStates())
            if (state.active && state.keyDown && state.triggerNote == triggerNote)
                return state.oscillator1FrequencyHz;
        throw std::runtime_error("Expected HMT test voice is not active");
    };
    const auto frequencyForNote = [](float midiNote) {
        return 440.0f * std::exp2((midiNote - 69.0f) / 12.0f);
    };
    const auto requireOscillatorPitch = [&](int triggerNote, float tunedNote,
                                            const char* message) {
        const auto expected = frequencyForNote(tunedNote);
        require(std::abs(oscillatorPitch(triggerNote) - expected) < expected * 1.0e-4f,
                message);
    };

    playHmtNote(60);
    playHmtNote(64);
    audio.clear();
    hmtEngine.render(audio, {}, hmtPerformance);
    requireOscillatorPitch(64, 63.86314f,
                           "New HMT chord note reverted to equal temperament");

    hmtLayer.tuningTable = 1;
    audio.clear();
    hmtEngine.render(audio, {}, hmtPerformance);
    requireOscillatorPitch(64, 64.0f,
                           "Changing from HMT did not retune a held note");

    hmtLayer.tuningTable = 2;
    audio.clear();
    hmtEngine.render(audio, {}, hmtPerformance);
    requireOscillatorPitch(64, 63.86314f,
                           "Switching to HMT retuned only until the next control update");

    // A held note started in Linear- can be dozens of semitones below its HMT
    // pitch. Retuning the voice must also retune its layer glide history, or
    // every later note with Glide enabled starts down at the old pitch.
    wave::dsp::WaldorfEngine glideEngine;
    glideEngine.prepare(48000.0, 32);
    auto glidePerformance = hmtPerformance;
    auto& glideLayer = glidePerformance.layers[0];
    glideLayer.tuningTable = 3;
    glideLayer.sound.glideEnabled = true;
    glideLayer.sound.glideRateValue = 50.0f;
    juce::MidiBuffer highNote;
    highNote.addEvent(juce::MidiMessage::noteOn(1, 84, 0.9f), 0);
    glideEngine.render(audio, highNote, glidePerformance);
    glideLayer.tuningTable = 2;
    glideEngine.render(audio, {}, glidePerformance);
    juce::MidiBuffer nextNote;
    nextNote.addEvent(juce::MidiMessage::noteOn(1, 86, 0.9f), 0);
    glideEngine.render(audio, nextNote, glidePerformance);
    auto nextPitch = -1.0f;
    for (const auto& state : glideEngine.voiceStates())
        if (state.active && state.keyDown && state.triggerNote == 86)
            nextPitch = state.glidePitch;
    require(nextPitch > 83.0f && nextPitch < 86.1f,
            "Changing to HMT left later Glide notes at the previous low tuning");

    // With Glide disabled, changing tuning under a held key must not leave
    // subsequent notes at the pitch of the old table.
    wave::dsp::WaldorfEngine directEngine;
    directEngine.prepare(48000.0, 32);
    auto directPerformance = hmtPerformance;
    auto& directLayer = directPerformance.layers[0];
    directLayer.sound.glideEnabled = false;
    directLayer.sound.oscillatorOctaves[0] = -1;
    for (const auto table : { 3, 2, 1, 2, 3, 1 })
    {
        directLayer.tuningTable = table;
        juce::MidiBuffer heldNote;
        heldNote.addEvent(juce::MidiMessage::noteOn(1, 72, 0.9f), 0);
        directEngine.render(audio, heldNote, directPerformance);
        const auto nextTable = table == 3 ? 2 : 3;
        directLayer.tuningTable = nextTable;
        directEngine.render(audio, {}, directPerformance);
        juce::MidiBuffer releaseAndPlay;
        releaseAndPlay.addEvent(juce::MidiMessage::noteOff(1, 72), 0);
        releaseAndPlay.addEvent(juce::MidiMessage::noteOn(1, 60, 0.9f), 1);
        directEngine.render(audio, releaseAndPlay, directPerformance);
        auto freshFrequency = 0.0f;
        for (const auto& state : directEngine.voiceStates())
            if (state.active && state.keyDown && state.triggerNote == 60)
                freshFrequency = state.oscillator1FrequencyHz;
        const auto expectedFrequency
            = 0.5f * frequencyForNote(nextTable == 3 ? 68.0f : 60.0f);
        require(std::abs(freshFrequency - expectedFrequency)
                    < expectedFrequency * 1.0e-4f,
                "Changing a tuning table under a held key left a new note subsonic without Glide");
        juce::MidiBuffer release;
        release.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
        directEngine.render(audio, release, directPerformance);
    }

    wave::dsp::WaldorfEngine chordEngine;
    chordEngine.prepare(48000.0, 32);
    auto chordPerformance = directPerformance;
    auto& chordLayer = chordPerformance.layers[0];
    chordLayer.tuningTable = 1;
    chordLayer.sound.oscillatorSemitones[0] = 3.0f;
    juce::MidiBuffer firstKey;
    firstKey.addEvent(juce::MidiMessage::noteOn(1, 72, 0.9f), 0);
    chordEngine.render(audio, firstKey, chordPerformance);
    chordLayer.tuningTable = 2;
    chordEngine.render(audio, {}, chordPerformance);
    juce::MidiBuffer secondKey;
    secondKey.addEvent(juce::MidiMessage::noteOn(1, 60, 0.9f), 0);
    chordEngine.render(audio, secondKey, chordPerformance);
    chordEngine.render(audio, {}, chordPerformance);
    for (const auto& state : chordEngine.voiceStates())
    {
        if (!state.active || !state.keyDown)
            continue;
        const auto expected
            = frequencyForNote(static_cast<float>(state.triggerNote) - 9.0f);
        require(std::abs(state.oscillator1FrequencyHz - expected)
                    < expected * 1.0e-4f,
                "Second key pressed while holding HMT chord has incorrect pitch");
    }
}

void testFreeRunningEngineLfo()
{
    wave::dsp::WaldorfEngine engine;
    engine.prepare(1000.0, 16);
    wave::parameters::Snapshot parameters;
    parameters.lfos[0].rate = 127.0f;
    parameters.lfos[0].shape = 0;
    parameters.lfos[0].sync = 0;
    parameters.attackSeconds = 0.001f;

    juce::AudioBuffer<float> idle(2, 10);
    engine.render(idle, {}, parameters);

    juce::AudioBuffer<float> onset(2, 1);
    juce::MidiBuffer noteOn;
    noteOn.addEvent(juce::MidiMessage::noteOn(1, 60, 1.0f), 0);
    engine.render(onset, noteOn, parameters);
    require(engine.firstActiveLfoValue(0) > 0.9f,
            "Unsynchronised Wave LFO phase froze while no voice was sounding");
}

void testFactorySetWhenAvailable()
{
    const auto* path = std::getenv("WAVE_FACTORY_SET");
    if (path == nullptr)
        return;

    juce::MemoryBlock data;
    require(juce::File(path).loadFileAsData(data),
            "WAVE_FACTORY_SET could not be read");
    wave::presets::WaveFactorySet factory;
    const auto report = factory.load(data);
    require(report.validLayout && report.validSounds == 256
                && report.validPerformances == 256,
            "Factory SET native sound or performance banks are incomplete");
    require(report.sha256
                == "cdcd1a1882f3c0ca38752cde552dcebf7579be94deb46fdfd2b26fbccaba44c9",
            "Factory SET image hash changed unexpectedly");
    require(factory.soundName(0, 0) == "sitar"
                && factory.soundName(0, 1) == "DROOPOLYFLANGE"
                && factory.performanceName(0, 0) == "drooSyn 1 oo DN"
                && factory.performanceName(1, 0) == "WoodOrgan    WMF",
            "Factory SET bank addressing or fixed-width name decoding is incorrect");

    const auto soundDump = factory.soundDump(0, 1, 0);
    const auto performanceDump = factory.performanceDump(0, 0);
    require(soundDump.size() == 266 && performanceDump.size() == 521
                && soundDump.front() == 0xf0 && soundDump.back() == 0xf7
                && performanceDump.front() == 0xf0 && performanceDump.back() == 0xf7,
            "Factory records were not framed as Wave SysEx dumps");
    const auto checksum = [](const std::vector<uint8_t>& dump) {
        auto sum = 0u;
        for (size_t index = 5; index + 2 < dump.size(); ++index)
            sum += dump[index];
        return static_cast<uint8_t>(sum & 0x7fu);
    };
    require(checksum(soundDump) == soundDump[soundDump.size() - 2]
                && checksum(performanceDump) == performanceDump[performanceDump.size() - 2],
            "Factory-record Wave SysEx checksum is incorrect");

    wave::dsp::WavetableBank wavetableBank;
    require(wavetableBank.loadWaveSetUserTables(data.getData(), data.getSize())
                && wavetableBank.hasWaveSetUserTables(),
            "Factory SET user Wavetables were not decoded");

    // Sound A001 (used by Performance A030, Sitar) stores selector 95.  That
    // is user-table slot 32, named TSITAR3 in this SET, and maps to engine
    // table index 95. Its first 13 entries reference flat factory Wave
    // numbers $0507..$0513: table 20, positions 7..19.
    constexpr auto sitarTable = 95;
    for (int wave = 0; wave < 13; ++wave)
        for (int sample = 0; sample < wave::dsp::WavetableBank::samplesPerWave; ++sample)
            require(wavetableBank.rawSample(sitarTable, wave, sample)
                        == wavetableBank.rawSample(20, 7 + wave, sample),
                    "TSITAR3 factory-Wave reference was decoded incorrectly");

    constexpr size_t userWaveBankOffset = 0x45afc;
    const auto* setBytes = static_cast<const uint8_t*>(data.getData());
    for (int sample = 0; sample < 64; ++sample)
    {
        const auto stored = setBytes[userWaveBankOffset + 13u * 64u
                                     + static_cast<size_t>(sample)];
        require(wavetableBank.rawSample(sitarTable, 13, sample)
                    == static_cast<int8_t>(static_cast<int>(stored) - 128)
                    && wavetableBank.rawSample(sitarTable, 13, 127 - sample)
                           == static_cast<int8_t>(static_cast<int>(
                                                     static_cast<uint8_t>(~stored))
                                                 - 128),
                "TSITAR3 user Wave was not reconstructed from its SET half-wave");
    }
}

std::vector<uint8_t> makeSyntheticPpgRom()
{
    std::vector<uint8_t> rom(wave::dsp::PpgWaveRom::minimumImageBytes, 0);
    size_t cursor = 0;
    for (int table = 0; table < wave::dsp::PpgWaveRom::storedTableCount + 1; ++table)
    {
        rom[cursor++] = static_cast<uint8_t>(table);
        rom[cursor++] = 0;
        rom[cursor++] = 0;
        rom[cursor++] = 1;
        rom[cursor++] = 60;
    }

    const auto waveformOffset = wave::dsp::PpgWaveRom::tableDirectoryBytes;
    for (size_t sample = 0; sample < wave::dsp::PpgWaveRom::halfWaveSamples; ++sample)
    {
        rom[waveformOffset + sample] = 128;
        rom[waveformOffset + wave::dsp::PpgWaveRom::halfWaveSamples
            + sample] = 192;
    }
    return rom;
}

void testPpgRomDecoding()
{
    auto rom = makeSyntheticPpgRom();
    wave::dsp::WavetableBank bank;
    require(bank.loadPpgWaveRom(rom.data(), rom.size()),
            "Valid sparse PPG EPROM image was rejected");
    require(bank.importedTableCount() == 30 && bank.isExternalRomLoaded(),
            "PPG EPROM source metadata is incorrect");
    require(bank.rawSample(0, 0, 7) == 0,
            "PPG key waveform was not read from the half-wave bank");
    require(bank.rawSample(0, 30, 7) == 32,
            "PPG sparse-table interpolation is incorrect");
    require(bank.rawSample(0, 1, 7) == 2,
            "PPG firmware midpoint interpolation order is incorrect");
    require(bank.rawSample(0, 30, 120) == -33,
            "PPG half-wave mirroring or polarity is incorrect");
    require(bank.rawSample(28, 0, 0) == -64
                && bank.rawSample(29, 0, 67) == 32
                && bank.rawSample(29, 0, 68) == -32
                && bank.rawSample(28, 3, 11) == -51,
            "PPG firmware-generated tables 28/29 are incorrect");
    require(bank.rawSample(0, 60, 0) == -96
                && bank.rawSample(0, 60, 64) == 96
                && bank.rawSample(0, 61, 125) == -48
                && bank.rawSample(0, 61, 126) == 127
                && bank.rawSample(0, 62, 0) == -96
                && bank.rawSample(0, 62, 64) == 96
                && bank.rawSample(0, 63, 0) == -64
                && bank.rawSample(0, 63, 127) == 63,
            "Classic PPG firmware tail waves were not generated byte-exactly");

    wave::dsp::WavetableBank rejected;
    const auto before = rejected.rawSample(0, 0, 0);
    rom[4] = 59; // Terminal key must be slot 60.
    require(!rejected.loadPpgWaveRom(rom.data(), rom.size()),
            "Malformed PPG sparse table was accepted");
    require(!rejected.isExternalRomLoaded() && rejected.rawSample(0, 0, 0) == before,
            "Rejected PPG image changed the active wavetable bank");
}

void testBundledPpgWavetables()
{
    wave::dsp::WavetableBank bank;
    std::vector<int8_t> samples;
    samples.reserve(30 * 64 * 128);
    for (int table = 0; table < 30; ++table)
        for (int wave = 0; wave < 64; ++wave)
            for (int sample = 0; sample < 128; ++sample)
                samples.push_back(bank.rawSample(table, wave, sample));
    require(juce::SHA256(samples.data(), samples.size()).toHexString()
                == "1e573f91e6dbd7b331e8287f6cf82b5b8c82c08f29c3f72cd3ce8386761cd105",
            "Bundled PPG sample bank is missing or differs from the decoded reference");
}

void testFactoryUpperWavetableBank()
{
#if WAVE_HAS_PRIVATE_UPPER_TABLES
    wave::dsp::WavetableBank bank;
    uint64_t fingerprint = 1469598103934665603ull;
    for (int table = 30; table < wave::dsp::WavetableBank::factoryTableCount; ++table)
    {
        for (int wave = 0; wave < 61; ++wave)
        {
            for (int sample = 0; sample < 64; ++sample)
            {
                const auto unsignedHalfWave = static_cast<uint8_t>(
                    static_cast<int>(bank.rawSample(table, wave, sample)) + 128);
                fingerprint ^= unsignedHalfWave;
                fingerprint *= 1099511628211ull;
                require(bank.rawSample(table, wave, 127 - sample)
                            == static_cast<int8_t>(static_cast<int>(
                                                      static_cast<uint8_t>(~unsignedHalfWave))
                                                  - 128),
                        "Upper factory-bank half-wave symmetry is incorrect");
            }
        }
        require(bank.rawSample(table, 61, 0) == -96
                    && bank.rawSample(table, 62, 126) == 127
                    && bank.rawSample(table, 63, 64) == 96,
                "An upper factory table lost its standard terminal waves");
    }
    require(fingerprint == 0x8021f195e5862f4full,
            "Wave factory tables 31..64 are not the complete reference bank");
#endif
}

void testExpandedFirst32Loading()
{
    constexpr auto byteCount = static_cast<size_t>(32)
                               * wave::dsp::WavetableBank::wavesPerTable
                               * wave::dsp::WavetableBank::samplesPerWave;
    std::vector<int8_t> expanded(byteCount);
    for (size_t i = 0; i < expanded.size(); ++i)
        expanded[i] = static_cast<int8_t>(static_cast<int>(i % 255) - 127);

    wave::dsp::WavetableBank bank;
    const auto untouched = bank.rawSample(32, 4, 9);
    require(bank.loadRomImage(expanded.data(), expanded.size()),
            "Expanded first-32 PPG/Waldorf image was rejected");
    require(bank.importedTableCount() == 32
                && bank.rawSample(31, 63, 127) == expanded.back(),
            "Expanded first-32 image did not map table-major data exactly");
    require(bank.rawSample(32, 4, 9) == untouched,
            "First-32 loader overwrote Waldorf table slots 32-63");
}

void testUserPpgRomWhenAvailable()
{
    const auto* path = std::getenv("PPG_WAVETABLE_ROM");
    if (path == nullptr)
        return;

    juce::MemoryBlock image;
    require(juce::File(path).loadFileAsData(image),
            "PPG_WAVETABLE_ROM could not be read");
    wave::dsp::WavetableBank bank;
    require(bank.loadRomImage(image.getData(), image.getSize()),
            "PPG_WAVETABLE_ROM is not a supported raw or expanded image");
    require(bank.importedTableCount() >= 30,
            "PPG_WAVETABLE_ROM loaded fewer than the stored PPG table set");

    const auto sourceHash = juce::SHA256(image.getData(), image.getSize()).toHexString();
    if (sourceHash == "ff9393d3649a402eab07d2d763bbdb6161def53f74101b7cb08969d436b9741a")
    {
        constexpr auto reconstructedBytes = static_cast<size_t>(30)
                                            * wave::dsp::WavetableBank::wavesPerTable
                                            * wave::dsp::WavetableBank::samplesPerWave;
        juce::MemoryBlock reconstructed(reconstructedBytes);
        auto* destination = static_cast<int8_t*>(reconstructed.getData());
        size_t cursor = 0;
        for (int table = 0; table < 30; ++table)
            for (int wave = 0; wave < wave::dsp::WavetableBank::wavesPerTable; ++wave)
                for (int sample = 0; sample < wave::dsp::WavetableBank::samplesPerWave; ++sample)
                    destination[cursor++] = bank.rawSample(table, wave, sample);
        require(juce::SHA256(reconstructed.getData(), reconstructed.getSize()).toHexString()
                    == "1e573f91e6dbd7b331e8287f6cf82b5b8c82c08f29c3f72cd3ce8386761cd105",
                "PPG V6 reconstructed 30-table image changed unexpectedly");
    }
}

void testCemStability()
{
    wave::dsp::Cem3387 filter;
    filter.prepare(48000.0, 0.72f);
    filter.setControls(18500.0f, 1.0f, 18.0f, 0.0f, 1.0f);

    auto peak = 0.0f;
    for (int i = 0; i < 96000; ++i)
    {
        const auto input = i < 48000
            ? std::sin(juce::MathConstants<float>::twoPi * 3200.0f
                       * static_cast<float>(i) / 48000.0f)
            : 0.0f;
        const auto output = filter.process(input, 1.0f);
        require(std::isfinite(output.left) && std::isfinite(output.right),
                "CEM3387 model became non-finite");
        peak = juce::jmax(peak, std::abs(output.left), std::abs(output.right));
    }
    require(peak < 1.01f, "CEM3387 model exceeded its nonlinear output rail");
}

void testCemFilterResponse()
{
    const auto measure = [](float cutoff, float resonance) {
        wave::dsp::Cem3387 filter;
        filter.prepare(48000.0, 0.0f);
        filter.setControls(cutoff, resonance, 0.0f, 0.0f, 0.0f);
        double energy = 0.0;
        constexpr auto frequency = 4000.0;
        constexpr auto sampleRate = 48000.0;
        for (int sample = 0; sample < 24000; ++sample)
        {
            const auto input = 0.1f * std::sin(
                static_cast<float>(juce::MathConstants<double>::twoPi * frequency
                                   * static_cast<double>(sample) / sampleRate));
            const auto output = filter.process(input, 1.0f).left;
            if (sample >= 4000)
                energy += static_cast<double>(output) * output;
        }
        return std::sqrt(energy / 20000.0);
    };

    const auto closed = measure(250.0f, 0.0f);
    const auto open = measure(12000.0f, 0.0f);
    require(open > closed * 20.0,
            "CEM3387 cutoff control does not open its four-pole low-pass response");
}

void testMeasuredWaveResonancePassbandLoss()
{
    const auto measure = [](float resonance) {
        wave::dsp::Cem3387 filter;
        filter.prepare(48000.0, 0.0f);
        filter.setControls(20000.0f, resonance, 0.0f, 0.0f, 0.0f);
        double inPhase = 0.0;
        double quadrature = 0.0;
        constexpr auto frequency = 130.8128;
        for (int sample = 0; sample < 48000; ++sample)
        {
            const auto phase = juce::MathConstants<double>::twoPi
                               * frequency * static_cast<double>(sample) / 48000.0;
            const auto input = 0.02f * std::sin(static_cast<float>(phase));
            const auto output = filter.process(input, 1.0f).left;
            if (sample >= 12000)
            {
                const auto position = static_cast<double>(sample - 12000) / 35999.0;
                const auto window = 0.5 - 0.5 * std::cos(
                    juce::MathConstants<double>::twoPi * position);
                inPhase += window * static_cast<double>(output) * std::sin(phase);
                quadrature += window * static_cast<double>(output) * std::cos(phase);
            }
        }
        return 2.0 * std::sqrt(inPhase * inPhase + quadrature * quadrature)
               / 17999.5;
    };

    const auto unresonant = measure(0.0f);
    const auto lossDb = [unresonant, &measure](float resonance) {
        return juce::Decibels::gainToDecibels(
            static_cast<float>(measure(resonance) / unresonant));
    };
    const auto loss30 = lossDb(30.0f / 127.0f);
    const auto loss62 = lossDb(62.0f / 127.0f);
    const auto loss100 = lossDb(100.0f / 127.0f);
    const auto loss127 = lossDb(1.0f);
    require(loss30 < -3.0f && loss30 > -4.5f
                && loss62 < -4.7f && loss62 > -5.9f
                && loss100 < -5.4f && loss100 > -6.6f
                && loss127 < -5.8f && loss127 > -7.0f,
            "CEM resonance passband loss no longer matches the measured Wave sweep");
}

void testAllVoiceFiltersAreCalibrated()
{
    std::array<bool, 4096> usedCodes{};
    auto distinctCodes = 0;
    for (int voice = 0; voice < 48; ++voice)
    {
        const auto tolerance
            = static_cast<float>(((voice * 37 + 11) % 19) - 9) / 9.0f;
        wave::dsp::Cem3387 filter;
        filter.prepare(48000.0, tolerance);
        filter.setControls(1000.0f, 0.0f, 0.0f, 0.0f, 1.0f);

        const auto code = filter.cutoffCalibrationCode();
        require(code <= 0x0fffu,
                "A CEM3387 VCF calibration word exceeds the Wave's 12-bit table");
        if (!usedCodes[code])
        {
            usedCodes[code] = true;
            ++distinctCodes;
        }
        require(std::abs(filter.calibratedCutoffCvResidual())
                    <= (0.5f / 4095.0f + 1.0e-7f),
                "A voice-card VCF retained more than one half trim LSB of cutoff error");
    }
    require(distinctCodes >= 10,
            "The 48 VCFs were assigned one fabricated global calibration value");

    auto minimumResponse = std::numeric_limits<double>::max();
    auto maximumResponse = 0.0;
    for (int voice = 0; voice < 48; ++voice)
    {
        const auto tolerance
            = static_cast<float>(((voice * 37 + 11) % 19) - 9) / 9.0f;
        wave::dsp::Cem3387 filter;
        filter.prepare(48000.0, tolerance);
        filter.setControls(1800.0f, 0.35f, 0.0f, 0.0f, 0.18f);
        filter.setCutoffCalibrationCode(filter.cutoffCalibrationCode());
        double energy = 0.0;
        for (int sample = 0; sample < 24000; ++sample)
        {
            const auto input = 0.1f * std::sin(
                static_cast<float>(juce::MathConstants<double>::twoPi * 4000.0
                                   * static_cast<double>(sample) / 48000.0));
            const auto output = filter.process(input, 1.0f).left;
            if (sample >= 4000)
                energy += static_cast<double>(output) * output;
        }
        const auto response = std::sqrt(energy / 20000.0);
        minimumResponse = std::min(minimumResponse, response);
        maximumResponse = std::max(maximumResponse, response);
    }
    require(maximumResponse / minimumResponse < 1.01,
            "The 12-bit VCF calibration leaves excessive residual spread");

    minimumResponse = std::numeric_limits<double>::max();
    maximumResponse = 0.0;
    for (int voice = 0; voice < 48; ++voice)
    {
        const auto tolerance
            = static_cast<float>(((voice * 37 + 11) % 19) - 9) / 9.0f;
        wave::dsp::ReconstructionStage reconstruction;
        reconstruction.prepare(48000.0, tolerance);
        reconstruction.setAge(0.18f);
        double energy = 0.0;
        for (int sample = 0; sample < 24000; ++sample)
        {
            const auto input = 0.1f * std::sin(
                static_cast<float>(juce::MathConstants<double>::twoPi * 10000.0
                                   * static_cast<double>(sample) / 48000.0));
            const auto output = reconstruction.process(input);
            if (sample >= 4000)
                energy += static_cast<double>(output) * output;
        }
        const auto response = std::sqrt(energy / 20000.0);
        minimumResponse = std::min(minimumResponse, response);
        maximumResponse = std::max(maximumResponse, response);
    }
    require(maximumResponse / minimumResponse < 1.0001,
            "Fixed reconstruction filters retain an undocumented voice spread");
}

void testCemControlVoltageSettling()
{
    wave::dsp::Cem3387 circuit;
    circuit.prepare(48000.0, 0.0f);
    circuit.setControls(1000.0f, 0.0f, 0.0f, 0.0f, 0.0f);

    (void) circuit.process(0.0f, 1.0f);
    const auto firstSample = circuit.currentVcaCv();
    require(firstSample > 0.0f && firstSample < 0.1f,
            "CEM3387 VCA control still changes as an ideal instantaneous step");

    for (int sample = 1; sample < 48; ++sample)
        (void) circuit.process(0.0f, 1.0f);
    const auto afterOneMillisecond = circuit.currentVcaCv();
    require(afterOneMillisecond > 0.96f && afterOneMillisecond < 0.985f,
            "CEM3387 sample-and-hold settling is outside its lightly smoothed transition");

    for (int sample = 48; sample < 240; ++sample)
        (void) circuit.process(0.0f, 1.0f);
    require(circuit.currentVcaCv() > 0.999f,
            "CEM3387 control voltage does not settle before the next WDV update");
}

void testLiveCutoffUsesContinuousBaseControlVoltage()
{
    wave::dsp::WaldorfEngine engine;
    engine.prepare(48000.0, 512);

    wave::parameters::Snapshot parameters;
    parameters.attackSeconds = 0.001f;
    parameters.decaySeconds = 0.001f;
    parameters.sustainLevel = 1.0f;
    parameters.releaseSeconds = 0.1f;
    parameters.filterEnvelopeSemitones = 0.0f;
    parameters.cutoffHz = 200.0f;

    juce::AudioBuffer<float> audio(2, 512);
    juce::MidiBuffer noteOn;
    noteOn.addEvent(juce::MidiMessage::noteOn(1, 60, 1.0f), 0);
    engine.render(audio, noteOn, parameters);
    const auto activeCutoff = [&engine]
    {
        for (const auto& voice : engine.voiceStates())
            if (voice.active)
                return voice.cutoffHz;
        return -1.0f;
    };

    parameters.cutoffHz = 12800.0f;
    audio.setSize(2, 1, false, false, true);
    audio.clear();
    juce::MidiBuffer noMidi;
    engine.render(audio, noMidi, parameters);
    const auto firstStep = activeCutoff();
    require(firstStep >= 190.0f && firstStep < 260.0f,
            "A live Cutoff edit still reaches the filter as an instantaneous step");

    audio.setSize(2, 2400, false, false, true);
    audio.clear();
    engine.render(audio, noMidi, parameters);
    const auto settled = activeCutoff();
    require(settled > 9000.0f && settled < 12800.0f,
            "The reconstructed Cutoff control voltage does not glide to its target");
}

void testAsicHighpassResponse()
{
    const auto measure = [](float frequency) {
        wave::dsp::AsicHighpassFilter filter;
        filter.prepare(48000.0);
        double energy = 0.0;
        for (int sample = 0; sample < 24000; ++sample)
        {
            const auto input = 0.1f * std::sin(
                static_cast<float>(juce::MathConstants<double>::twoPi * frequency
                                   * static_cast<double>(sample) / 48000.0));
            const auto output = filter.process(input, 1200.0f);
            if (sample >= 4000)
                energy += static_cast<double>(output) * output;
        }
        return std::sqrt(energy / 20000.0);
    };

    require(measure(6000.0f) > measure(100.0f) * 30.0,
            "ASIC 12 dB high-pass does not reject frequencies below its cutoff");
}

void testSerialBandpassTopology()
{
    const auto measure = [](float frequency) {
        wave::dsp::AsicHighpassFilter highpass;
        wave::dsp::Cem3387 lowpass;
        highpass.prepare(48000.0);
        lowpass.prepare(48000.0, 0.0f);
        lowpass.setControls(2200.0f, 0.0f, 0.0f, 0.0f, 0.0f);
        double energy = 0.0;
        for (int sample = 0; sample < 24000; ++sample)
        {
            const auto input = 0.1f * std::sin(
                static_cast<float>(juce::MathConstants<double>::twoPi * frequency
                                   * static_cast<double>(sample) / 48000.0));
            const auto filtered = highpass.process(input, 500.0f);
            const auto output = lowpass.process(filtered, 1.0f).left;
            if (sample >= 4000)
                energy += static_cast<double>(output) * output;
        }
        return std::sqrt(energy / 20000.0);
    };

    const auto passband = measure(1000.0f);
    require(passband > measure(80.0f) * 12.0
                && passband > measure(9000.0f) * 12.0,
            "Serial ASIC high-pass and CEM low-pass do not form the Wave band-pass");
}

void testCemSelfOscillation()
{
    const auto tailLevel = [](float resonance) {
        wave::dsp::Cem3387 filter;
        filter.prepare(48000.0, 0.0f);
        filter.setControls(1000.0f, resonance, 0.0f, 0.0f, 0.0f);
        double energy = 0.0;
        for (int sample = 0; sample < 96000; ++sample)
        {
            const auto output = filter.process(sample == 0 ? 0.1f : 0.0f, 1.0f).left;
            if (sample >= 72000)
                energy += static_cast<double>(output) * output;
        }
        return std::sqrt(energy / 24000.0);
    };

    const auto unresonantTail = tailLevel(0.0f);
    const auto oscillatingTail = tailLevel(1.0f);
    require(oscillatingTail > unresonantTail * 20.0 && oscillatingTail > 1.0e-4,
            "Maximum CEM3387 resonance does not sustain oscillation after excitation");

    const auto oscillationFrequency = [](float cutoff) {
        wave::dsp::Cem3387 filter;
        filter.prepare(48000.0, 0.0f);
        filter.setControls(cutoff, 1.0f, 0.0f, 0.0f, 0.0f);
        auto previous = 0.0f;
        auto crossings = 0;
        for (int sample = 0; sample < 144000; ++sample)
        {
            const auto output
                = filter.process(sample == 0 ? 0.1f : 0.0f, 1.0f).left;
            if (sample >= 96000 && previous <= 0.0f && output > 0.0f)
                ++crossings;
            previous = output;
        }
        return static_cast<float>(crossings);
    };
    const auto atZero = oscillationFrequency(
        wave::parameters::cutoffFrequencyForStep(0.0f));
    const auto at62 = oscillationFrequency(
        wave::parameters::cutoffFrequencyForStep(62.0f));
    const auto at100 = oscillationFrequency(
        wave::parameters::cutoffFrequencyForStep(100.0f));
    require(std::abs(atZero - 28.0f) <= 3.0f
                && std::abs(at62 - 957.0f) <= 50.0f
                && std::abs(at100 - 7779.0f) <= 400.0f,
            "CEM self-oscillation tracking no longer matches the measured Wave cutoffs");
}

void testCemResonanceAcrossSampleRates()
{
    // A continuous analogue feedback loop must not start oscillating earlier
    // as its cutoff approaches the digital sample rate. Drive is an input
    // level control, so it must not change the unexcited loop's threshold.
    for (const auto cutoff : { 1000.0f, 6000.0f })
    {
        auto referenceFrequency = 0;
        for (const auto rate : { 44100.0, 48000.0, 96000.0 })
            for (const auto drive : { 0.0f, 18.0f })
            {
                const auto measure = [=](float resonance) {
                    wave::dsp::Cem3387 filter;
                    filter.prepare(rate, 0.0f);
                    filter.setControls(cutoff, resonance, drive, 0.0f, 0.0f);
                    double energy = 0.0;
                    auto crossings = 0;
                    auto previous = 0.0f;
                    for (int sample = 0; sample < static_cast<int>(rate * 2.0); ++sample)
                    {
                        const auto output = filter.process(sample == 0 ? 0.1f : 0.0f,
                                                           1.0f).left;
                        if (sample >= static_cast<int>(rate))
                        {
                            energy += static_cast<double>(output) * output;
                            if (previous <= 0.0f && output > 0.0f)
                                ++crossings;
                        }
                        previous = output;
                    }
                    return std::pair { std::sqrt(energy / rate), crossings };
                };
                const auto belowThreshold = measure(0.85f);
                require(belowThreshold.first < 1.0e-5,
                        "CEM resonance oscillates below threshold at high cutoff or drive");
                require(measure(0.95f).first < 1.0e-5,
                        "CEM resonance starts oscillating before the analogue threshold");
                const auto oscillating = measure(1.0f);
                require(oscillating.first > 0.01,
                        "CEM maximum resonance fails to oscillate across sample rates");
                if (referenceFrequency == 0)
                    referenceFrequency = oscillating.second;
                require(std::abs(static_cast<float>(oscillating.second)
                                 / static_cast<float>(referenceFrequency) - 1.0f) < 0.02f,
                        "CEM oscillation frequency shifts with sample rate or drive");
            }
    }
}

void testCemSmallSignalResonanceResponse()
{
    // Datasheet's classical four-pole application: at w = wc, the
    // open-loop response is -1/4. Negative feedback k therefore gives
    // |H(wc)| / |H(0)| = (1+k)/(4-k), independent of input makeup gain.
    constexpr auto resonance = 0.7f;
    constexpr auto feedback = 4.15f * resonance;
    constexpr auto expectedRatio = (1.0f + feedback) / (4.0f - feedback);
    for (const auto rate : { 44100.0, 48000.0, 96000.0 })
    {
        const auto measure = [=](double frequency) {
            wave::dsp::Cem3387 filter;
            filter.prepare(rate, 0.0f);
            filter.setControls(wave::parameters::cutoffFrequencyForStep(62.0f),
                               resonance, 0.0f, 0.0f, 0.0f);
            double energy = 0.0;
            for (int sample = 0; sample < static_cast<int>(rate); ++sample)
            {
                const auto input = 0.001f * static_cast<float>(std::sin(
                    juce::MathConstants<double>::twoPi * frequency * sample / rate));
                const auto output = filter.process(input, 1.0f).left;
                if (sample >= static_cast<int>(rate * 0.5))
                    energy += static_cast<double>(output) * output;
            }
            return std::sqrt(energy / (rate * 0.5));
        };
        const auto ratio = measure(957.0) / measure(100.0);
        require(std::abs(ratio / expectedRatio - 1.0) < 0.03,
                "CEM resonance peak does not match the analogue four-pole response");
    }
}

void testIndependentFilterEnvelope()
{
    wave::dsp::WaldorfEngine engine;
    engine.prepare(1000.0, 64);
    wave::parameters::Snapshot parameters;
    parameters.attackSeconds = 0.001f;
    parameters.sustainLevel = 1.0f;
    parameters.filterDelaySeconds = 0.020f;
    parameters.filterAttackSeconds = 0.010f;
    parameters.filterDecaySeconds = 0.001f;
    parameters.filterSustainLevel = 1.0f;

    juce::AudioBuffer<float> beforeDelay(2, 10);
    juce::MidiBuffer noteOn;
    noteOn.addEvent(juce::MidiMessage::noteOn(1, 60, 1.0f), 0);
    engine.render(beforeDelay, noteOn, parameters);
    require(engine.firstActiveFilterEnvelopeValue() == 0.0f,
            "Filter envelope ignored its factory delay stage");

    juce::AudioBuffer<float> afterDelay(2, 20);
    engine.render(afterDelay, {}, parameters);
    require(engine.firstActiveFilterEnvelopeValue() > 0.5f,
            "Filter envelope did not start independently of the amplifier envelope");
}

void testAmplifierEnvelopeStages()
{
    wave::dsp::WaldorfEngine engine;
    engine.prepare(1000.0, 128);
    wave::parameters::Snapshot parameters;
    parameters.attackSeconds = 0.050f;
    parameters.decaySeconds = 0.050f;
    parameters.sustainLevel = 0.4f;
    parameters.releaseSeconds = 0.050f;
    parameters.cutoffHz = 18000.0f;
    parameters.filterEnvelopeSemitones = 0.0f;

    juce::AudioBuffer<float> attack(2, 25);
    juce::MidiBuffer noteOn;
    noteOn.addEvent(juce::MidiMessage::noteOn(1, 60, 1.0f), 0);
    engine.render(attack, noteOn, parameters);
    const auto attackLevel = engine.firstActiveAmplifierEnvelopeValue();
    require(attackLevel > 0.35f && attackLevel < 0.65f,
            "VCA envelope did not traverse its attack stage");

    juce::AudioBuffer<float> decay(2, 600);
    engine.render(decay, {}, parameters);
    const auto sustainLevel = engine.firstActiveAmplifierEnvelopeValue();
    require(std::abs(sustainLevel - 0.4f) < 0.03f,
            "VCA envelope did not reach its programmed sustain level");

    juce::AudioBuffer<float> firstReleaseHalf(2, 25);
    juce::MidiBuffer noteOff;
    noteOff.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
    engine.render(firstReleaseHalf, noteOff, parameters);
    const auto releaseLevel = engine.firstActiveAmplifierEnvelopeValue();
    require(releaseLevel > 0.1f && releaseLevel < sustainLevel,
            "VCA envelope did not traverse its release stage");

    juce::AudioBuffer<float> releaseEnd(2, 500);
    engine.render(releaseEnd, {}, parameters);
    require(engine.activeVoiceCount() == 0
                && engine.firstActiveAmplifierEnvelopeValue() == 0.0f,
            "VCA envelope did not close the voice after release");
}

void testMeasuredFastAmplifierAttackScaling()
{
    const auto riseTimeMilliseconds = [](uint8_t rate) {
        wave::dsp::WdvEnvelope envelope;
        envelope.prepare(48000.0);
        wave::dsp::WdvEnvelope::Parameters parameters;
        parameters.attack
            = wave::dsp::WdvEnvelope::amplifierAttackTimeConstantForRate(rate);
        parameters.decay = 10.0f;
        parameters.sustain = 1.0f;
        parameters.release = 10.0f;
        parameters.useMeasuredAmplifierAttackScaling = true;
        envelope.setParameters(parameters);
        envelope.noteOn();

        auto tenPercentSample = -1;
        auto ninetyPercentSample = -1;
        for (auto sample = 0; sample < 2048 && ninetyPercentSample < 0; ++sample)
        {
            const auto level = envelope.getNextSample();
            const auto vcaGain = level * level * (3.0f - 2.0f * level);
            if (tenPercentSample < 0 && vcaGain >= 0.1f)
                tenPercentSample = sample;
            if (vcaGain >= 0.9f)
                ninetyPercentSample = sample;
        }
        require(tenPercentSample >= 0 && ninetyPercentSample >= tenPercentSample,
                "Fast amplifier attack did not traverse its measured range");
        return static_cast<float>(ninetyPercentSample - tenPercentSample)
               * 1000.0f / 48000.0f;
    };

    require(riseTimeMilliseconds(0) < 0.5f,
            "VCA attack zero lost the Wave's abrupt click-prone response");
    require(std::abs(riseTimeMilliseconds(1) - 5.0f) < 0.25f,
            "VCA attack one no longer matches the measured five-millisecond rise");
}

void testShortVcaReleaseDrainsAnalogueControl()
{
    wave::dsp::WaldorfEngine engine;
    engine.prepare(48000.0, 2048);
    wave::parameters::Snapshot parameters;
    parameters.attackSeconds = 0.001f;
    parameters.decaySeconds = 0.001f;
    parameters.sustainLevel = 1.0f;
    parameters.releaseSeconds = 0.001f;
    parameters.cutoffHz = 16000.0f;
    parameters.filterEnvelopeSemitones = 0.0f;
    parameters.driveDb = 0.0f;

    juce::AudioBuffer<float> held(2, 2048);
    juce::MidiBuffer noteOn;
    noteOn.addEvent(juce::MidiMessage::noteOn(1, 60, 1.0f), 0);
    engine.render(held, noteOn, parameters);

    juce::AudioBuffer<float> releaseStart(2, 64);
    juce::MidiBuffer noteOff;
    noteOff.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
    engine.render(releaseStart, noteOff, parameters);
    auto largestHeldStep = 0.0f;
    for (int sample = 1; sample < held.getNumSamples(); ++sample)
        largestHeldStep = juce::jmax(
            largestHeldStep,
            std::abs(held.getSample(0, sample) - held.getSample(0, sample - 1)));
    auto largestReleaseStep = std::abs(
        releaseStart.getSample(0, 0)
        - held.getSample(0, held.getNumSamples() - 1));
    for (int sample = 1; sample < releaseStart.getNumSamples(); ++sample)
        largestReleaseStep = juce::jmax(
            largestReleaseStep,
            std::abs(releaseStart.getSample(0, sample)
                     - releaseStart.getSample(0, sample - 1)));
    require(engine.activeVoiceCount() == 1
                && releaseStart.getMagnitude(0, 0, releaseStart.getNumSamples()) > 1.0e-6f,
            "Shortest VCA release bypassed the analogue hold-capacitor discharge");
    require(largestReleaseStep <= largestHeldStep * 1.25f + 1.0e-4f,
            "Shortest VCA release introduced a discontinuity larger than the waveform");

    juce::AudioBuffer<float> releaseEnd(2, 1024);
    engine.render(releaseEnd, {}, parameters);
    require(engine.activeVoiceCount() == 0,
            "Analogue VCA drain did not retire the released voice");
}

void testCentredVoiceHasNoArtificialPanSpread()
{
    wave::dsp::WaldorfEngine engine;
    engine.prepare(48000.0, 4096);
    wave::parameters::Snapshot parameters;
    parameters.panAmount = 0.0f;
    parameters.circuitAgeAmount = 1.0f;
    parameters.attackSeconds = 0.001f;
    parameters.filterEnvelopeSemitones = 0.0f;
    parameters.cutoffHz = 18000.0f;

    juce::AudioBuffer<float> audio(2, 4096);
    juce::MidiBuffer noteOn;
    noteOn.addEvent(juce::MidiMessage::noteOn(1, 60, 1.0f), 0);
    engine.render(audio, noteOn, parameters);
    const auto left = audio.getRMSLevel(0, 512, 3584);
    const auto right = audio.getRMSLevel(1, 512, 3584);
    require(std::abs(left - right) < juce::jmax(left, right) * 0.01f,
            "Per-voice circuit tolerance introduced an artificial stereo pan offset");
}

void testSampleAccurateMidiStart()
{
    wave::dsp::WaldorfEngine engine;
    engine.prepare(48000.0, 128);
    wave::parameters::Snapshot parameters;
    parameters.attackSeconds = 0.001f;
    parameters.cutoffHz = 18000.0f;
    parameters.filterEnvelopeSemitones = 0.0f;

    juce::AudioBuffer<float> audio(2, 128);
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, 60, 1.0f), 37);
    engine.render(audio, midi, parameters);

    require(audio.getMagnitude(0, 0, 37) < 1.0e-4f
                && audio.getMagnitude(1, 0, 37) < 1.0e-4f,
            "Idle analogue noise exceeded the calibrated pre-event floor");
    require(audio.getMagnitude(0, 37, 91) > 1.0e-6f,
            "Voice did not produce audio after the MIDI event offset");
}

void testAudibleKeyboardRange()
{
    wave::dsp::WaldorfEngine engine;
    engine.prepare(96000.0, 512);
    wave::parameters::Snapshot parameters;
    parameters.attackSeconds = 0.008f;
    parameters.cutoffHz = 8200.0f;
    parameters.filterEnvelopeSemitones = 28.0f;
    parameters.outputDb = -7.0f;

    for (const auto note : { 36, 60, 84 })
    {
        engine.reset();
        juce::AudioBuffer<float> audio(2, 512);
        auto peak = 0.0f;
        for (int block = 0; block < 6; ++block)
        {
            juce::MidiBuffer midi;
            if (block == 0)
                midi.addEvent(juce::MidiMessage::noteOn(1, note, 0.35f), 0);
            engine.render(audio, midi, parameters);
            peak = juce::jmax(peak, audio.getMagnitude(0, 0, audio.getNumSamples()),
                             audio.getMagnitude(1, 0, audio.getNumSamples()));
        }
        require(peak > 0.005f,
                "A playable keyboard note was attenuated below an audible output level");
    }
}

void testPerformanceMidi()
{
    wave::dsp::WaldorfEngine engine;
    engine.prepare(48000.0, 512);
    wave::parameters::Snapshot parameters;
    parameters.attackSeconds = 0.001f;
    parameters.releaseSeconds = 0.002f;
    parameters.cutoffHz = 16000.0f;
    parameters.filterEnvelopeSemitones = 0.0f;
    parameters.wavePosition = 0.0f;
    parameters.waveScan = 0.0f;
    parameters.modulationRoutes[wave::parameters::wave1Mod1]
        = { 22, 38, 63.0f };

    juce::AudioBuffer<float> audio(2, 512);
    juce::MidiBuffer noteOn;
    noteOn.addEvent(juce::MidiMessage::pitchWheel(1, 16383), 0);
    noteOn.addEvent(juce::MidiMessage::noteOn(1, 64, 0.9f), 1);
    noteOn.addEvent(juce::MidiMessage::controllerEvent(1, 1, 127), 2);
    noteOn.addEvent(juce::MidiMessage::channelPressureChange(1, 127), 3);
    engine.render(audio, noteOn, parameters);
    require(engine.activeVoiceCount() == 1,
            "Performance MIDI note did not allocate a voice");
    require(engine.firstActiveWavePosition() > 62.0f,
            "Full mod-wheel route did not traverse the full 64-position wavetable");
    require(std::abs(engine.getPitchBendSemitones() - 2.0f) < 0.001f,
            "Full-up pitch wheel did not produce the two-semitone bend range");
    require(audio.getMagnitude(0, 0, audio.getNumSamples()) > 1.0e-6f,
            "Performance controller sequence produced no audio");
    for (int channel = 0; channel < audio.getNumChannels(); ++channel)
        for (int sample = 0; sample < audio.getNumSamples(); ++sample)
            require(std::isfinite(audio.getSample(channel, sample)),
                    "Performance controller sequence produced non-finite audio");

    juce::MidiBuffer sustain;
    sustain.addEvent(juce::MidiMessage::controllerEvent(1, 64, 127), 0);
    sustain.addEvent(juce::MidiMessage::noteOff(1, 64), 1);
    engine.render(audio, sustain, parameters);
    require(engine.isSustainPedalDown() && engine.activeVoiceCount() == 1,
            "Sustain pedal did not hold a released key");

    juce::MidiBuffer pedalUp;
    pedalUp.addEvent(juce::MidiMessage::controllerEvent(1, 64, 0), 0);
    engine.render(audio, pedalUp, parameters);
    juce::AudioBuffer<float> releaseTail(2, 1024);
    engine.render(releaseTail, {}, parameters);
    require(!engine.isSustainPedalDown() && engine.activeVoiceCount() == 0,
            "Pedal release did not complete the held voice envelope");

    juce::MidiBuffer panic;
    panic.addEvent(juce::MidiMessage::noteOn(1, 67, 1.0f), 0);
    panic.addEvent(juce::MidiMessage::allSoundOff(1), 32);
    engine.render(audio, panic, parameters);
    require(engine.activeVoiceCount() == 0,
            "MIDI all-sound-off did not stop voices immediately");
}

void testWaveGlideModes()
{
    wave::dsp::WaldorfEngine engine;
    engine.prepare(48000.0, 512);
    wave::parameters::Snapshot sound;
    sound.glideEnabled = true;
    sound.glideTypeMode = 1;
    sound.glideRateValue = 50.0f;
    sound.glideTimeModeValue = 0;
    sound.glideRateModulationSource = 38;
    sound.glideRateModulationAmount = 0.0f;
    sound.attackSeconds = 0.001f;
    sound.cutoffHz = 18000.0f;
    sound.filterEnvelopeSemitones = 0.0f;

    juce::AudioBuffer<float> audio(2, 512);
    const auto send = [&](const juce::MidiMessage& message) {
        juce::MidiBuffer midi;
        midi.addEvent(message, 0);
        audio.clear();
        engine.render(audio, midi, sound);
    };
    const auto pitchFor = [&](int triggerNote) {
        for (const auto& voice : engine.voiceStates())
            if (voice.active && voice.triggerNote == triggerNote)
                return voice.glidePitch;
        return -1000.0f;
    };
    const auto heldPitchFor = [&](int triggerNote) {
        for (const auto& voice : engine.voiceStates())
            if (voice.active && voice.keyDown && voice.triggerNote == triggerNote)
                return voice.glidePitch;
        return -1000.0f;
    };

    send(juce::MidiMessage::noteOn(1, 60, 0.9f));
    send(juce::MidiMessage::noteOff(1, 60));
    send(juce::MidiMessage::noteOn(1, 72, 0.9f));
    require(pitchFor(72) > 59.9f && pitchFor(72) < 61.0f,
            "Portamento did not start the new voice at the preceding pitch");
    for (int block = 0; block < 48; ++block)
    {
        audio.clear();
        engine.render(audio, {}, sound);
    }
    require(pitchFor(72) > 60.0f && pitchFor(72) < 72.0f,
            "Portamento did not travel continuously towards its target");

    const auto pitchBeforeReferenceBlock = heldPitchFor(72);
    audio.clear();
    engine.render(audio, {}, sound);
    const auto pitchAfterReferenceBlock = heldPitchFor(72);
    const auto referenceAdvance
        = pitchAfterReferenceBlock - pitchBeforeReferenceBlock;
    require(referenceAdvance > 0.0f,
            "Portamento trajectory stopped before the retrigger test");

    send(juce::MidiMessage::noteOff(1, 72));
    const auto releasedTrajectoryPitch = pitchFor(72);
    send(juce::MidiMessage::noteOn(1, 72, 0.9f));
    const auto retriggeredPitch = heldPitchFor(72);
    require(retriggeredPitch > releasedTrajectoryPitch && retriggeredPitch < 72.0f,
            "Repeated glide destination reset or jumped to its target");
    require(std::abs((retriggeredPitch - releasedTrajectoryPitch)
                     - referenceAdvance)
                < juce::jmax(0.002f, referenceAdvance * 0.05f),
            "Repeated glide destination did not inherit the existing trajectory");

    audio.clear();
    engine.render(audio, {}, sound);
    const auto continuedPitch = heldPitchFor(72);
    require(std::abs((continuedPitch - retriggeredPitch) - referenceAdvance)
                < juce::jmax(0.002f, referenceAdvance * 0.05f),
            "Inherited glide trajectory changed speed after retrigger");

    // The glide accumulator belongs to the layer rather than to an allocated
    // voice. Let every released voice finish before the next repeated note so
    // this cannot pass by finding a still-active voice from the preceding key.
    engine.reset();
    sound.releaseSeconds = 0.001f;
    send(juce::MidiMessage::noteOn(1, 60, 0.9f));
    send(juce::MidiMessage::noteOff(1, 60));
    send(juce::MidiMessage::noteOn(1, 72, 0.9f));
    auto repeatedPitch = heldPitchFor(72);
    require(repeatedPitch > 59.9f && repeatedPitch < 61.0f,
            "Persistent portamento did not start at the preceding pitch");
    for (int repetition = 0; repetition < 5; ++repetition)
    {
        send(juce::MidiMessage::noteOff(1, 72));
        require(pitchFor(72) < -900.0f,
                "Short release did not retire the previous glide voice");
        send(juce::MidiMessage::noteOn(1, 72, 0.9f));
        const auto nextPitch = heldPitchFor(72);
        require(nextPitch > repeatedPitch && nextPitch < 72.0f,
                "Continuous glide reset during repeated destination notes");
        repeatedPitch = nextPitch;
    }

    // Changing the destination during the same flight also has to start from
    // the live curve, rather than from the last discrete key (72 here).
    send(juce::MidiMessage::noteOff(1, 72));
    send(juce::MidiMessage::noteOn(1, 67, 0.9f));
    const auto redirectedPitch = heldPitchFor(67);
    require(redirectedPitch > repeatedPitch
                && redirectedPitch < repeatedPitch + 1.0f,
            "A new glide destination reset to the preceding MIDI note");

    // A lower target below the live pitch must reverse both the newly played
    // voice and the still-audible VCA tail of the note that was released.
    engine.reset();
    sound.releaseSeconds = 0.5f;
    send(juce::MidiMessage::noteOn(1, 60, 0.9f));
    send(juce::MidiMessage::noteOff(1, 60));
    send(juce::MidiMessage::noteOn(1, 72, 0.9f));
    for (int block = 0; block < 80; ++block)
    {
        audio.clear();
        engine.render(audio, {}, sound);
    }
    send(juce::MidiMessage::noteOff(1, 72));
    send(juce::MidiMessage::noteOn(1, 48, 0.9f));
    const auto lowerPitchBefore = heldPitchFor(48);
    const auto releasedUpperPitchBefore = pitchFor(72);
    audio.clear();
    engine.render(audio, {}, sound);
    const auto lowerPitchAfter = heldPitchFor(48);
    const auto releasedUpperPitchAfter = pitchFor(72);
    require(lowerPitchAfter < lowerPitchBefore,
            "A lower destination did not reverse the live glide");
    require(releasedUpperPitchAfter < releasedUpperPitchBefore,
            "An audible release tail kept following the abandoned upward glide");

    engine.reset();
    sound.releaseSeconds = 0.2f;
    sound.glideTypeMode = 2;
    send(juce::MidiMessage::noteOn(1, 60, 0.9f));
    send(juce::MidiMessage::noteOff(1, 60));
    send(juce::MidiMessage::noteOn(1, 72, 0.9f));
    for (int block = 0; block < 48; ++block)
    {
        audio.clear();
        engine.render(audio, {}, sound);
    }
    require(std::abs(pitchFor(72) - std::round(pitchFor(72))) < 1.0e-6f,
            "Glissando did not quantise the moving pitch to semitone steps");

    engine.reset();
    sound.glideTypeMode = 5;
    send(juce::MidiMessage::noteOn(1, 60, 0.9f));
    send(juce::MidiMessage::noteOff(1, 60));
    send(juce::MidiMessage::noteOn(1, 72, 0.9f));
    require(std::abs(pitchFor(72) - 72.0f) < 0.01f,
            "Fingered portamento incorrectly affected a staccato note");

    engine.reset();
    send(juce::MidiMessage::noteOn(1, 60, 0.9f));
    send(juce::MidiMessage::noteOn(1, 72, 0.9f));
    require(pitchFor(72) > 59.9f && pitchFor(72) < 61.0f,
            "Fingered portamento ignored a legato note transition");
}

void testLfoLevelModifierGate()
{
    wave::dsp::WaldorfEngine engine;
    engine.prepare(48000.0, 512);
    wave::parameters::Snapshot parameters;
    parameters.attackSeconds = 0.001f;
    parameters.cutoffHz = 16000.0f;
    parameters.filterEnvelopeSemitones = 0.0f;
    parameters.lfos[0].rate = 100.0f;
    parameters.modulationRoutes[wave::parameters::osc1PitchMod1]
        = { 0, 38, 24.0f };
    parameters.modulationRoutes[wave::parameters::lfo1LevelMod]
        = { 38, 22, 63.0f }; // Maximum x Modwheel, as used by factory pads.

    juce::AudioBuffer<float> audio(2, 512);
    juce::MidiBuffer noteOn;
    noteOn.addEvent(juce::MidiMessage::noteOn(1, 60, 0.9f), 0);
    engine.render(audio, noteOn, parameters);
    auto wheelDownPeak = std::abs(engine.firstActivePitchModulation());
    for (int block = 0; block < 80; ++block)
    {
        audio.clear();
        engine.render(audio, {}, parameters);
        wheelDownPeak = juce::jmax(
            wheelDownPeak, std::abs(engine.firstActivePitchModulation()));
    }
    require(wheelDownPeak < 1.0e-5f,
            "Modwheel-gated LFO remained full-on with the wheel down");

    juce::MidiBuffer wheelUp;
    wheelUp.addEvent(juce::MidiMessage::controllerEvent(1, 1, 127), 0);
    auto wheelUpPeak = 0.0f;
    for (int block = 0; block < 80; ++block)
    {
        audio.clear();
        engine.render(audio, block == 0 ? wheelUp : juce::MidiBuffer {}, parameters);
        wheelUpPeak = juce::jmax(
            wheelUpPeak, std::abs(engine.firstActivePitchModulation()));
    }
    require(wheelUpPeak > 0.25f,
            "Modwheel-gated LFO did not fade in with the wheel raised");

    engine.reset();
    parameters.modulationRoutes[wave::parameters::lfo1LevelMod].amount = 0.0f;
    engine.render(audio, noteOn, parameters);
    auto bypassPeak = 0.0f;
    for (int block = 0; block < 80; ++block)
    {
        audio.clear();
        engine.render(audio, {}, parameters);
        bypassPeak = juce::jmax(
            bypassPeak, std::abs(engine.firstActivePitchModulation()));
    }
    require(bypassPeak > 0.25f,
            "Zero LFO Level Modifier did not retain the Wave's full-level bypass");
}

void testPerformanceControlXFeedsLfoRoutes()
{
    wave::dsp::WaldorfEngine engine;
    engine.prepare(48000.0, 512);

    wave::dsp::WaldorfEngine::PerformanceSnapshot performance;
    performance.controlXController = 74;
    auto& layer = performance.layers[0];
    layer.enabled = true;
    layer.source = 2;
    layer.sound.attackSeconds = 0.001f;
    layer.sound.cutoffHz = 16000.0f;
    layer.sound.filterEnvelopeSemitones = 0.0f;
    layer.sound.filterVelocitySemitones = 0.0f;
    layer.sound.filterKeytrackAmount = 0.0f;
    layer.sound.lfos[0].rate = 100.0f;
    layer.sound.modulationRoutes[wave::parameters::osc1PitchMod1]
        = { 0, 34, 24.0f }; // LFO 1 x Performance Control X.
    layer.sound.modulationRoutes[wave::parameters::filterMod1]
        = { 0, 34, 24.0f };

    juce::AudioBuffer<float> audio(2, 512);
    juce::MidiBuffer noteOn;
    noteOn.addEvent(juce::MidiMessage::noteOn(1, 60, 0.9f), 0);
    engine.render(audio, noteOn, performance);
    auto controlDownPeak = 0.0f;
    auto controlDownMinimumCutoff = std::numeric_limits<float>::max();
    auto controlDownMaximumCutoff = 0.0f;
    for (int block = 0; block < 80; ++block)
    {
        audio.clear();
        engine.render(audio, {}, performance);
        controlDownPeak = juce::jmax(
            controlDownPeak, std::abs(engine.firstActivePitchModulation()));
        for (const auto& state : engine.voiceStates())
            if (state.active)
            {
                controlDownMinimumCutoff
                    = juce::jmin(controlDownMinimumCutoff, state.cutoffHz);
                controlDownMaximumCutoff
                    = juce::jmax(controlDownMaximumCutoff, state.cutoffHz);
            }
    }
    require(controlDownPeak < 1.0e-5f,
            "Control-X-gated LFO was active before its assigned MIDI controller moved");
    require(controlDownMaximumCutoff - controlDownMinimumCutoff < 1.0f,
            "Control-X-gated filter cutoff moved before its controller moved");

    juce::MidiBuffer controlUp;
    controlUp.addEvent(juce::MidiMessage::controllerEvent(1, 74, 127), 0);
    auto controlUpPeak = 0.0f;
    auto controlUpMinimumCutoff = std::numeric_limits<float>::max();
    auto controlUpMaximumCutoff = 0.0f;
    for (int block = 0; block < 80; ++block)
    {
        audio.clear();
        engine.render(audio, block == 0 ? controlUp : juce::MidiBuffer {}, performance);
        controlUpPeak = juce::jmax(
            controlUpPeak, std::abs(engine.firstActivePitchModulation()));
        for (const auto& state : engine.voiceStates())
            if (state.active)
            {
                controlUpMinimumCutoff
                    = juce::jmin(controlUpMinimumCutoff, state.cutoffHz);
                controlUpMaximumCutoff
                    = juce::jmax(controlUpMaximumCutoff, state.cutoffHz);
            }
    }
    require(controlUpPeak > 0.25f,
            "Performance Control X did not open its LFO modulation routes");
    require(controlUpMaximumCutoff / controlUpMinimumCutoff > 4.0f,
            "Performance Control X did not open the LFO filter-cutoff route");
}

void testOscillatorLinkUsesOscillatorOneModulation()
{
    wave::dsp::WaldorfEngine engine;
    engine.prepare(48000.0, 512);
    wave::parameters::Snapshot parameters;
    parameters.attackSeconds = 0.001f;
    parameters.cutoffHz = 16000.0f;
    parameters.filterEnvelopeSemitones = 0.0f;
    parameters.lfos[0].rate = 100.0f;
    parameters.modulationRoutes[wave::parameters::osc1PitchMod1]
        = { 0, 38, 24.0f };
    parameters.modulationRoutes[wave::parameters::osc2PitchMod1]
        = { 37, 38, 0.0f };

    juce::AudioBuffer<float> audio(2, 512);
    juce::MidiBuffer noteOn;
    noteOn.addEvent(juce::MidiMessage::noteOn(1, 60, 0.9f), 0);
    engine.render(audio, noteOn, parameters);
    auto unlinkedPeak = 0.0f;
    for (int block = 0; block < 80; ++block)
    {
        audio.clear();
        engine.render(audio, {}, parameters);
        unlinkedPeak = juce::jmax(
            unlinkedPeak, std::abs(engine.firstActivePitchModulation(1)));
    }
    require(unlinkedPeak < 1.0e-5f,
            "Unlinked Oscillator 2 used Oscillator 1's pitch modulation");

    engine.reset();
    parameters.oscillatorLinkEnabled = true;
    engine.render(audio, noteOn, parameters);
    auto linkedPeak = 0.0f;
    for (int block = 0; block < 80; ++block)
    {
        audio.clear();
        engine.render(audio, {}, parameters);
        linkedPeak = juce::jmax(
            linkedPeak, std::abs(engine.firstActivePitchModulation(1)));
    }
    require(linkedPeak > 0.25f,
            "Wave Link did not apply Oscillator 1 pitch modulation to Oscillator 2");
}

void testReleasedVoicesAreStolenBeforeHeldChord()
{
    wave::dsp::WaldorfEngine engine;
    engine.prepare(48000.0, 64);
    wave::parameters::Snapshot parameters;
    parameters.attackSeconds = 0.001f;
    parameters.releaseSeconds = 20.0f;
    parameters.cutoffHz = 16000.0f;
    parameters.filterEnvelopeSemitones = 0.0f;

    juce::AudioBuffer<float> audio(2, 64);
    juce::MidiBuffer chord;
    for (const auto note : { 60, 64, 67, 71 })
        chord.addEvent(juce::MidiMessage::noteOn(1, note, 0.8f), 0);
    engine.render(audio, chord, parameters);
    require(engine.heldVoiceCount() == 4,
            "Held chord did not allocate all four voices");

    // Long release tails deliberately fill all remaining voices. Further bass
    // notes must reuse those tails rather than cutting keys still held down.
    for (int index = 0; index < wave::dsp::WaldorfEngine::voiceCount; ++index)
    {
        juce::MidiBuffer bass;
        const auto note = 36 + index % 8;
        bass.addEvent(juce::MidiMessage::noteOn(1, note, 0.9f), 0);
        bass.addEvent(juce::MidiMessage::noteOff(1, note), 32);
        engine.render(audio, bass, parameters);
    }
    require(engine.activeVoiceCount() == wave::dsp::WaldorfEngine::voiceCount,
            "Voice-stealing test did not exhaust polyphony");
    require(engine.heldVoiceCount() == 4,
            "Released bass tails stole voices from the held chord");
}

void testVoiceStealPreservesAnalogueHandover()
{
    wave::dsp::WaldorfEngine engine;
    engine.prepare(48000.0, 512);
    wave::parameters::Snapshot parameters;
    parameters.attackSeconds = 0.001f;
    parameters.sustainLevel = 1.0f;
    parameters.filterAttackSeconds = 1.0f;
    parameters.filterSustainLevel = 1.0f;
    parameters.cutoffHz = 16000.0f;
    parameters.filterEnvelopeSemitones = 0.0f;

    juce::AudioBuffer<float> audio(2, 512);
    juce::MidiBuffer fillPolyphony;
    for (int note = 48; note < 48 + wave::dsp::WaldorfEngine::voiceCount; ++note)
        fillPolyphony.addEvent(juce::MidiMessage::noteOn(1, note, 0.9f), 0);
    engine.render(audio, fillPolyphony, parameters);
    for (int block = 0; block < 8; ++block)
        engine.render(audio, {}, parameters);
    require(engine.firstActiveVcaControlValue() > 0.75f,
            "Voice-stealing fixture did not reach a stable VCA voltage");
    const auto filterEnvelopeBeforeSteal = engine.firstActiveFilterEnvelopeValue();
    require(filterEnvelopeBeforeSteal > 0.05f,
            "Voice-stealing fixture did not establish a prior filter envelope");

    juce::MidiBuffer steal;
    steal.addEvent(juce::MidiMessage::noteOn(1, 108, 0.9f), 0);
    engine.render(audio, steal, parameters);
    require(engine.firstActiveVcaControlValue() > 0.60f,
            "Voice stealing discharged the analogue VCA and introduced a click");
    require(engine.firstActiveFilterEnvelopeValue()
                < filterEnvelopeBeforeSteal * 0.4f,
            "Voice stealing carried the previous note's filter tuning into the new note");
}

void testFullThreeCardPolyphony()
{
    static_assert(wave::dsp::WaldorfEngine::voiceBoardCount == 3);
    static_assert(wave::dsp::WaldorfEngine::voicesPerBoard == 16);
    static_assert(wave::dsp::WaldorfEngine::voiceCount == 48);

    wave::dsp::WaldorfEngine engine;
    engine.prepare(48000.0, 64);
    wave::parameters::Snapshot parameters;
    parameters.attackSeconds = 0.001f;
    parameters.releaseSeconds = 1.0f;
    parameters.cutoffHz = 16000.0f;
    parameters.filterEnvelopeSemitones = 0.0f;

    juce::AudioBuffer<float> audio(2, 64);
    juce::MidiBuffer notes;
    for (int note = 36; note < 36 + wave::dsp::WaldorfEngine::voiceCount; ++note)
        notes.addEvent(juce::MidiMessage::noteOn(1, note, 0.8f), 0);
    engine.render(audio, notes, parameters);
    require(engine.activeVoiceCount() == 48 && engine.heldVoiceCount() == 48,
            "Three-card engine did not allocate all 48 simultaneous voices");

    juce::MidiBuffer fortyNinth;
    fortyNinth.addEvent(juce::MidiMessage::noteOn(1, 96, 0.8f), 0);
    engine.render(audio, fortyNinth, parameters);
    require(engine.activeVoiceCount() == 48,
            "Voice allocator exceeded the 48-voice hardware limit");
}

void testParallelVoiceCardsMatchSerialRenderer()
{
    constexpr auto sampleRate = 48000.0;
    constexpr auto blockSize = 256;
    wave::dsp::WaldorfEngine serial;
    wave::dsp::WaldorfEngine parallel;
    serial.setVoiceCardThreadingEnabled(false);
    serial.prepare(sampleRate, blockSize);
    parallel.prepare(sampleRate, blockSize);

    wave::parameters::Snapshot parameters;
    parameters.attackSeconds = 0.001f;
    parameters.releaseSeconds = 0.8f;
    parameters.cutoffHz = 7200.0f;
    parameters.resonanceAmount = 0.46f;
    parameters.filterEnvelopeSemitones = 18.0f;
    parameters.noiseLevel = 0.11f;
    parameters.wavePosition = 17.25f;
    parameters.wavePosition2 = 42.5f;

    juce::AudioBuffer<float> serialAudio(2, blockSize);
    juce::AudioBuffer<float> parallelAudio(2, blockSize);
    for (int block = 0; block < 10; ++block)
    {
        juce::MidiBuffer midi;
        if (block == 0)
            for (int voice = 0; voice < wave::dsp::WaldorfEngine::voiceCount; ++voice)
                midi.addEvent(juce::MidiMessage::noteOn(1, 36 + voice, 0.8f), 0);
        if (block == 4)
        {
            // Exercise serial and parallel sub-ranges in the same host block.
            midi.addEvent(juce::MidiMessage::noteOff(1, 36), 31);
            midi.addEvent(juce::MidiMessage::noteOn(1, 92, 0.7f), 96);
        }

        serialAudio.clear();
        parallelAudio.clear();
        serial.render(serialAudio, midi, parameters);
        parallel.render(parallelAudio, midi, parameters);
        for (int channel = 0; channel < 2; ++channel)
            for (int sample = 0; sample < blockSize; ++sample)
                require(std::abs(serialAudio.getSample(channel, sample)
                                 - parallelAudio.getSample(channel, sample))
                            < 1.0e-7f,
                        "Parallel voice cards changed the deterministic audio result");
    }

    require(serial.parallelVoiceCardRenderCount() == 0,
            "Disabled card threading still dispatched a worker");
    require(parallel.parallelVoiceCardRenderCount() > 0,
            "Full polyphony did not dispatch the voice-card workers");
    const auto serialVoices = serial.voiceStates();
    const auto parallelVoices = parallel.voiceStates();
    for (size_t voice = 0; voice < serialVoices.size(); ++voice)
        require(serialVoices[voice].triggerNote == parallelVoices[voice].triggerNote
                    && serialVoices[voice].active == parallelVoices[voice].active
                    && serialVoices[voice].keyDown == parallelVoices[voice].keyDown,
                "Parallel voice-card state diverged from the serial allocator");

    // Hosts may change sample rate and block size repeatedly. This must stop
    // and replace the persistent workers without leaving an in-flight job.
    for (int prepare = 0; prepare < 4; ++prepare)
    {
        const auto size = prepare % 2 == 0 ? 64 : 128;
        parallel.prepare(prepare % 2 == 0 ? 44100.0 : 96000.0, size);
        juce::AudioBuffer<float> lifecycleAudio(2, size);
        parallel.render(lifecycleAudio, {}, parameters);
    }
}

juce::AudioBuffer<float> renderReferenceSequence(int samples)
{
    wave::dsp::WaldorfEngine engine;
    engine.prepare(48000.0, 256);
    wave::parameters::Snapshot parameters;
    parameters.attackSeconds = 0.001f;
    parameters.releaseSeconds = 0.1f;
    parameters.cutoffHz = 12000.0f;
    parameters.filterEnvelopeSemitones = 0.0f;

    juce::AudioBuffer<float> audio(2, samples);
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, 60, 0.8f), 0);
    if (samples > 24000)
        midi.addEvent(juce::MidiMessage::noteOff(1, 60), 24000);
    engine.render(audio, midi, parameters);
    return audio;
}

void testReferenceComparison()
{
    const auto reference = renderReferenceSequence(48000);
    const auto repeat = renderReferenceSequence(48000);
    const auto deterministic = wave::dsp::ReferenceComparator::compare(reference, repeat, 8);
    require(deterministic.lagSamples == 0 && deterministic.correlation > 0.999999f
                && deterministic.rmsError < 1.0e-7f,
            "Audio model is not deterministic enough for capture comparison");

    const auto* capturePath = std::getenv("WAVE_REFERENCE_CAPTURE");
    if (capturePath == nullptr)
        return;

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(
        formats.createReaderFor(juce::File(capturePath)));
    require(reader != nullptr, "WAVE_REFERENCE_CAPTURE is not a readable audio file");
    const auto captureSamples = static_cast<int>(juce::jmin<int64_t>(reader->lengthInSamples,
                                                                     48000 * 20));
    juce::AudioBuffer<float> capture(juce::jmin(2, static_cast<int>(reader->numChannels)),
                                     captureSamples);
    require(reader->read(&capture, 0, captureSamples, 0, true, true),
            "Real-Wave reference capture could not be read");
    const auto model = renderReferenceSequence(captureSamples);
    const auto metrics = wave::dsp::ReferenceComparator::compare(capture, model, 4096);
    require(std::isfinite(metrics.correlation) && std::isfinite(metrics.rmsError),
            "Reference comparison produced invalid metrics");
    std::cerr << "Real Wave comparison: lag=" << metrics.lagSamples
              << " gain=" << metrics.fittedGain
              << " corr=" << metrics.correlation
              << " rms=" << metrics.rmsError
              << " peak=" << metrics.peakError << '\n';
}

void testOfficialFirmwareWhenAvailable()
{
    const auto* path = std::getenv("WAVE_FIRMWARE_DIR");
    if (path == nullptr)
        return;

    wave::firmware::Bundle bundle;
    const auto report = bundle.load(juce::File(path));
    require(report.authenticity == wave::firmware::Bundle::Authenticity::verifiedOs1700,
            "WAVE_FIRMWARE_DIR does not contain the known OS 1.700 pair");

    const auto* master = static_cast<const uint8_t*>(bundle.getMasterImage().getData());
    const auto* voice = static_cast<const uint8_t*>(bundle.getVoiceImage().getData());
    const auto bigEndian32 = [](const uint8_t* bytes) {
        return (static_cast<uint32_t>(bytes[0]) << 24u)
               | (static_cast<uint32_t>(bytes[1]) << 16u)
               | (static_cast<uint32_t>(bytes[2]) << 8u)
               | static_cast<uint32_t>(bytes[3]);
    };
    require(master[0] == 0x4e && master[1] == 0xf9 && bigEndian32(master + 2) == 0x100c,
            "Master OS entry jump does not match the authenticated image layout");
    require(bigEndian32(master + 0xdf78) == 0xfea00000
                && bigEndian32(master + 0xdf7e) == 0xfea04000,
            "Master OS LCD video-memory references changed unexpectedly");
    require(bigEndian32(voice + 0x20) == 0x00008ffe
                && bigEndian32(voice + 0x24) == 0x00000400,
            "Voice image loader header or reset vectors changed unexpectedly");

    auto sharedMemoryStorage = std::make_unique<wave::firmware::SharedFirmwareMemory>();
    auto& sharedMemory = *sharedMemoryStorage;
    sharedMemory.clear();
    auto masterRuntimeStorage = std::make_unique<wave::firmware::MasterFirmwareRuntime>();
    auto& masterRuntime = *masterRuntimeStorage;
    masterRuntime.attachSharedMemory(sharedMemory);
    require(masterRuntime.loadAndStart(bundle.getMasterImage()),
            "Authenticated master firmware could not be installed on the CPU-board bus");
    require(masterRuntime.localByte(wave::firmware::MasterFirmwareRuntime::imageBase) == 0x4e
                && masterRuntime.localByte(
                       wave::firmware::MasterFirmwareRuntime::imageBase + 1u) == 0xf9,
            "Master OS image is not installed at its linked $001000 address");
    const auto executedMasterCycles = masterRuntime.runCycles(1000);
    require(executedMasterCycles > 0,
            "Master firmware did not consume its deterministic CPU cycle budget");
    if (!masterRuntime.completedColdHardwareSetup())
        std::cerr << "Master boot trace: PC=0x" << std::hex << masterRuntime.programCounter()
                  << " SP=0x" << masterRuntime.stackPointer()
                  << " last-unmapped=0x" << masterRuntime.lastUnmappedReadAddress()
                  << " unmapped=" << std::dec << masterRuntime.unmappedReadCount() << '\n';
    require(masterRuntime.completedColdHardwareSetup(),
            "Master firmware did not perform its observed cold-start hardware writes");

    masterRuntime.runCycles(2000000);
    require(masterRuntime.runOs1700InitialisationFileLoad(),
            "Master OS did not read synthetic INIT.SND and INIT.PFM through its file loader");
    const auto localLong = [&masterRuntime](uint32_t address) {
        return (static_cast<uint32_t>(masterRuntime.localByte(address)) << 24u)
               | (static_cast<uint32_t>(masterRuntime.localByte(address + 1u)) << 16u)
               | (static_cast<uint32_t>(masterRuntime.localByte(address + 2u)) << 8u)
               | masterRuntime.localByte(address + 3u);
    };
    require(localLong(0x4de14u) >= wave::firmware::MasterFirmwareRuntime::imageBase
                && localLong(0x4de14u) < 0x00050000u
                && localLong(0x4de18u) >= wave::firmware::MasterFirmwareRuntime::imageBase
                && localLong(0x4de18u) < 0x00050000u,
            "Genuine startup did not install the OS MIDI callbacks after floppy bypass");
    require(masterRuntime.loadedSyntheticInitialisationFiles(),
            "Both synthetic initialization files were not consumed by the genuine loader");
    require(masterRuntime.lcdVideoWriteCount() > 0,
            "Genuine master firmware did not reach its LCD drawing path");
    require(masterRuntime.unmappedReadCount() == 0,
            "Extended main-OS boot reached an unmodelled bus address");
    auto voiceRuntimeStorage = std::make_unique<wave::firmware::VoiceFirmwareRuntime>();
    auto& voiceRuntime = *voiceRuntimeStorage;
    voiceRuntime.attachSharedMemory(sharedMemory);
    require(voiceRuntime.loadAndReset(bundle.getVoiceImage()),
            "Authenticated voice firmware could not be installed on the WDV bus");
    auto executedVoiceCycles = 0;
    for (int slice = 0; slice < 20 && !voiceRuntime.waitingForMasterAcknowledgement(); ++slice)
        executedVoiceCycles += voiceRuntime.runCycles(100000);
    require(voiceRuntime.waitingForMasterAcknowledgement(),
            "Voice firmware did not publish and wait at its master acknowledgement mailbox");
    require(sharedMemory.program[0x5086] == 0xff && sharedMemory.program[0x508a] == 0x00,
            "Voice bootstrap mailbox state is inconsistent before master acknowledgement");

    const auto handoffCompleted = masterRuntime.runOs1700VoiceBoardLoaderHandoff(1);
    if (!handoffCompleted)
        std::cerr << "Master handoff trace: PC=0x" << std::hex << masterRuntime.programCounter()
                  << " ack=0x" << static_cast<int>(sharedMemory.program[0x508a])
                  << " service=0x" << static_cast<int>(sharedMemory.program[0x508e]) << '\n';
    require(handoffCompleted,
            "Master OS 1.700 did not execute its post-copy WDV loader handoff");
    require(sharedMemory.program[0x508a] == 0x01,
            "Main 68000 did not issue the observed WDV acknowledgement value");
    for (int slice = 0; slice < 20 && !voiceRuntime.reachedServiceLoop(); ++slice)
        executedVoiceCycles += voiceRuntime.runCycles(500000);
    require(executedVoiceCycles > 0,
            "Voice firmware did not consume its deterministic CPU cycle budget");
    require(voiceRuntime.controlTickCount() > 0,
            "WDV control interrupt clock did not advance");
    if (!voiceRuntime.reachedServiceLoop())
        std::cerr << "WDV boot trace: PC=0x" << std::hex << voiceRuntime.programCounter()
                  << " publish=0x" << static_cast<int>(voiceRuntime.sharedByte(0x5086))
                  << " service=0x" << static_cast<int>(voiceRuntime.sharedByte(0x508e))
                  << " asic=0x" << voiceRuntime.asicWord(0x12)
                  << " unmapped=" << std::dec << voiceRuntime.unmappedReadCount() << '\n';
    require(voiceRuntime.reachedServiceLoop(),
            "Voice firmware did not complete bootstrap and enter its shared-memory loop");
    require(masterRuntime.completeOs1700VoiceBoardServiceHandoff(),
            "Master firmware did not resume through the WDV service-success branch");
    require(voiceRuntime.asicWord(0x12) == 0x0004,
            "Voice firmware did not perform the observed oscillator-chip register write");
    const auto hardwareWrites = voiceRuntime.consumeHardwareWrites();
    require(!hardwareWrites.empty(), "WDV hardware writes were not timestamped");
    require(std::is_sorted(hardwareWrites.begin(), hardwareWrites.end(), [](const auto& a,
                                                                           const auto& b) {
                return a.cycle < b.cycle;
            }),
            "WDV hardware-write trace is not monotonic");
    require(std::any_of(hardwareWrites.begin(), hardwareWrites.end(), [](const auto& write) {
                return write.address == wave::firmware::VoiceFirmwareRuntime::asicBase + 0x12u;
            }),
            "WDV trace omitted the observed oscillator-chip control write");

    for (int slice = 0; slice < 40; ++slice)
    {
        masterRuntime.runCycles(50000);
        voiceRuntime.runCycles(100000);
    }
    require(masterRuntime.timerTickCount() > 0,
            "Main-board 6522 timer did not advance after OS hardware setup");
    require(masterRuntime.installedSyntheticInitialisationRecords(),
            "Master runtime did not install the synthetic INIT.SND and INIT.PFM records");
    require(std::equal(master + 0x0a42e, master + 0x0a52e,
                       sharedMemory.program.begin() + 0x5300),
            "Installed INIT.SND bytes differ from the genuine OS fallback record");
    require(std::equal(master + 0x0a22e, master + 0x0a42e,
                       sharedMemory.program.begin() + 0x5400),
            "Installed INIT.PFM bytes differ from the genuine OS fallback record");
    require(masterRuntime.programCounter() >= wave::firmware::MasterFirmwareRuntime::imageBase
                && masterRuntime.programCounter()
                       < wave::firmware::MasterFirmwareRuntime::localRamSize,
            "Master OS did not return safely to its genuine main loop after WDV startup");
    require(masterRuntime.unmappedReadCount() == 0,
            "Post-WDV main loop reached an unmodelled CPU-board address");

    const auto discardedWrites = voiceRuntime.consumeHardwareWrites();
    juce::ignoreUnused(discardedWrites);
    masterRuntime.pushMidiByte(0, 0x90);
    masterRuntime.pushMidiByte(0, 60);
    masterRuntime.pushMidiByte(0, 100);
    for (int slice = 0; slice < 30; ++slice)
    {
        masterRuntime.runCycles(50000);
        voiceRuntime.runCycles(100000);
    }
    const auto midiWrites = voiceRuntime.consumeHardwareWrites();
    require(!midiWrites.empty(),
            "MIDI note did not produce WDV oscillator/CV hardware writes");
    require(std::any_of(midiWrites.begin(), midiWrites.end(), [](const auto& write) {
                return write.address >= wave::firmware::VoiceFirmwareRuntime::asicBase
                       && write.address
                              < wave::firmware::VoiceFirmwareRuntime::asicBase + 0x200u;
            }),
            "Firmware-driven note did not reach either oscillator-chip register page");
    require(masterRuntime.unmappedReadCount() == 0 && voiceRuntime.unmappedReadCount() == 0,
            "Firmware-driven MIDI note reached an unmodelled system-bus address");
}

void testDecodedVoiceBoardProtocol()
{
    wave::firmware::SharedFirmwareMemory memory;
    memory.clear();
    wave::firmware::VoiceBoardProtocol protocol(memory);
    auto record = protocol.voiceRecord(0, 7);
    record[6] = 0x01;
    record[44] = 0x12;
    require(protocol.requestUpdate(0, 7),
            "Decoded WDV update request could not acquire the shared bus mutex");
    const auto pending = protocol.pendingUpdates();
    require(pending.size() == 1 && pending[0].voice == 7
                && pending[0].recordOffset == 0x700
                && pending[0].semaphoreOffset == 0x50a9,
            "WDV update mask, record stride, or semaphore layout is incorrect");
    require(memory.program[wave::firmware::VoiceBoardProtocol::busMutex] == 0,
            "WDV shared bus mutex was not released");
}

void testVoiceBoardWaveRamDecode()
{
    // Hand-built WDV image (no firmware needed): SP 0x8FFE, reset 0x400, then
    // byte stores through RAM A, RAM B and the write-both alias, and a spin.
    static constexpr uint8_t code[] = {
        0x13, 0xfc, 0x00, 0xa5, 0x00, 0x06, 0x00, 0x01, // move.b #$a5,$060001
        0x13, 0xfc, 0x00, 0x5a, 0x00, 0x05, 0x00, 0x03, // move.b #$5a,$050003
        0x13, 0xfc, 0x00, 0x11, 0x00, 0x04, 0x00, 0x05, // move.b #$11,$040005
        0x60, 0xfe                                       // bra.s *
    };
    juce::MemoryBlock image(0x20 + 0x400 + sizeof(code), true);
    auto* bytes = static_cast<uint8_t*>(image.getData());
    bytes[0x20 + 2] = 0x8f;
    bytes[0x20 + 3] = 0xfe;
    bytes[0x20 + 6] = 0x04;
    std::copy(std::begin(code), std::end(code), bytes + 0x20 + 0x400);

    wave::firmware::SharedFirmwareMemory memory;
    memory.clear();
    wave::firmware::VoiceFirmwareRuntime runtime;
    runtime.attachSharedMemory(memory);
    require(runtime.loadAndReset(image), "Synthetic WDV image was rejected");
    runtime.runCycles(2000);
    require(runtime.waveRamByte(0, 1) == 0xa5 && runtime.waveRamByte(1, 1) == 0xa5,
            "Write at $060001 did not reach both wave RAMs");
    require(runtime.waveRamByte(1, 3) == 0x5a && runtime.waveRamByte(0, 3) == 0,
            "Write at $050003 did not reach wave RAM B only");
    require(runtime.waveRamByte(0, 5) == 0x11 && runtime.waveRamByte(1, 5) == 0,
            "Write at $040005 did not reach wave RAM A only");
    require(memory.mainRam[0x60001] == 0 && memory.mainRam[0x50003] == 0
                && memory.mainRam[0x40005] == 0,
            "Voice-board wave RAM stores leaked into master DRAM");
}

class Test68000Bus final : public wave::firmware::M68000Bus
{
public:
    [[nodiscard]] uint8_t read8(uint32_t address) noexcept override
    {
        return memory[static_cast<size_t>(address) % memory.size()];
    }

    void write8(uint32_t address, uint8_t value) noexcept override
    {
        memory[static_cast<size_t>(address) % memory.size()] = value;
    }

    void write16(uint32_t address, uint16_t value)
    {
        write8(address, static_cast<uint8_t>(value >> 8u));
        write8(address + 1u, static_cast<uint8_t>(value));
    }

    void write32(uint32_t address, uint32_t value)
    {
        write16(address, static_cast<uint16_t>(value >> 16u));
        write16(address + 2u, static_cast<uint16_t>(value));
    }

    std::array<uint8_t, 65536> memory{};
};

void test68000ExecutionCore()
{
    Test68000Bus bus;
    bus.write32(0x0000, 0x00008000); // Initial supervisor stack pointer.
    bus.write32(0x0004, 0x00000100); // Reset program counter.
    bus.write16(0x0100, 0x702a);     // MOVEQ #42,D0
    bus.write16(0x0102, 0x5280);     // ADDQ.L #1,D0
    bus.write16(0x0104, 0x13fc);     // MOVE.B #$5a,$00000200
    bus.write16(0x0106, 0x005a);
    bus.write32(0x0108, 0x00000200);
    bus.write16(0x010c, 0x4e72);     // STOP #$2700
    bus.write16(0x010e, 0x2700);

    wave::firmware::M68000 cpu;
    cpu.reset(bus);
    require(cpu.programCounter() == 0x100 && cpu.stackPointer() == 0x8000,
            "68000 reset did not fetch the big-endian vector table");
    require(cpu.execute(bus, 100) > 0, "68000 core did not execute a cycle budget");
    require(cpu.dataRegister(0) == 43, "68000 arithmetic/register execution is incorrect");
    require(bus.memory[0x200] == 0x5a, "68000 absolute byte write did not reach the bus");

    wave::firmware::M68000 delayCpu;
    delayCpu.start(bus, 0x8000, 0x0100);
    delayCpu.setDataRegister(bus, 4, 1000u);
    const auto skipped = delayCpu.fastForwardDbraLoop(bus, 4, 4096u);
    require(skipped == 4090u && (delayCpu.dataRegister(4) & 0xffffu) == 591u,
            "68000 DBRA fast-forward did not preserve its cycle/register state");
}

void test6522TimerOneInterruptPath()
{
    wave::firmware::Via6522 via;
    via.reset();

    // The CPU-board wiring leaves PA0-PA6 pulled high and PA7 as the floppy
    // DRQ input. DDR bits must select between those pins and the output latch.
    via.write(3, 0xf0u);
    via.write(1, 0xa5u);
    require(via.read(1, 0x3cu) == 0xacu,
            "6522 port A did not merge its data-direction, latch, and input pins");

    via.write(11, 0x40u); // Timer 1 continuous mode.
    via.write(4, 0xceu);
    via.write(5, 0x04u);  // OS 1.700's $04CE timer latch.
    via.write(14, 0xc0u); // Enable Timer 1 IRQ.
    require(!via.interruptAsserted(),
            "6522 asserted Timer 1 IRQ before the counter expired");
    require(via.advanceCpuCycles(12319) == 0 && !via.interruptAsserted(),
            "6522 Timer 1 expired before its E-clock count elapsed");
    require(via.advanceCpuCycles(1) == 1 && via.interruptAsserted()
                && (via.read(13) & 0xc0u) == 0xc0u,
            "6522 Timer 1 did not raise its enabled interrupt flag");

    (void) via.read(4); // The Wave handler acknowledges by reading T1 low.
    require(!via.interruptAsserted() && (via.read(13) & 0x40u) == 0u,
            "6522 Timer 1 low-byte read did not acknowledge IRQ");
    require(via.advanceCpuCycles(12320) == 1 && via.interruptAsserted(),
            "6522 continuous Timer 1 did not reload from its latch");
}

void testLcdFramebuffer()
{
    wave::ui::LcdFramebuffer lcd;
    lcd.clear();
    require(wave::ui::LcdFramebuffer::memorySize == 3840,
            "LCD framebuffer size does not match 480 x 64 at one bit per pixel");

    lcd.write(0, 0x80);
    lcd.write(59, 0x01);
    lcd.write(60, 0x80);
    require(lcd.pixel(0, 0), "LCD first controller bit is not the first pixel");
    require(lcd.pixel(479, 0), "LCD last byte did not address the end of the scanline");
    require(lcd.pixel(0, 1), "LCD row stride is not 60 bytes");

    lcd.clear();
    lcd.line(3, 4, 19, 11);
    require(lcd.pixel(3, 4) && lcd.pixel(19, 11), "LCD line rasteriser lost an endpoint");
    lcd.text(24, 16, "WAVE 1.700");
    require(lcd.data() != std::array<uint8_t, wave::ui::LcdFramebuffer::memorySize>{},
            "LCD glyph renderer did not alter display RAM");

    lcd.write(static_cast<uint16_t>(wave::ui::LcdFramebuffer::memorySize), 0x55);
    require(lcd.read(0) == 0x55, "LCD controller address wrap is inconsistent");

    std::array<uint8_t, 0x4000> videoRam{};
    constexpr auto page = 2u;
    constexpr auto pageOffset = page * 0x1000u;
    videoRam[pageOffset] = 0x80;      // First visible byte, first pixel.
    videoRam[pageOffset + 59] = 0x01; // Last visible byte, last pixel.
    videoRam[pageOffset + 60] = 0xff; // Hardware padding must remain invisible.
    const auto byte = static_cast<size_t>(2 * 64 + 3);
    videoRam[pageOffset + byte] = 0x40;
    lcd.loadHardwareVideoRam(videoRam.data(), videoRam.size(), page);
    require(lcd.pixel(0, 0) && lcd.pixel(479, 0),
            "LCD visible byte span or bit order is incorrect");
    require(lcd.pixel(25, 2), "LCD scanout word stride is incorrect");
    require(!lcd.pixel(0, 1) && !lcd.pixel(32, 1),
            "LCD hardware padding leaked into the next scanline");
}

void testCompletePanelWiringContract()
{
    require(wave::panel::buttonDispatchCode.size() == 87
                && wave::panel::buttonDispatchCode[0] == 10
                && wave::panel::buttonDispatchCode[8] == 20
                && wave::panel::buttonDispatchCode[41] == 48,
            "Known OS 1.700 post-scan dispatch anchors changed");
    require(wave::panel::matrixIndexForDiagnosticCode(0) == 0
                && wave::panel::matrixIndexForDiagnosticCode(22) == 22
                && wave::panel::matrixIndexForDiagnosticCode(39) == 39,
            "The UI reapplied the firmware's private dispatch permutation");

    std::array<bool, 128> visibleInputs{};
    for (const auto& control : wave::panel::visibleSwitches)
    {
        const auto matrix = wave::panel::physicalMatrixIndex(control);
        require(matrix >= 0 && matrix < 87,
                "A visible switch has no physical matrix input");
        require(!visibleInputs[static_cast<size_t>(matrix)],
                "Two visible controls are wired to the same matrix input");
        visibleInputs[static_cast<size_t>(matrix)] = true;
    }
    for (const auto& control : wave::panel::keyboardPanelSwitches)
    {
        const auto matrix = wave::panel::physicalMatrixIndex(control);
        require(matrix >= 0 && matrix < 87,
                "A keyboard-panel switch has no physical matrix input");
        require(!visibleInputs[static_cast<size_t>(matrix)],
                "Two visible controls are wired to the same matrix input");
        visibleInputs[static_cast<size_t>(matrix)] = true;
    }
    require(visibleInputs[71] && visibleInputs[70],
            "CANCEL and OK are absent from the visible switch map");
    const auto* glideSwitch = wave::panel::keyboardPanelSwitchAt(228.0f, 751.0f);
    const auto* glideEdit = wave::panel::keyboardPanelSwitchAt(366.173f, 753.174f);
    require(glideSwitch != nullptr && glideSwitch->diagnosticCode == 6
                && glideEdit != nullptr && glideEdit->diagnosticCode == 11,
            "The lower Glide switches are absent from the canonical input map");
    const auto* glideEditLed = wave::panel::keyboardPanelLedAt(396.0f, 724.0f);
    require(glideEditLed != nullptr && glideEditLed->redSerialCode == 75,
            "The lower Glide Edit lamp is absent from the serial-output map");
    require(std::any_of(
                wave::panel::editIndicators.begin(),
                wave::panel::editIndicators.end(), [](const auto& indicator) {
                    return indicator.buttonDiagnosticCode == 11
                           && indicator.ledSerialCode == 75;
                }),
            "Glide Edit is absent from the mutually-exclusive Edit lamp map");
    constexpr std::array lowerKeyboardSwitches {
        std::tuple { 110.0f, 766.5f, 3 },  // Button 1
        std::tuple { 170.0f, 766.5f, 4 },  // Button 2
        std::tuple { 288.0f, 849.0f, 12 }, // Octave Up
        std::tuple { 288.0f, 927.0f, 73 }  // Octave Down
    };
    for (const auto& [x, y, serial] : lowerKeyboardSwitches)
    {
        const auto* control = wave::panel::keyboardPanelSwitchAt(x, y);
        require(control != nullptr && control->diagnosticCode == serial,
                "An updated-SVG lower keyboard switch is absent from the serial map");
    }
    const auto* octaveUpLed = wave::panel::keyboardPanelLedAt(288.0f, 821.0f);
    const auto* octaveDownLed = wave::panel::keyboardPanelLedAt(288.0f, 900.0f);
    require(octaveUpLed != nullptr && octaveUpLed->redSerialCode == 58
                && octaveDownLed != nullptr
                && octaveDownLed->redSerialCode == 27,
            "Keyboard octave LEDs are absent from the serial-output map");
    const auto* knobModeSelect = wave::panel::switchAt(786.0f, 555.0f);
    require(knobModeSelect != nullptr
                && knobModeSelect->diagnosticCode == 20
                && wave::panel::physicalMatrixIndex(*knobModeSelect) == 20,
            "Knob Mode Select is absent from the canonical panel input map");
    constexpr std::array<float, 4> knobModeLedY { 488.0f, 501.0f, 513.0f, 526.0f };
    constexpr std::array<int, 4> knobModeLedSerials { 6, 70, 71, 7 };
    for (size_t mode = 0; mode < knobModeLedY.size(); ++mode)
    {
        const auto* led = wave::panel::ledAt(784.0f, knobModeLedY[mode]);
        require(led != nullptr && led->redSerialCode == knobModeLedSerials[mode]
                    && led->greenSerialCode < 0,
                "A Knob Mode lamp is absent from the canonical LED output map");
    }

    std::array<int, 128> dispatchCounts{};
    for (const auto serial : wave::panel::buttonDispatchCode)
    {
        require(serial >= 0 && serial <= 100,
                "An OS button dispatch serial is out of range");
        ++dispatchCounts[static_cast<size_t>(serial)];
    }
    for (size_t serial = 0; serial < dispatchCounts.size(); ++serial)
    {
        const auto expectedMaximum = serial == 100 ? 2 : 1;
        require(dispatchCounts[serial] <= expectedMaximum,
                "Two genuine OS button actions share one dispatch serial");
    }
    require(dispatchCounts[100] == 2,
            "The two unused OS button-dispatch sentinels changed");

    std::array<bool, 128> ledOutputs{};
    for (const auto& led : wave::panel::visibleLeds)
    {
        for (const auto serial : { led.redSerialCode, led.greenSerialCode })
        {
            if (serial < 0)
                continue;
            require(serial < 128, "A visible LED serial code is out of range");
            require(!ledOutputs[static_cast<size_t>(serial)],
                    "Two visible LEDs share one physical output");
            ledOutputs[static_cast<size_t>(serial)] = true;
        }
    }
    for (const auto& led : wave::panel::keyboardPanelLeds)
    {
        require(led.redSerialCode >= 0 && led.redSerialCode < 128,
                "A keyboard-panel LED serial code is out of range");
        require(!ledOutputs[static_cast<size_t>(led.redSerialCode)],
                "Two visible LEDs share one physical output");
        ledOutputs[static_cast<size_t>(led.redSerialCode)] = true;
    }

    std::array<int, 128> potDiagnosticCounts{};
    std::array<int, 128> potAdcCounts{};
    for (size_t index = 0; index < wave::panel::visiblePots.size(); ++index)
    {
        const auto& pot = wave::panel::visiblePots[index];
        require(pot.parameterId != nullptr
                    && std::string_view(pot.parameterId).size() > 0,
                "A visible pot has no parameter identity");
        require(pot.diagnosticCode >= 0 && pot.diagnosticCode < 128
                    && pot.adcChannel >= 0 && pot.adcChannel < 128,
                "A visible pot serial is out of range");
        require(pot.adcChannel == pot.diagnosticCode + 1,
                "A pot diagnostic serial and analogue mux channel disagree");
        for (size_t previous = 0; previous < index; ++previous)
            require(std::string_view(pot.parameterId)
                        != wave::panel::visiblePots[previous].parameterId,
                    "Two visible pot entries control the same parameter");
        ++potDiagnosticCounts[static_cast<size_t>(pot.diagnosticCode)];
        ++potAdcCounts[static_cast<size_t>(pot.adcChannel)];
    }

    // LFO Select rebinds these three physical pots between LFO 1 and LFO 2.
    // They are the only legitimate duplicate entries in the analogue map.
    constexpr std::array<int, 3> sharedLfoDiagnosticSerials { 4, 6, 21 };
    constexpr std::array<int, 3> sharedLfoAdcChannels { 5, 7, 22 };
    const auto isExpectedAlias = [](int serial, const auto& aliases) {
        return std::find(aliases.begin(), aliases.end(), serial) != aliases.end();
    };
    for (size_t serial = 0; serial < potDiagnosticCounts.size(); ++serial)
    {
        const auto count = potDiagnosticCounts[serial];
        if (count == 0)
            continue;
        require(count == (isExpectedAlias(static_cast<int>(serial),
                                          sharedLfoDiagnosticSerials) ? 2 : 1),
                "An analogue pot diagnostic serial is duplicated unexpectedly");
    }
    for (const auto serial : sharedLfoDiagnosticSerials)
        require(potDiagnosticCounts[static_cast<size_t>(serial)] == 2,
                "An intentional LFO pot diagnostic alias is incomplete");
    for (size_t channel = 0; channel < potAdcCounts.size(); ++channel)
    {
        const auto count = potAdcCounts[channel];
        if (count == 0)
            continue;
        require(count == (isExpectedAlias(static_cast<int>(channel),
                                          sharedLfoAdcChannels) ? 2 : 1),
                "An analogue pot ADC channel is duplicated unexpectedly");
    }
    for (const auto channel : sharedLfoAdcChannels)
        require(potAdcCounts[static_cast<size_t>(channel)] == 2,
                "An intentional LFO pot ADC alias is incomplete");

    std::array<bool, 128> faderAdcChannels{};
    for (const auto channel : wave::panel::performanceFaderAdcChannels)
    {
        require(channel >= 0 && channel < 128,
                "A Performance fader ADC channel is out of range");
        require(!faderAdcChannels[static_cast<size_t>(channel)],
                "Two Performance faders share one ADC channel");
        require(potAdcCounts[static_cast<size_t>(channel)] == 0,
                "A Performance fader and panel pot share one ADC channel");
        faderAdcChannels[static_cast<size_t>(channel)] = true;
    }

    std::array<bool, 128> encoderSerials{};
    for (const auto serial : wave::panel::encoderSerialCodes)
    {
        require(serial >= 0 && serial < 128,
                "A dial-board encoder serial is out of range");
        require(!encoderSerials[static_cast<size_t>(serial)],
                "Two dial-board encoders share one serial code");
        encoderSerials[static_cast<size_t>(serial)] = true;
    }

    const auto adcFor = [](const char* parameterId) {
        for (const auto& pot : wave::panel::visiblePots)
            if (std::string_view(pot.parameterId) == parameterId)
                return pot.adcChannel;
        return -1;
    };
    require(adcFor(wave::parameters::modulationAmount[wave::parameters::resonanceMod]) == 55
                && adcFor(wave::parameters::modulationAmount[wave::parameters::filterMod1]) == 47,
            "Resonance and cutoff modulation ADC channels are crossed");
    require(adcFor(wave::parameters::highpassVelocity) == 36
                && adcFor(wave::parameters::modulationAmount[wave::parameters::highpassMod2]) == 38,
            "High-pass panel pots are missing from the analogue multiplexer");
    require(adcFor(wave::parameters::modulationAmount[wave::parameters::panMod1]) == 53
                && adcFor(wave::parameters::modulationAmount[wave::parameters::panMod2]) == 43,
            "Panning modulation pots are not wired to their panel channels");

    constexpr std::array<int, 9> expectedEncoders { 12, 13, 14, 15, 11, 10, 9, 8, 0 };
    require(wave::panel::encoderSerialCodes == expectedEncoders,
            "Dial-board encoder serial order changed");
}
} // namespace

int main()
{
    try
    {
        testWavetableQuantisation();
        testAsicClockMixOverflowAndVcfSaturation();
        testAsicResampling();
        testHighRegisterOscillatorResampling();
        testCutoffControlLaw();
        testQuickEditFastAccessControls();
        testWaveEnvelopeTraversal();
        testWaveLfo();
        testDspMathTables();
        testPerformanceTuningTables();
        testFreeRunningEngineLfo();
        testFactorySetWhenAvailable();
        testPpgRomDecoding();
        testBundledPpgWavetables();
        testFactoryUpperWavetableBank();
        testExpandedFirst32Loading();
        testUserPpgRomWhenAvailable();
        testCemStability();
        testCemFilterResponse();
        testMeasuredWaveResonancePassbandLoss();
        testAllVoiceFiltersAreCalibrated();
        testCemControlVoltageSettling();
        testLiveCutoffUsesContinuousBaseControlVoltage();
        testAsicHighpassResponse();
        testSerialBandpassTopology();
        testCemSelfOscillation();
        testCemResonanceAcrossSampleRates();
        testCemSmallSignalResonanceResponse();
        testMeasuredFastAmplifierAttackScaling();
        testAmplifierEnvelopeStages();
        testShortVcaReleaseDrainsAnalogueControl();
        testIndependentFilterEnvelope();
        testCentredVoiceHasNoArtificialPanSpread();
        testSampleAccurateMidiStart();
        testAudibleKeyboardRange();
        testPerformanceMidi();
        testWaveGlideModes();
        testReleasedVoicesAreStolenBeforeHeldChord();
        testVoiceStealPreservesAnalogueHandover();
        testFullThreeCardPolyphony();
        testParallelVoiceCardsMatchSerialRenderer();
        testLfoLevelModifierGate();
        testOscillatorLinkUsesOscillatorOneModulation();
        testPerformanceControlXFeedsLfoRoutes();
        testReferenceComparison();
        test68000ExecutionCore();
        test6522TimerOneInterruptPath();
        testDosFloppyImageCreation();
        testDp8473MountedDiskImage();
        testLcdFramebuffer();
        testCompletePanelWiringContract();
        testDecodedVoiceBoardProtocol();
        testVoiceBoardWaveRamDecode();
        testOfficialFirmwareWhenAvailable();
        std::cout << "WaveCoreTests: all checks passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "WaveCoreTests: " << error.what() << '\n';
        return 1;
    }
}
