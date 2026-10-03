#include "PluginProcessor.h"

#include "PanelWiring.h"
#include "PluginEditor.h"
#include "Firmware/DosFloppyImage.h"

#include <BinaryData.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <type_traits>

namespace
{
constexpr int factoryStateSchemaVersion = 1;
constexpr int machineStateSchemaVersion = 2;
constexpr int machineEditBufferMagic = 0x57415645; // "WAVE"
constexpr uint32_t firmwareEditPerformanceOffset = 0x5400u;
constexpr auto hostStateDirectoryName = "Waldorf Wave Host State";

juce::File defaultFirmwarePreferenceFile()
{
    auto directory = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory);
   #if JUCE_MAC
    directory = directory.getChildFile("Application Support");
   #endif
    return directory.getChildFile("DJW/Wave Emulation/firmware-folder.txt");
}

constexpr std::array panelOperatingPageButtonCodes {
    38, 33, 35, 34, 32, 37, 36, 39
};

void releasePanelPageButtonLatches(wave::firmware::MasterFirmwareRuntime& firmware)
{
    for (const auto code : panelOperatingPageButtonCodes)
        firmware.releasePanelEventLatch(code);
    // Group Edit is a page-changing serial action while Instrument Edit or
    // External Edit owns the display.  It is not an operating mode, but it
    // needs the same discrete press/release normalisation as those buttons.
    firmware.releasePanelEventLatch(31);
    for (const auto& indicator : wave::panel::editIndicators)
        firmware.releasePanelEventLatch(indicator.buttonDiagnosticCode);
}

bool isPanelEditPageButton(int diagnosticCode) noexcept
{
    return std::any_of(
        wave::panel::editIndicators.begin(), wave::panel::editIndicators.end(),
        [diagnosticCode](const auto& indicator) {
            return indicator.buttonDiagnosticCode == diagnosticCode;
        });
}

bool isInsideHostStateDirectory(juce::File file)
{
    while (file != juce::File{})
    {
        if (file.getFileName() == hostStateDirectoryName)
            return true;
        const auto parent = file.getParentDirectory();
        if (parent == file)
            break;
        file = parent;
    }
    return false;
}

struct ModulationRecordLayout
{
    wave::parameters::ModulationRouteIndex route;
    int sourceOffset;
    int controlOffset;
    int amountOffset;
    bool volumeAmount = false;
};

constexpr std::array modulationRecordLayouts {
    ModulationRecordLayout { wave::parameters::osc1PitchMod1, 5, 6, 7 },
    ModulationRecordLayout { wave::parameters::osc1PitchMod2, 8, -1, 9 },
    ModulationRecordLayout { wave::parameters::osc2PitchMod1, 17, 18, 19 },
    ModulationRecordLayout { wave::parameters::osc2PitchMod2, 20, -1, 21 },
    ModulationRecordLayout { wave::parameters::wave1StartMod, 28, -1, 29 },
    ModulationRecordLayout { wave::parameters::wave1Mod1, 34, 35, 36 },
    ModulationRecordLayout { wave::parameters::wave1Mod2, 37, -1, 38 },
    ModulationRecordLayout { wave::parameters::wave2StartMod, 44, -1, 45 },
    ModulationRecordLayout { wave::parameters::wave2Mod1, 50, 51, 52 },
    ModulationRecordLayout { wave::parameters::wave2Mod2, 53, -1, 54 },
    ModulationRecordLayout { wave::parameters::wave1VolumeMod, 62, -1, 63, true },
    ModulationRecordLayout { wave::parameters::wave2VolumeMod, 64, -1, 65, true },
    ModulationRecordLayout { wave::parameters::noiseVolumeMod, 66, -1, 67, true },
    ModulationRecordLayout { wave::parameters::amplifierMod1, 72, 73, 74 },
    ModulationRecordLayout { wave::parameters::amplifierMod2, 75, -1, 76 },
    ModulationRecordLayout { wave::parameters::filterMod1, 85, 86, 87 },
    ModulationRecordLayout { wave::parameters::filterMod2, 88, -1, 89 },
    ModulationRecordLayout { wave::parameters::resonanceMod, 90, 91, 92 },
    ModulationRecordLayout { wave::parameters::highpassMod1, 99, 100, 101 },
    ModulationRecordLayout { wave::parameters::highpassMod2, 102, -1, 103 },
    ModulationRecordLayout { wave::parameters::panMod1, 194, 195, 196 },
    ModulationRecordLayout { wave::parameters::panMod2, 197, -1, 198 },
    ModulationRecordLayout { wave::parameters::lfo1RateMod, 176, -1, 177 },
    ModulationRecordLayout { wave::parameters::lfo1LevelMod, 178, 179, 180 },
    ModulationRecordLayout { wave::parameters::lfo2RateMod, 186, -1, 187 },
    ModulationRecordLayout { wave::parameters::lfo2LevelMod, 188, 189, 190 }
};

enum class PanelPotEncoding
{
    unsignedSevenBit,
    signedAround64,
    semitoneQuarter,
    level112,
    unit127,
    cutoffFrequency,
    envelopeTime,
    amplifierAttack,
    envelopeDelay
};

struct PanelPotRecordBinding
{
    uint8_t soundOffset;
    PanelPotEncoding encoding;
};

constexpr auto unsigned7 = PanelPotEncoding::unsignedSevenBit;
constexpr auto signed64 = PanelPotEncoding::signedAround64;
constexpr auto quarterTone = PanelPotEncoding::semitoneQuarter;
constexpr auto level112 = PanelPotEncoding::level112;
constexpr auto unit127 = PanelPotEncoding::unit127;
constexpr auto cutoffFrequency = PanelPotEncoding::cutoffFrequency;
constexpr auto envelopeTimeEncoding = PanelPotEncoding::envelopeTime;
constexpr auto amplifierAttackEncoding = PanelPotEncoding::amplifierAttack;
constexpr auto envelopeDelay = PanelPotEncoding::envelopeDelay;

// Same physical order as panel::visiblePots. Each entry is the native Sound
// field changed by OS 1.700 after applying the selected Knob Mode.
constexpr std::array panelPotRecordBindings {
    PanelPotRecordBinding { 2, signed64 }, PanelPotRecordBinding { 7, signed64 },
    PanelPotRecordBinding { 1, quarterTone }, PanelPotRecordBinding { 9, signed64 },
    PanelPotRecordBinding { 27, unsigned7 }, PanelPotRecordBinding { 31, signed64 },
    PanelPotRecordBinding { 36, signed64 }, PanelPotRecordBinding { 26, unsigned7 },
    PanelPotRecordBinding { 30, signed64 }, PanelPotRecordBinding { 32, signed64 },
    PanelPotRecordBinding { 38, signed64 }, PanelPotRecordBinding { 59, level112 },
    PanelPotRecordBinding { 14, signed64 }, PanelPotRecordBinding { 19, signed64 },
    PanelPotRecordBinding { 13, quarterTone }, PanelPotRecordBinding { 21, signed64 },
    PanelPotRecordBinding { 43, unsigned7 }, PanelPotRecordBinding { 47, signed64 },
    PanelPotRecordBinding { 52, signed64 }, PanelPotRecordBinding { 42, unsigned7 },
    PanelPotRecordBinding { 46, signed64 }, PanelPotRecordBinding { 48, signed64 },
    PanelPotRecordBinding { 54, signed64 }, PanelPotRecordBinding { 60, level112 },
    PanelPotRecordBinding { 61, level112 },
    PanelPotRecordBinding { 172, unsigned7 }, PanelPotRecordBinding { 177, signed64 },
    PanelPotRecordBinding { 180, signed64 }, PanelPotRecordBinding { 182, unsigned7 },
    PanelPotRecordBinding { 187, signed64 }, PanelPotRecordBinding { 190, signed64 },
    PanelPotRecordBinding { 80, unit127 }, PanelPotRecordBinding { 82, signed64 },
    PanelPotRecordBinding { 92, signed64 }, PanelPotRecordBinding { 87, signed64 },
    PanelPotRecordBinding { 79, cutoffFrequency }, PanelPotRecordBinding { 81, signed64 },
    PanelPotRecordBinding { 83, signed64 }, PanelPotRecordBinding { 89, signed64 },
    PanelPotRecordBinding { 96, signed64 }, PanelPotRecordBinding { 95, signed64 },
    PanelPotRecordBinding { 97, signed64 }, PanelPotRecordBinding { 101, signed64 },
    PanelPotRecordBinding { 103, signed64 },
    PanelPotRecordBinding { 119, envelopeDelay },
    PanelPotRecordBinding { 120, envelopeTimeEncoding },
    PanelPotRecordBinding { 121, envelopeTimeEncoding }, PanelPotRecordBinding { 122, unit127 },
    PanelPotRecordBinding { 123, envelopeTimeEncoding },
    PanelPotRecordBinding { 106, amplifierAttackEncoding },
    PanelPotRecordBinding { 107, envelopeTimeEncoding }, PanelPotRecordBinding { 108, unit127 },
    PanelPotRecordBinding { 109, envelopeTimeEncoding },
    PanelPotRecordBinding { 196, signed64 }, PanelPotRecordBinding { 198, signed64 }
};
static_assert(panelPotRecordBindings.size() == wave::panel::visiblePots.size());

float decodePanelPotValue(const PanelPotRecordBinding& binding,
                          uint8_t storedValue) noexcept
{
    const auto value = static_cast<float>(storedValue & 0x7fu);
    switch (binding.encoding)
    {
        case PanelPotEncoding::unsignedSevenBit: return value;
        case PanelPotEncoding::signedAround64: return value - 64.0f;
        case PanelPotEncoding::semitoneQuarter: return (value - 64.0f) / 4.0f;
        case PanelPotEncoding::level112: return value / 112.0f;
        case PanelPotEncoding::unit127: return value / 127.0f;
        case PanelPotEncoding::cutoffFrequency:
            return wave::parameters::cutoffFrequencyForStep(value);
        case PanelPotEncoding::envelopeTime:
            return wave::dsp::WdvEnvelope::timeConstantForRate(
                static_cast<uint8_t>(value));
        case PanelPotEncoding::amplifierAttack:
            return wave::dsp::WdvEnvelope::amplifierAttackTimeConstantForRate(
                static_cast<uint8_t>(value));
        case PanelPotEncoding::envelopeDelay:
            return value == 0.0f
                       ? 0.0f
                       : wave::dsp::WdvEnvelope::timeConstantForRate(
                             static_cast<uint8_t>(value));
    }
    return 0.0f;
}

uint8_t encodePanelPotValue(const PanelPotRecordBinding& binding,
                            float value) noexcept
{
    auto stored = 0;
    switch (binding.encoding)
    {
        case PanelPotEncoding::unsignedSevenBit:
            stored = juce::roundToInt(value);
            break;
        case PanelPotEncoding::signedAround64:
            stored = juce::roundToInt(value + 64.0f);
            break;
        case PanelPotEncoding::semitoneQuarter:
            stored = juce::roundToInt(value * 4.0f + 64.0f);
            break;
        case PanelPotEncoding::level112:
            stored = juce::roundToInt(value * 112.0f);
            break;
        case PanelPotEncoding::unit127:
            stored = juce::roundToInt(value * 127.0f);
            break;
        case PanelPotEncoding::cutoffFrequency:
            stored = juce::roundToInt(
                wave::parameters::cutoffStepForFrequency(value));
            break;
        case PanelPotEncoding::envelopeDelay:
            if (value <= 0.0f)
                return 0u;
            [[fallthrough]];
        case PanelPotEncoding::envelopeTime:
        case PanelPotEncoding::amplifierAttack:
        {
            auto nearestError = std::numeric_limits<float>::max();
            for (auto rate = 0; rate < 128; ++rate)
            {
                const auto candidate = binding.encoding
                                               == PanelPotEncoding::amplifierAttack
                                           ? wave::dsp::WdvEnvelope::
                                                 amplifierAttackTimeConstantForRate(
                                                     static_cast<uint8_t>(rate))
                                           : wave::dsp::WdvEnvelope::timeConstantForRate(
                                                 static_cast<uint8_t>(rate));
                const auto error = std::abs(candidate - value);
                if (error < nearestError)
                {
                    nearestError = error;
                    stored = rate;
                }
            }
            break;
        }
    }
    return static_cast<uint8_t>(juce::jlimit(0, 127, stored));
}

void preserveUnexposedControlFields(wave::parameters::Snapshot& edited,
                                    const wave::parameters::Snapshot& native)
{
    edited.controlSampleAndHoldSource = native.controlSampleAndHoldSource;
    edited.controlSampleAndHoldRate = native.controlSampleAndHoldRate;
    edited.controlSampleAndHoldRateModSource = native.controlSampleAndHoldRateModSource;
    edited.controlSampleAndHoldRateModAmount = native.controlSampleAndHoldRateModAmount;
    edited.controlComparatorSource = native.controlComparatorSource;
    edited.controlComparatorThreshold = native.controlComparatorThreshold;
}

wave::parameters::Snapshot decodeFactorySound(
    std::span<const uint8_t, wave::presets::WaveFactorySet::soundSize> sound)
{
    using namespace wave::parameters;
    Snapshot result;
    const auto sevenBit = [](uint8_t value) { return static_cast<int>(value & 0x7fu); };
    const auto signedAmount = [&sevenBit](uint8_t value) {
        return static_cast<float>(sevenBit(value) - 64);
    };
    const auto envelopeTime = [](uint8_t value) {
        return wave::dsp::WdvEnvelope::timeConstantForRate(value);
    };
    const auto filterFrequency = [](uint8_t value) {
        return 20.0f * std::pow(2.0f, static_cast<float>(value) / 12.0f);
    };
    const auto setRoute = [&](ModulationRouteIndex route, int sourceOffset,
                              int controlOffset, int amountOffset,
                              bool volumeAmount = false) {
        auto& destination = result.modulationRoutes[static_cast<size_t>(route)];
        destination.source = juce::jlimit(0, 39, sevenBit(sound[static_cast<size_t>(sourceOffset)]));
        destination.control = controlOffset >= 0
                                  ? juce::jlimit(0, 39, sevenBit(sound[static_cast<size_t>(controlOffset)]))
                                  : 38;
        destination.amount = volumeAmount
                                 ? static_cast<float>(sevenBit(sound[static_cast<size_t>(amountOffset)]) / 8 - 8)
                                 : signedAmount(sound[static_cast<size_t>(amountOffset)]);
    };

    result.wavetableIndex = sevenBit(sound[25]);
    result.oscillatorLinkEnabled = sevenBit(sound[23]) != 0;
    result.wavePosition = static_cast<float>(sevenBit(sound[26]));
    result.waveScan = signedAmount(sound[30]);
    result.wavePosition2 = static_cast<float>(sevenBit(sound[42]));
    result.waveScan2 = signedAmount(sound[46]);
    result.oscillatorOctaves[0]
        = juce::jlimit(-2, 2, sevenBit(sound[0]) / 16 - 2);
    result.oscillatorOctaves[1]
        = juce::jlimit(-2, 2, sevenBit(sound[12]) / 16 - 2);
    result.oscillatorSemitones[0] = signedAmount(sound[1]) / 4.0f;
    result.oscillatorSemitones[1] = signedAmount(sound[13]) / 4.0f;
    const auto bendRange = [&sevenBit](uint8_t value) {
        const auto stored = sevenBit(value);
        // Raw 12 delegates to the global Bend Range. The current global
        // controller model is the hardware default of two semitones.
        return stored == 12 ? 2.0f : static_cast<float>(stored - 64) / 4.0f;
    };
    result.oscillatorBendRanges = { bendRange(sound[3]), bendRange(sound[15]) };
    result.oscillatorDetuneCents[0] = signedAmount(sound[2]);
    result.oscillatorDetuneCents[1] = signedAmount(sound[14]);
    result.wavePhases[0] = static_cast<float>(sevenBit(sound[27]));
    result.wavePhases[1] = static_cast<float>(sevenBit(sound[43]));
    result.waveEnvelopeVelocityAmounts[0] = signedAmount(sound[31]);
    result.waveEnvelopeVelocityAmounts[1] = signedAmount(sound[47]);
    result.waveKeytrackAmounts[0] = signedAmount(sound[32]);
    result.waveKeytrackAmounts[1] = signedAmount(sound[48]);
    for (size_t point = 0; point < result.waveEnvelopeTimes.size(); ++point)
    {
        result.waveEnvelopeTimes[point] = static_cast<float>(sevenBit(sound[135 + point * 2]));
        result.waveEnvelopeLevels[point] = static_cast<float>(sevenBit(sound[136 + point * 2]));
    }
    result.waveEnvelopeKeyOffPoint = juce::jlimit(0, 7, sevenBit(sound[155]));
    result.waveEnvelopeLoopStartPoint = juce::jlimit(0, 7, sevenBit(sound[156]));
    result.waveEnvelopeLoop = sound[157] != 0;
    for (size_t lfo = 0; lfo < result.lfos.size(); ++lfo)
    {
        const auto base = static_cast<size_t>(172 + lfo * 10);
        auto& destination = result.lfos[lfo];
        destination.rate = static_cast<float>(sevenBit(sound[base]));
        destination.shape = juce::jlimit(0, 5, sevenBit(sound[base + 1]));
        destination.symmetry = signedAmount(sound[base + 2]);
        destination.humanize = juce::jlimit(0, 7, sevenBit(sound[base + 3]));
        destination.sync = juce::jlimit(0, 2, sevenBit(sound[base + 9]));
        const auto phaseOffset = lfo == 0 ? 225u : 232u;
        destination.phaseDegrees = static_cast<float>(juce::jlimit(0, 90,
                                                                   sevenBit(sound[phaseOffset])) * 4);
    }
    result.glideTypeMode = juce::jlimit(1, 6, sevenBit(sound[233]));
    result.glideRateValue = static_cast<float>(sevenBit(sound[234]));
    result.glideTimeModeValue = juce::jlimit(0, 1, sevenBit(sound[235]));
    result.glideRateModulationSource
        = juce::jlimit(0, 39, sevenBit(sound[236]));
    result.glideRateModulationAmount = signedAmount(sound[237]);
    result.glideEnabled = sevenBit(sound[238]) != 0;

    // Native Sound record offsets from the Wave SysEx specification. These
    // control modules are modulation sources in their own right; A013 uses
    // S&H -> Comparator Positive to switch its single instrument across the
    // stereo field.
    result.controlComparatorSource = juce::jlimit(0, 39, sevenBit(sound[199]));
    result.controlComparatorThreshold = signedAmount(sound[200]);
    result.controlSampleAndHoldSource = juce::jlimit(0, 39, sevenBit(sound[221]));
    result.controlSampleAndHoldRate = static_cast<float>(sevenBit(sound[222]));
    result.controlSampleAndHoldRateModSource
        = juce::jlimit(0, 39, sevenBit(sound[223]));
    result.controlSampleAndHoldRateModAmount = signedAmount(sound[224]);

    for (const auto& layout : modulationRecordLayouts)
        setRoute(layout.route, layout.sourceOffset, layout.controlOffset,
                 layout.amountOffset, layout.volumeAmount);

    const auto waveOneLevel = static_cast<float>(sevenBit(sound[59])) / 112.0f;
    const auto waveTwoLevel = static_cast<float>(sevenBit(sound[60])) / 112.0f;
    result.waveLevels = { waveOneLevel, waveTwoLevel };
    const auto oscillatorLevel = waveOneLevel + waveTwoLevel;
    result.oscillatorBalance = oscillatorLevel > 0.0f
                                   ? waveTwoLevel / oscillatorLevel : 0.5f;
    result.detuneCents = juce::jlimit(-50.0f, 50.0f,
                                      static_cast<float>(sevenBit(sound[14])
                                                         - sevenBit(sound[2])));
    result.noiseLevel = juce::jlimit(0.0f, 1.0f,
                                     static_cast<float>(sevenBit(sound[61])) / 112.0f);
    result.filterMode = juce::jlimit(0, 3, sevenBit(sound[78]));
    result.cutoffHz = filterFrequency(sound[79]);
    result.resonanceAmount = static_cast<float>(sevenBit(sound[80])) / 127.0f;
    result.filterEnvelopeSemitones = signedAmount(sound[81]);
    result.filterVelocitySemitones = signedAmount(sound[82]);
    result.filterKeytrackAmount = signedAmount(sound[83]);
    result.filterKeyCenterNote = sevenBit(sound[84]);
    result.attackSeconds
        = wave::dsp::WdvEnvelope::amplifierAttackTimeConstantForRate(sound[106]);
    result.decaySeconds = envelopeTime(sound[107]);
    result.sustainLevel = static_cast<float>(sevenBit(sound[108])) / 127.0f;
    result.releaseSeconds = envelopeTime(sound[109]);
    result.filterDelaySeconds = sound[119] == 0 ? 0.0f : envelopeTime(sound[119]);
    result.filterAttackSeconds = envelopeTime(sound[120]);
    result.filterDecaySeconds = envelopeTime(sound[121]);
    result.filterSustainLevel = static_cast<float>(sevenBit(sound[122])) / 127.0f;
    result.filterReleaseSeconds = envelopeTime(sound[123]);
    for (size_t stage = 0; stage < result.filterEnvelopeModSources.size(); ++stage)
    {
        result.filterEnvelopeModSources[stage] = juce::jlimit(
            0, 39, sevenBit(sound[126 + stage * 2]));
        result.filterEnvelopeModAmounts[stage] = signedAmount(sound[127 + stage * 2]);
    }
    result.highpassCutoffHz = filterFrequency(sound[93]);
    result.highpassEnvelopeSelector = juce::jlimit(0, 3, sevenBit(sound[94]));
    result.highpassEnvelopeSemitones = signedAmount(sound[95]);
    result.highpassVelocitySemitones = signedAmount(sound[96]);
    result.highpassKeytrackAmount = signedAmount(sound[97]);
    result.highpassKeyCenterNote = sevenBit(sound[98]);
    result.bandpassBandwidthSemitones = static_cast<float>(sevenBit(sound[104]));
    for (size_t point = 0; point < result.freeEnvelopeTimes.size(); ++point)
    {
        result.freeEnvelopeTimes[point] = static_cast<float>(sevenBit(sound[159 + point * 2]));
        result.freeEnvelopeLevels[point] = signedAmount(sound[160 + point * 2]);
    }
    result.freeEnvelopeZeroAxis = signedAmount(sound[171]);
    return result;
}

bool applyFirmwareModifierRouting(
    wave::parameters::Snapshot& sound,
    wave::firmware::MasterFirmwareRuntime& firmware,
    std::array<int, wave::parameters::modulationRouteCount>& lastFirmwareSources,
    std::array<int, wave::parameters::modulationRouteCount>& lastHostSources,
    std::array<int, wave::parameters::modulationRouteCount>& effectiveSources,
    std::array<int, wave::parameters::modulationRouteCount>& lastFirmwareControls,
    std::array<int, wave::parameters::modulationRouteCount>& lastHostControls,
    std::array<int, wave::parameters::modulationRouteCount>& effectiveControls,
    std::array<int, wave::parameters::modulationRouteCount>& lastFirmwareAmounts,
    std::array<float, wave::parameters::modulationRouteCount>& lastHostAmounts,
    std::array<float, wave::parameters::modulationRouteCount>& effectiveAmounts,
    std::array<std::atomic<int>, wave::parameters::modulationRouteCount>&
        pendingParameterSources,
    std::array<std::atomic<int>, wave::parameters::modulationRouteCount>&
        pendingParameterControls,
    std::array<std::atomic<int>, wave::parameters::modulationRouteCount>&
        pendingParameterAmounts,
    bool resetRoutingTracking) noexcept
{
    if (!firmware.isLoaded())
        return false;

    const auto currentSound = firmware.currentSoundRecordOffset();
    if (!currentSound.has_value())
        return false;

    // The genuine OS owns the selected Instrument's live Sound record. Its
    // address moves as performances and instruments change, so all three
    // route fields must follow that record rather than the $5300 boot copy.
    auto parameterSyncPending = false;
    for (const auto& layout : modulationRecordLayouts)
    {
        const auto routeIndex = static_cast<size_t>(layout.route);
        auto& route = sound.modulationRoutes[routeIndex];
        const auto firmwareSource = juce::jlimit(
            0, 39, static_cast<int>(firmware.sharedProgramByte(
                       *currentSound + static_cast<uint32_t>(layout.sourceOffset))
                                   & 0x7fu));
        const auto firmwareControl
            = layout.controlOffset >= 0
                  ? juce::jlimit(
                        0, 39,
                        static_cast<int>(firmware.sharedProgramByte(
                            *currentSound + static_cast<uint32_t>(layout.controlOffset))
                                         & 0x7fu))
                  : 38;
        const auto hostSource = route.source;
        const auto hostControl = route.control;
        const auto initialSource
            = resetRoutingTracking || lastFirmwareSources[routeIndex] < 0;
        const auto hostSourceChanged
            = !initialSource && hostSource != lastHostSources[routeIndex];
        auto storedSource = firmwareSource;
        if (initialSource)
        {
            effectiveSources[routeIndex] = firmwareSource;
            if (hostSource != firmwareSource)
            {
                pendingParameterSources[routeIndex].store(
                    firmwareSource, std::memory_order_release);
                parameterSyncPending = true;
            }
        }
        else if (firmwareSource != lastFirmwareSources[routeIndex])
        {
            effectiveSources[routeIndex] = firmwareSource;
            pendingParameterSources[routeIndex].store(
                firmwareSource, std::memory_order_release);
            parameterSyncPending = true;
        }
        else if (hostSourceChanged)
            effectiveSources[routeIndex] = hostSource;
        if (!initialSource && hostSourceChanged)
        {
            storedSource = juce::jlimit(0, 39, hostSource);
            firmware.writeCurrentSoundRecordByte(
                static_cast<uint32_t>(layout.sourceOffset),
                static_cast<uint8_t>(storedSource));
        }
        route.source = effectiveSources[routeIndex];
        lastFirmwareSources[routeIndex] = storedSource;
        lastHostSources[routeIndex] = hostSource;

        const auto initialControl
            = resetRoutingTracking || lastFirmwareControls[routeIndex] < 0;
        const auto hostControlChanged
            = !initialControl && hostControl != lastHostControls[routeIndex];
        auto storedControl = firmwareControl;
        if (initialControl)
        {
            effectiveControls[routeIndex] = firmwareControl;
            if (hostControl != firmwareControl)
            {
                pendingParameterControls[routeIndex].store(
                    firmwareControl, std::memory_order_release);
                parameterSyncPending = true;
            }
        }
        else if (firmwareControl != lastFirmwareControls[routeIndex])
        {
            effectiveControls[routeIndex] = firmwareControl;
            pendingParameterControls[routeIndex].store(
                firmwareControl, std::memory_order_release);
            parameterSyncPending = true;
        }
        else if (hostControlChanged)
            effectiveControls[routeIndex] = hostControl;
        if (layout.controlOffset >= 0 && !initialControl && hostControlChanged)
        {
            storedControl = juce::jlimit(0, 39, hostControl);
            firmware.writeCurrentSoundRecordByte(
                static_cast<uint32_t>(layout.controlOffset),
                static_cast<uint8_t>(storedControl));
        }
        route.control = effectiveControls[routeIndex];
        lastFirmwareControls[routeIndex] = storedControl;
        lastHostControls[routeIndex] = hostControl;
        const auto storedAmount = static_cast<int>(firmware.sharedProgramByte(
            *currentSound + static_cast<uint32_t>(layout.amountOffset))
                                                   & 0x7fu);
        const auto firmwareAmount = layout.volumeAmount
                                        ? static_cast<float>(storedAmount / 8 - 8)
                                        : static_cast<float>(storedAmount - 64);
        const auto hostAmount = route.amount;
        const auto initialSample
            = resetRoutingTracking || lastFirmwareAmounts[routeIndex] < 0;
        const auto firmwareChanged
            = !initialSample && storedAmount != lastFirmwareAmounts[routeIndex];
        const auto hostChanged
            = !std::isnan(lastHostAmounts[routeIndex])
              && std::abs(hostAmount - lastHostAmounts[routeIndex]) > 1.0e-5f;

        auto effectiveStoredAmount = storedAmount;
        if (initialSample)
        {
            effectiveAmounts[routeIndex] = firmwareAmount;
            if (std::abs(hostAmount - firmwareAmount) > 1.0e-5f)
            {
                pendingParameterAmounts[routeIndex].store(
                    juce::roundToInt(firmwareAmount), std::memory_order_release);
                parameterSyncPending = true;
            }
        }
        else if (firmwareChanged)
        {
            // The contextual LCD fader changed the genuine edit buffer. Use
            // it in this audio block, then mirror it to APVTS on the message
            // thread so state saving and host displays see the same value.
            effectiveAmounts[routeIndex] = firmwareAmount;
            pendingParameterAmounts[routeIndex].store(
                juce::roundToInt(firmwareAmount), std::memory_order_release);
            parameterSyncPending = true;
        }
        else if (hostChanged)
            effectiveAmounts[routeIndex] = hostAmount;

        if (!initialSample && hostChanged)
        {
            effectiveStoredAmount = layout.volumeAmount
                                        ? (juce::roundToInt(hostAmount) + 8) * 8
                                        : juce::roundToInt(hostAmount) + 64;
            effectiveStoredAmount = juce::jlimit(0, 127, effectiveStoredAmount);
            firmware.writeCurrentSoundRecordByte(
                static_cast<uint32_t>(layout.amountOffset),
                static_cast<uint8_t>(effectiveStoredAmount));
        }

        route.amount = effectiveAmounts[routeIndex];
        lastFirmwareAmounts[routeIndex] = effectiveStoredAmount;
        lastHostAmounts[routeIndex] = hostAmount;
    }
    return parameterSyncPending;
}
} // namespace

WaveEmulationAudioProcessor::WaveEmulationAudioProcessor(const juce::File& preferenceFile,
                                                       InitialBank initialBank)
    : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      parameters(*this, nullptr, "WaveState", wave::parameters::createLayout()),
      firmwarePreferenceFile(preferenceFile == juce::File{}
                                 ? defaultFirmwarePreferenceFile() : preferenceFile)
{
    for (auto& value : pendingFirmwareFaderValues)
        value.store(-1, std::memory_order_relaxed);
    for (auto& value : pendingInstrumentFaderEdits)
        value.store(-1, std::memory_order_relaxed);
    for (auto& value : pendingPanelEncoderSteps)
        value.store(0, std::memory_order_relaxed);
    for (auto& value : pendingFirmwareModulationAmounts)
        value.store(-128, std::memory_order_relaxed);
    for (auto& value : pendingFirmwareModulationSources)
        value.store(-1, std::memory_order_relaxed);
    for (auto& value : pendingFirmwareModulationControls)
        value.store(-1, std::memory_order_relaxed);
    for (auto& value : pendingFirmwarePanelPotValues)
        value.store(std::numeric_limits<float>::quiet_NaN(),
                    std::memory_order_relaxed);
    for (auto& value : programRecallPanelPotEchoValues)
        value.store(std::numeric_limits<float>::quiet_NaN(),
                    std::memory_order_relaxed);
    for (auto& value : pendingFirmwareGlideValues)
        value.store(-1000, std::memory_order_relaxed);
    for (auto& value : pendingGlideFaderEdits)
        value.store(-1, std::memory_order_relaxed);
    lastFirmwarePanelPotRecordBytes.fill(-1);
    lastHostPanelPotValues.fill(std::numeric_limits<float>::quiet_NaN());
    lastFirmwareGlideRecordBytes.fill(-1);
    lastFirmwareModulationSources.fill(-1);
    lastHostModulationSources.fill(-1);
    effectiveModulationSources.fill(38);
    lastFirmwareModulationControls.fill(-1);
    lastHostModulationControls.fill(-1);
    effectiveModulationControls.fill(38);
    lastFirmwareModulationAmounts.fill(-1);
    lastHostModulationAmounts.fill(std::numeric_limits<float>::quiet_NaN());
    effectiveModulationAmounts.fill(0.0f);
    for (auto& down : panelButtonDown)
        down.store(false, std::memory_order_relaxed);
    for (auto& channels : activeMidiNoteChannels)
        channels.store(0, std::memory_order_relaxed);
    for (auto& octave : panelOscillatorOctaves)
        octave.store(0, std::memory_order_relaxed);
    localKeyboardTransposedNotes.fill(-1);
    masterFirmware.attachSharedMemory(sharedFirmwareMemory);
    for (int board = 0; board < static_cast<int>(voiceFirmwares.size()); ++board)
    {
        auto& runtime = voiceFirmwares[static_cast<size_t>(board)];
        runtime.attachSharedMemory(sharedFirmwareMemory);
        runtime.setBoardIndex(board);
    }
    if (initialBank == InitialBank::embeddedFactory)
        loadEmbeddedFactorySet();
    loadEmbeddedPrivateRoms();
    if (initialBank == InitialBank::embeddedFactory)
        applyFactoryProgram(0, false);
    loadRememberedFirmware();
    if (initialBank == InitialBank::empty)
    {
        resetToColdStart();
        // A fresh instance inserts an empty disk, with no imported sound bank.
        // Host/session recall subsequently replaces this with its own image.
        restoredHostStateDirectory = juce::File::getSpecialLocation(juce::File::tempDirectory)
            .getChildFile(hostStateDirectoryName).getChildFile(hostStateInstanceId);
        if (restoredHostStateDirectory.createDirectory().wasOk())
            (void) createBlankDiskImage(restoredHostStateDirectory.getChildFile("Blank Wave.img"));
    }
}

WaveEmulationAudioProcessor::~WaveEmulationAudioProcessor()
{
    cancelPendingUpdate();
    // Persist any sector writes made by the Wave disk pages.  The controller
    // never performs host filesystem I/O on the audio thread.
    masterFirmware.flushDiskImage();
    removeRestoredHostStateDisk();
}

void WaveEmulationAudioProcessor::removeRestoredHostStateDisk() noexcept
{
    if (restoredHostStateDirectory == juce::File{})
        return;

    const auto mounted = masterFirmware.mountedDiskImageFile();
    if (mounted != juce::File{}
        && mounted.getFullPathName().startsWith(
            restoredHostStateDirectory.getFullPathName()
            + juce::File::getSeparatorString()))
        (void) masterFirmware.ejectDiskImage();

    if (restoredHostStateDirectory.exists())
        (void) restoredHostStateDirectory.deleteRecursively();
    restoredHostStateDirectory = juce::File{};
}

juce::File WaveEmulationAudioProcessor::getDiskImageChooserDirectory() const
{
    const auto remembered = juce::File(
        parameters.state.getProperty("diskImageChooserDirectory").toString());
    if (remembered.isDirectory() && !isInsideHostStateDirectory(remembered))
        return remembered;

    const auto mounted = masterFirmware.mountedDiskImageFile();
    if (mounted.existsAsFile() && !isInsideHostStateDirectory(mounted))
        return mounted.getParentDirectory();

    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
}

juce::Result WaveEmulationAudioProcessor::mountDiskImage(const juce::File& image)
{
    returnToPerformanceAfterDiskImport.store(false,
                                             std::memory_order_release);
    storeSaveCompletionPending.store(false, std::memory_order_release);
    storeExitModePending.store(false,
                                            std::memory_order_release);
    pendingDiskSetBytes.store(0, std::memory_order_release);
    pendingDiskSetActivation.store(false, std::memory_order_release);
    pendingManagerExitProgram.store(-1, std::memory_order_release);
    diskSetImportConfirmationPending.store(false,
                                           std::memory_order_release);
    const auto result = masterFirmware.mountDiskImage(image);
    if (result.wasOk())
    {
        if (!isInsideHostStateDirectory(image))
            removeRestoredHostStateDisk();
        parameters.state.setProperty("mountedDiskImage", image.getFullPathName(), nullptr);
        if (!isInsideHostStateDirectory(image))
            parameters.state.setProperty(
                "diskImageChooserDirectory",
                image.getParentDirectory().getFullPathName(), nullptr);
        wave::firmware::DosFloppyImage::SetupFile setup;
        if (wave::firmware::DosFloppyImage::readWaveSetup(image, setup).wasOk())
        {
            auto diskSet = std::make_shared<wave::presets::WaveFactorySet>();
            if (diskSet->load(setup.data).validLayout)
            {
                setActivePerformanceSet(
                    std::static_pointer_cast<const wave::presets::WaveFactorySet>(diskSet));
                pendingDiskSetBytes.store(static_cast<uint64_t>(setup.data.getSize()),
                                          std::memory_order_release);
                pendingDiskSetActivation.store(true, std::memory_order_release);
                diskSetImportConfirmationPending.store(
                    true, std::memory_order_release);
            }
        }
    }
    return result;
}

juce::Result WaveEmulationAudioProcessor::createDiskImageFromWaveSetup(
    const juce::File& setup, const juce::File& destination)
{
    auto output = destination;
    if (output.getFileExtension().isEmpty())
        output = output.withFileExtension(".img");
    const auto created = wave::firmware::DosFloppyImage::createWithWaveSetup(output, setup);
    if (created.failed())
        return created;
    return mountDiskImage(output);
}

juce::Result WaveEmulationAudioProcessor::createBlankDiskImage(
    const juce::File& destination)
{
    auto output = destination;
    if (output.getFileExtension().isEmpty())
        output = output.withFileExtension(".img");
    const auto created = wave::firmware::DosFloppyImage::createEmpty(output);
    if (created.failed())
        return created;
    return mountDiskImage(output);
}

juce::Result WaveEmulationAudioProcessor::createDiskImageFromWavetable(
    const juce::File& wavetable, const juce::File& destination)
{
    auto output = destination;
    if (output.getFileExtension().isEmpty())
        output = output.withFileExtension(".img");
    const auto created = wave::firmware::DosFloppyImage::createWithWaveWavetable(
        output, wavetable);
    if (created.failed())
        return created;
    return mountDiskImage(output);
}

juce::Result WaveEmulationAudioProcessor::flushMountedDiskImage()
{
    return masterFirmware.flushDiskImage();
}

juce::Result WaveEmulationAudioProcessor::saveMountedDiskImageAs(
    const juce::File& destination)
{
    if (!hasMountedDiskImage())
        return juce::Result::fail("There is no mounted disk image to save.");

    const auto flushed = flushMountedDiskImage();
    if (flushed.failed())
        return flushed;

    auto output = destination;
    if (output.getFileExtension().isEmpty())
        output = output.withFileExtension(".img");
    const auto source = getMountedDiskImageFile();
    if (source == output)
        return juce::Result::ok();

    juce::MemoryBlock bytes;
    if (!source.loadFileAsData(bytes) || bytes.getSize() == 0u)
        return juce::Result::fail("The mounted disk image could not be read.");
    if (!output.replaceWithData(bytes.getData(), bytes.getSize()))
        return juce::Result::fail("The disk image copy could not be saved.");

    // Save As selects the copy as the current writable medium, without
    // re-importing its SET file or changing the currently sounding program.
    const auto mounted = masterFirmware.mountDiskImage(output);
    if (mounted.wasOk())
    {
        removeRestoredHostStateDisk();
        parameters.state.setProperty("mountedDiskImage", output.getFullPathName(), nullptr);
        parameters.state.setProperty(
            "diskImageChooserDirectory",
            output.getParentDirectory().getFullPathName(), nullptr);
    }
    return mounted;
}

juce::Result WaveEmulationAudioProcessor::ejectDiskImage()
{
    const auto result = masterFirmware.ejectDiskImage();
    if (result.wasOk())
    {
        pendingDiskSetBytes.store(0, std::memory_order_release);
        pendingDiskSetActivation.store(false, std::memory_order_release);
        pendingManagerExitProgram.store(-1, std::memory_order_release);
        diskSetImportConfirmationPending.store(false,
                                               std::memory_order_release);
        returnToPerformanceAfterDiskImport.store(false,
                                                 std::memory_order_release);
        storeSaveCompletionPending.store(false, std::memory_order_release);
        storeExitModePending.store(false,
                                                std::memory_order_release);
        removeRestoredHostStateDisk();
        parameters.state.removeProperty("mountedDiskImage", nullptr);
    }
    return result;
}

juce::Result WaveEmulationAudioProcessor::reloadBankFromMountedImage()
{
    const juce::ScopedLock callbackLock(getCallbackLock());
    wave::firmware::DosFloppyImage::SetupFile setup;
    const auto read = wave::firmware::DosFloppyImage::readWaveSetup(
        masterFirmware.mountedDiskImageFile(), setup);
    if (read.failed())
        return read;
    wave::presets::WaveFactorySet set;
    if (!set.load(setup.data).validLayout)
        return juce::Result::fail("The mounted image contains no valid Wave bank.");
    juce::MemoryBlock saved;
    getStateInformation(saved);
    const auto xml = getXmlFromBinary(saved.getData(), static_cast<int>(saved.getSize()));
    auto state = juce::ValueTree::fromXml(*xml);
    state.setProperty("machineActiveSetData", juce::var(setup.data), nullptr);
    state.setProperty("machineHasActiveSet", true, nullptr);
    for (const auto* key : { "machinePerformanceSnapshot", "machineFirmwareEditBuffers" })
        state.removeProperty(key, nullptr);
    copyXmlToBinary(*state.createXml(), saved);
    setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
    return juce::Result::ok();
}

void WaveEmulationAudioProcessor::resetToColdStart()
{
    suspendProcessing(true);
    cancelPendingUpdate();
    suspendFirmwareSoundFeedback();
    pendingFirmwareProgram.store(-1, std::memory_order_release);
    pendingFirmwareInstrument.store(-1, std::memory_order_release);
    pendingInstrumentPageSelection.store(-1, std::memory_order_release);
    instrumentEditPage.store(0, std::memory_order_release);
    for (auto& value : pendingInstrumentFaderEdits)
        value.store(-1, std::memory_order_release);
    for (auto& value : pendingGlideFaderEdits)
        value.store(-1, std::memory_order_release);
    instrumentPageReady.store(false, std::memory_order_release);
    startInstrumentPageSelectionDelay.store(false, std::memory_order_release);
    pendingFirmwareSoftButton.store(-1, std::memory_order_release);
    pendingPanelCancel.store(false, std::memory_order_release);
    pendingManagerExitProgram.store(-1, std::memory_order_release);
    pendingFirmwarePageButton.store(-1, std::memory_order_release);
    pendingFirmwareModeButton.store(-1, std::memory_order_release);
    pendingFirmwareGlideSwitchClicks.store(0, std::memory_order_release);
    queuedFirmwareGlideSwitchClicks = 0;
    firmwareGlideSwitchTransactionActive = false;
    firmwareGlideSwitchReleaseSent = false;
    firmwareGlideSwitchBaseline = -1;
    firmwareGlideSwitchWaitBlocks = 0;
    firmwareGlideSwitchRetryCount = 0;
    pendingPanelStepButtonEvents.store(0, std::memory_order_release);
    cancelPanelStepButtonEvents.store(true, std::memory_order_release);
    panelModeDisplayTransitionActive.store(false, std::memory_order_release);
    panelModeDisplayAwaitingDispatch.store(false, std::memory_order_release);
    pendingEngineProgram.store(-1, std::memory_order_release);
    pendingPanelPerformance.store(-1, std::memory_order_release);
    pendingDiskSetBytes.store(0, std::memory_order_release);
    pendingDiskSetActivation.store(false, std::memory_order_release);
    pendingManagerExitProgram.store(-1, std::memory_order_release);
    diskSetImportConfirmationPending.store(false,
                                           std::memory_order_release);
    returnToPerformanceAfterDiskImport.store(false,
                                             std::memory_order_release);
    storeSaveCompletionPending.store(false, std::memory_order_release);
    storeExitModePending.store(false,
                                            std::memory_order_release);
    keyboardOctaveShift.store(0, std::memory_order_release);
    pendingKeyboardOctaveRestore.store(2, std::memory_order_release);
    keyboardOctaveRestoreWaitBlocks.store(0, std::memory_order_release);
    localKeyboardTransposedNotes.fill(-1);
    storeMenuActive.store(false, std::memory_order_release);
    diskMenuActive.store(false, std::memory_order_release);
    filterCalibrationServiceActive.store(false, std::memory_order_release);
    filterCalibrationServiceExitPending.store(false,
                                              std::memory_order_release);
    pendingFilterCalibrationServiceSeed.store(false,
                                              std::memory_order_release);
    (void) ejectDiskImage();
    setActivePerformanceSet(nullptr);
    currentProgram.store(0, std::memory_order_release);
    performanceFadersTouched.store(0, std::memory_order_release);
    instrumentButtonMode.store(static_cast<int>(InstrumentButtonMode::normal),
                               std::memory_order_release);
    parameters.state.setProperty("factoryProgram", 0, nullptr);

    auto emptyPerformance
        = std::make_shared<wave::dsp::WaldorfEngine::PerformanceSnapshot>();
    emptyPerformance->editableLayer = -1;
    std::atomic_store_explicit(
        &factoryPerformance,
        std::static_pointer_cast<const wave::dsp::WaldorfEngine::PerformanceSnapshot>(
            emptyPerformance),
        std::memory_order_release);
    auto emptySoundSeed = std::make_shared<PerformanceInstrumentSoundSeed>();
    std::atomic_store_explicit(
        &instrumentSoundSeed,
        std::static_pointer_cast<const PerformanceInstrumentSoundSeed>(emptySoundSeed),
        std::memory_order_release);
    engine.reset();

    // A standalone power cycle is not a factory-set recall. Reboot OS 1.700
    // using the genuine safe INIT.SND/INIT.PFM records embedded in the OS
    // image, rather than the factory A001 records used during construction.
    masterFirmware.useFirmwareInitialisationRecords();
    startLoadedFirmware(firmware.getReport(), false);
    suspendProcessing(false);
}

int WaveEmulationAudioProcessor::getNumPrograms()
{
    const auto set = currentPerformanceSet();
    return set != nullptr && set->isLoaded()
               ? wave::presets::WaveFactorySet::programCount
               : 1;
}

const juce::String WaveEmulationAudioProcessor::getProgramName(int index)
{
    const auto set = currentPerformanceSet();
    if (set == nullptr || !set->isLoaded())
        return "Initial Wave";
    const auto bounded = juce::jlimit(0, wave::presets::WaveFactorySet::programCount - 1,
                                      index);
    return set->performanceName(bounded / 128, bounded % 128);
}

void WaveEmulationAudioProcessor::setCurrentProgram(int index)
{
    const auto set = currentPerformanceSet();
    if (set == nullptr || !set->isLoaded())
    {
        currentProgram = 0;
        return;
    }
    const auto bounded = juce::jlimit(0, wave::presets::WaveFactorySet::programCount - 1,
                                      index);
    const auto managerExitPending
        = pendingManagerExitProgram.load(std::memory_order_acquire) >= 0;
    const auto leaveCompletedStore = managerExitPending
        || (storeMenuActive.load(std::memory_order_acquire)
            && completedStoreMode.load(std::memory_order_acquire) >= 0);
    armPanelPotProgramRecallGuard();
    instrumentButtonMode.store(static_cast<int>(InstrumentButtonMode::normal),
                               std::memory_order_release);
    pendingFirmwareInstrument.store(-1, std::memory_order_release);
    pendingInstrumentPageSelection.store(-1, std::memory_order_release);
    instrumentPageReady.store(false, std::memory_order_release);
    startInstrumentPageSelectionDelay.store(false, std::memory_order_release);
    if (!managerExitPending)
    {
        pendingFirmwareSoftButton.store(-1, std::memory_order_release);
        pendingPanelCancel.store(false, std::memory_order_release);
        pendingManagerExitProgram.store(-1, std::memory_order_release);
    }
    pendingFirmwarePageButton.store(-1, std::memory_order_release);
    pendingFirmwareModeButton.store(-1, std::memory_order_release);
    panelModeDisplayTransitionActive.store(false, std::memory_order_release);
    panelModeDisplayAwaitingDispatch.store(false, std::memory_order_release);
    pendingPanelPerformance.store(-1, std::memory_order_release);
    // A numeric Performance selection asks OS 1.700 to leave the current
    // operating page and install the Performance callbacks. Keep the serial
    // panel context in lockstep with that firmware request so the following
    // -/+ contacts are not misrouted to the page we just left.
    diskMenuActive.store(false, std::memory_order_release);
    storeMenuActive.store(false, std::memory_order_release);
    storeSaveCompletionPending.store(false, std::memory_order_release);
    storeExitModePending.store(false,
                                            std::memory_order_release);
    panelSelectedMode.store(39, std::memory_order_release);
    panelSelectedEdit.store(-1, std::memory_order_release);
    cancelPanelStepButtonEvents.store(true, std::memory_order_release);
    currentProgram = bounded;
    parameters.state.setProperty("factoryProgram", bounded, nullptr);
    applyFactoryProgram(bounded, !leaveCompletedStore);
    if (leaveCompletedStore)
    {
        // A completed Store still owns the native chooser. Exit it before
        // recalling the requested program, retaining one Cancel transaction
        // while rapid browsing clicks update its destination.
        pendingManagerExitProgram.store(bounded, std::memory_order_release);
        if (!managerExitPending)
            pendingPanelCancel.store(true, std::memory_order_release);
    }
    updateHostDisplay(ChangeDetails{}.withProgramChanged(true)
                          .withNonParameterStateChanged(true));
}

bool WaveEmulationAudioProcessor::selectFactoryPerformance(int bank,
                                                            int performanceNumber)
{
    const auto set = currentPerformanceSet();
    if (set == nullptr || !set->isLoaded() || bank < 0 || bank > 1
        || performanceNumber < 1 || performanceNumber > 128)
        return false;

    setCurrentProgram(bank * 128 + performanceNumber - 1);
    return true;
}

void WaveEmulationAudioProcessor::selectFactoryBank(int bank)
{
    const auto set = currentPerformanceSet();
    if (set == nullptr || !set->isLoaded() || bank < 0 || bank > 1)
        return;

    setCurrentProgram(bank * 128
                      + currentProgram.load(std::memory_order_acquire) % 128);
}

void WaveEmulationAudioProcessor::stepFactoryPerformance(int delta)
{
    const auto programCount = getNumPrograms();
    if (programCount <= 1 || delta == 0)
        return;

    auto destination = (currentProgram.load(std::memory_order_acquire) + delta)
                       % programCount;
    if (destination < 0)
        destination += programCount;
    setCurrentProgram(destination);
}

bool WaveEmulationAudioProcessor::selectPerformanceInstrument(int instrument)
{
    if (instrument < 0 || instrument >= 8)
        return false;
    const auto current = std::atomic_load_explicit(
        &factoryPerformance, std::memory_order_acquire);
    if (current == nullptr
        || (!current->layers[static_cast<size_t>(instrument)].enabled
            && (panelSelectedMode.load(std::memory_order_acquire) != 36
                || masterFirmware.currentInstrumentEditTarget() != instrument)))
        return false;
    static constexpr std::array instrumentCodes { 22, 25, 26, 27,
                                                   79, 28, 29, 30 };
    const auto requestProgrammaticFirmwareSelection = [this, instrument] {
        const auto physicalSelectionInFlight = std::any_of(
            instrumentCodes.begin(), instrumentCodes.end(), [this](int code) {
                const auto matrix = wave::panel::matrixIndexForDiagnosticCode(code);
                return matrix >= 0
                       && panelButtonDown[static_cast<size_t>(matrix)].load(
                           std::memory_order_acquire);
            });
        if (physicalSelectionInFlight || !masterFirmware.isLoaded())
            return;
        cancelActiveInstrumentSelection.store(true, std::memory_order_release);
        const auto firmwareTarget = panelSelectedMode.load(std::memory_order_acquire) == 36
            ? masterFirmware.currentInstrumentEditTarget()
            : masterFirmware.currentPerformanceInstrument();
        pendingFirmwareInstrument.store(
            firmwareTarget == instrument
                ? -1 : instrument,
            std::memory_order_release);
    };
    if (current->editableLayer == instrument)
    {
        requestProgrammaticFirmwareSelection();
        return true;
    }

    auto staged = std::make_shared<wave::dsp::WaldorfEngine::PerformanceSnapshot>(*current);
    const auto previous = staged->editableLayer;
    if (previous >= 0 && previous < 8)
    {
        const auto native
            = staged->layers[static_cast<size_t>(previous)].sound;
        const auto panelMirror = wave::parameters::readSnapshot(parameters);
        auto edited = panelMirror;
        const auto confirmed = std::atomic_load_explicit(
            &latestFirmwareSelectedSound, std::memory_order_acquire);
        if (confirmed != nullptr
            && confirmed->performance
                   == currentProgram.load(std::memory_order_acquire)
            && confirmed->instrument == previous)
        {
            // Once this Instrument ceases to be editable it will no longer be
            // replaced from the live firmware record in processBlock(). Save
            // the last confirmed record now, never the potentially stale
            // shared panel mirror. Only host-side extensions absent from a
            // native Sound record are retained from APVTS.
            edited = confirmed->sound;
            edited.quickEditAmounts = panelMirror.quickEditAmounts;
            edited.driveDb = panelMirror.driveDb;
        }
        // Pan and Pan Mode are Instrument fields in the Performance record,
        // not bytes in the selected 256-byte Sound record. decodeFactorySound
        // therefore quite correctly supplies neutral defaults for them. Never
        // let an edit-target change copy those defaults over the outgoing
        // Instrument: doing so collapses stereo Performances such as A045 and
        // A051 towards the centre and changes their apparent level merely by
        // pressing a softbutton. The output/age values are Performance-wide
        // host extensions and likewise do not belong to this per-Instrument
        // save boundary.
        edited.panAmount = native.panAmount;
        edited.panModulationMode = native.panModulationMode;
        edited.outputDb = native.outputDb;
        edited.circuitAgeAmount = native.circuitAgeAmount;
        for (auto& amount : edited.quickEditAmounts)
            if (std::abs(amount) < 1.0e-6f)
                amount = 0.0f;
        for (size_t oscillator = 0; oscillator < panelOscillatorOctaves.size();
             ++oscillator)
            edited.oscillatorOctaves[oscillator]
                = panelOscillatorOctaves[oscillator].load(
                    std::memory_order_acquire);
        preserveUnexposedControlFields(edited, native);
        staged->layers[static_cast<size_t>(previous)].sound = edited;
    }

    suspendFirmwareSoundFeedback();

    // Delimit the parameter update without publishing a Performance that has
    // no editable Instrument. The audio thread uses this version as a seqlock
    // and skips its panel-parameter merge if it overlaps the transaction.
    performanceInstrumentSelectionVersion.fetch_add(1, std::memory_order_acq_rel);
    wave::parameters::writeSnapshot(
        parameters, staged->layers[static_cast<size_t>(instrument)].sound, false);
    // The newly selected Instrument's stored Sound is authoritative. Without
    // this boundary, the next audio block reapplies the previous Instrument's
    // still-visible firmware modifier selectors to the new voice.
    modulationRoutingResetPending.store(true, std::memory_order_release);
    for (size_t oscillator = 0; oscillator < panelOscillatorOctaves.size(); ++oscillator)
        panelOscillatorOctaves[oscillator].store(
            staged->layers[static_cast<size_t>(instrument)]
                .sound.oscillatorOctaves[oscillator],
            std::memory_order_release);

    auto selected = std::make_shared<wave::dsp::WaldorfEngine::PerformanceSnapshot>(*staged);
    selected->editableLayer = instrument;
    std::atomic_store_explicit(
        &factoryPerformance,
        std::static_pointer_cast<const wave::dsp::WaldorfEngine::PerformanceSnapshot>(selected),
        std::memory_order_release);
    performanceInstrumentSelectionVersion.fetch_add(1, std::memory_order_release);

    // Calls originating from a physical Instrument switch already have that
    // matrix action in flight. Programmatic selection has no such event, so
    // complete the same serial transaction instead of leaving the engine and
    // OS 1.700 pointed at different Sound records.
    requestProgrammaticFirmwareSelection();
    return true;
}

bool WaveEmulationAudioProcessor::pressPerformanceInstrumentButton(int instrument)
{
    if (instrument < 0 || instrument >= 8)
        return false;
    const auto current = std::atomic_load_explicit(
        &factoryPerformance, std::memory_order_acquire);
    if (current == nullptr || !current->layers[static_cast<size_t>(instrument)].enabled)
        return false;

    const auto mode = getInstrumentButtonMode();
    if (mode == InstrumentButtonMode::normal)
        return selectPerformanceInstrument(instrument);

    auto changed = std::make_shared<wave::dsp::WaldorfEngine::PerformanceSnapshot>(*current);
    if (mode == InstrumentButtonMode::mute)
    {
        auto& target = changed->layers[static_cast<size_t>(instrument)];
        target.muted = !target.muted;
    }
    else
    {
        for (auto& layer : changed->layers)
            layer.soloed = false;
        changed->layers[static_cast<size_t>(instrument)].soloed = true;
    }
    std::atomic_store_explicit(
        &factoryPerformance,
        std::static_pointer_cast<const wave::dsp::WaldorfEngine::PerformanceSnapshot>(changed),
        std::memory_order_release);
    return true;
}

void WaveEmulationAudioProcessor::togglePerformanceMuteMode()
{
    const auto current = std::atomic_load_explicit(
        &factoryPerformance, std::memory_order_acquire);
    if (current == nullptr)
        return;
    const auto wasMute = getInstrumentButtonMode() == InstrumentButtonMode::mute;
    auto changed = std::make_shared<wave::dsp::WaldorfEngine::PerformanceSnapshot>(*current);
    for (auto& layer : changed->layers)
    {
        layer.soloed = false;
        if (wasMute)
            layer.muted = false;
    }
    instrumentButtonMode.store(
        static_cast<int>(wasMute ? InstrumentButtonMode::normal
                                 : InstrumentButtonMode::mute),
        std::memory_order_release);
    std::atomic_store_explicit(
        &factoryPerformance,
        std::static_pointer_cast<const wave::dsp::WaldorfEngine::PerformanceSnapshot>(changed),
        std::memory_order_release);
}

void WaveEmulationAudioProcessor::togglePerformanceSoloMode()
{
    const auto current = std::atomic_load_explicit(
        &factoryPerformance, std::memory_order_acquire);
    if (current == nullptr)
        return;
    const auto wasSolo = getInstrumentButtonMode() == InstrumentButtonMode::solo;
    auto changed = std::make_shared<wave::dsp::WaldorfEngine::PerformanceSnapshot>(*current);
    for (auto& layer : changed->layers)
    {
        layer.muted = false;
        layer.soloed = false;
    }
    instrumentButtonMode.store(
        static_cast<int>(wasSolo ? InstrumentButtonMode::normal
                                 : InstrumentButtonMode::solo),
        std::memory_order_release);
    std::atomic_store_explicit(
        &factoryPerformance,
        std::static_pointer_cast<const wave::dsp::WaldorfEngine::PerformanceSnapshot>(changed),
        std::memory_order_release);
}

int WaveEmulationAudioProcessor::getSelectedPerformanceInstrument() const noexcept
{
    const auto performance = std::atomic_load_explicit(
        &factoryPerformance, std::memory_order_acquire);
    return performance != nullptr ? performance->editableLayer : -1;
}

bool WaveEmulationAudioProcessor::isPerformanceInstrumentActive(int instrument) const noexcept
{
    if (instrument < 0 || instrument >= 8)
        return false;
    const auto performance = std::atomic_load_explicit(
        &factoryPerformance, std::memory_order_acquire);
    return performance != nullptr
           && performance->layers[static_cast<size_t>(instrument)].enabled;
}

bool WaveEmulationAudioProcessor::isPerformanceInstrumentMuted(int instrument) const noexcept
{
    if (instrument < 0 || instrument >= 8)
        return false;
    const auto performance = std::atomic_load_explicit(
        &factoryPerformance, std::memory_order_acquire);
    return performance != nullptr
           && performance->layers[static_cast<size_t>(instrument)].muted;
}

bool WaveEmulationAudioProcessor::isPerformanceInstrumentAudible(int instrument) const noexcept
{
    if (instrument < 0 || instrument >= 8)
        return false;
    const auto performance = std::atomic_load_explicit(
        &factoryPerformance, std::memory_order_acquire);
    if (performance == nullptr
        || !performance->layers[static_cast<size_t>(instrument)].enabled)
        return false;
    const auto hasSolo = std::any_of(
        performance->layers.begin(), performance->layers.end(), [](const auto& layer) {
            return layer.enabled && layer.soloed;
        });
    const auto& layer = performance->layers[static_cast<size_t>(instrument)];
    return !layer.muted && (!hasSolo || layer.soloed);
}

void WaveEmulationAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    currentSampleRate = juce::jmax(1.0, sampleRate);
    uiMidiCollector.reset(currentSampleRate);
    engine.prepare(sampleRate, samplesPerBlock);
    seedFilterCalibrationTable();
    outputPeak.store(0.0f);
    localKeyboardTransposedNotes.fill(-1);
}

void WaveEmulationAudioProcessor::audioWorkgroupContextChanged(const juce::AudioWorkgroup& workgroup)
{
    engine.setAudioWorkgroup(workgroup);
}

void WaveEmulationAudioProcessor::releaseResources()
{
    engine.reset();
    for (auto& channels : activeMidiNoteChannels)
        channels.store(0, std::memory_order_release);
    localKeyboardTransposedNotes.fill(-1);
}

bool WaveEmulationAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto output = layouts.getMainOutputChannelSet();
    return output == juce::AudioChannelSet::mono() || output == juce::AudioChannelSet::stereo();
}

void WaveEmulationAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer,
                                                juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    juce::MidiBuffer localKeyboardMidi;
    uiMidiCollector.removeNextBlockOfMessages(localKeyboardMidi, buffer.getNumSamples());
    juce::MidiBuffer routedMidi;
    // A plug-in wrapper has only one note input: it is the playable keybed
    // substitute, not a separately exposed copy of the Wave's rear-panel MIDI
    // socket. Route its notes through the local-keyboard path so the genuine
    // lower-panel Octave state, Keyboard+MIDI layer fan-out, and held-note
    // bookkeeping behave exactly as they do in the standalone instrument.
    const auto pluginWrapperOwnsPlayableMidi
        = wrapperType != juce::AudioProcessor::wrapperType_Undefined
          && wrapperType != juce::AudioProcessor::wrapperType_Standalone;
    const auto useMidiAsLocalKeyboard
        = pluginWrapperOwnsPlayableMidi
          || midiInputActsAsLocalKeyboard.load(std::memory_order_acquire);
    const auto* performanceMidi = &midi;
    if (useMidiAsLocalKeyboard)
    {
        for (const auto metadata : midi)
        {
            const auto& message = metadata.getMessage();
            if (message.isNoteOnOrOff())
                localKeyboardMidi.addEvent(message, metadata.samplePosition);
            else
                routedMidi.addEvent(message, metadata.samplePosition);
        }
        performanceMidi = &routedMidi;
    }

    const auto trackActiveMidiNotes = [this](const juce::MidiBuffer& messages) {
        for (const auto metadata : messages)
        {
            const auto& message = metadata.getMessage();
            const auto channel = juce::jlimit(1, 16, message.getChannel());
            const auto channelBit = static_cast<uint16_t>(1u << (channel - 1));
            if (message.isNoteOn())
            {
                const auto note = static_cast<size_t>(message.getNoteNumber());
                activeMidiNoteChannels[note].fetch_or(channelBit,
                                                      std::memory_order_release);
            }
            else if (message.isNoteOff())
            {
                const auto note = static_cast<size_t>(message.getNoteNumber());
                activeMidiNoteChannels[note].fetch_and(
                    static_cast<uint16_t>(~channelBit), std::memory_order_release);
            }
            else if (message.isAllNotesOff() || message.isAllSoundOff())
            {
                for (auto& channels : activeMidiNoteChannels)
                    channels.fetch_and(static_cast<uint16_t>(~channelBit),
                                       std::memory_order_release);
            }
            if (message.isPitchWheel())
                keyboardPitchWheel.store(message.getPitchWheelValue(),
                                         std::memory_order_release);
            else if (message.isController()
                     && message.getControllerNumber() == 1)
            {
                keyboardModWheel.store(message.getControllerValue(),
                                       std::memory_order_release);
                const auto sequence = controllerInputSequence.fetch_add(
                                          1, std::memory_order_acq_rel)
                                      + 1;
                modWheelInputSequence.store(sequence, std::memory_order_release);
            }
            else if (message.isController()
                     && message.getControllerNumber() == 16)
                keyboardFreeWheel.store(message.getControllerValue(),
                                        std::memory_order_release);
        }
    };
    trackActiveMidiNotes(midi);
    trackActiveMidiNotes(localKeyboardMidi);

    runFirmwareTimeline(*performanceMidi, buffer.getNumSamples());
    // The keyboard controller owns its transpose state. OS 1.700 exposes that
    // state on the two genuine lower-panel LED outputs, so use those outputs
    // as feedback instead of incrementing a UI-side octave counter.
    const auto octaveUp = masterFirmware.panelLed(58);
    const auto octaveDown = masterFirmware.panelLed(27);
    const auto octaveShift = octaveUp == octaveDown ? 0 : octaveUp ? 1 : -1;
    keyboardOctaveShift.store(octaveShift, std::memory_order_release);
    const auto restoreOctave
        = pendingKeyboardOctaveRestore.load(std::memory_order_acquire);
    if (restoreOctave >= -1 && restoreOctave <= 1)
    {
        if (restoreOctave == octaveShift)
        {
            pendingKeyboardOctaveRestore.store(2, std::memory_order_release);
            keyboardOctaveRestoreWaitBlocks.store(0, std::memory_order_release);
        }
        else if (keyboardOctaveRestoreWaitBlocks.load(
                     std::memory_order_acquire) > 0)
        {
            keyboardOctaveRestoreWaitBlocks.fetch_sub(
                1, std::memory_order_acq_rel);
        }
        else
        {
            // Restore the saved hardware state by pressing the same genuine
            // lower-panel serial contact a user would press. Firmware remains
            // the owner of both transposition and the two LED outputs.
            const auto diagnosticCode = restoreOctave > octaveShift ? 12 : 73;
            masterFirmware.setPanelButton(diagnosticCode, true);
            masterFirmware.setPanelButton(diagnosticCode, false);
            keyboardOctaveRestoreWaitBlocks.store(16,
                                                  std::memory_order_release);
        }
    }

    // Transpose only events produced by the Wave's physical/local keyboard.
    // In a plug-in the host's sole note stream substitutes for that keyboard;
    // direct engine/test clients can still select genuine external-MIDI
    // routing explicitly.
    // Remember each note-on's routed pitch so changing octave while holding a
    // key cannot strand the note when its original key number is released.
    juce::MidiBuffer transposedLocalKeyboardMidi;
    for (const auto metadata : localKeyboardMidi)
    {
        auto message = metadata.getMessage();
        if (message.isNoteOn())
        {
            const auto source = juce::jlimit(0, 127, message.getNoteNumber());
            const auto destination
                = juce::jlimit(0, 127, source + octaveShift * 12);
            localKeyboardTransposedNotes[static_cast<size_t>(source)] = destination;
            message.setNoteNumber(destination);
        }
        else if (message.isNoteOff())
        {
            const auto source = juce::jlimit(0, 127, message.getNoteNumber());
            auto& destination
                = localKeyboardTransposedNotes[static_cast<size_t>(source)];
            message.setNoteNumber(destination >= 0
                                      ? destination
                                      : juce::jlimit(
                                            0, 127, source + octaveShift * 12));
            destination = -1;
        }
        else if (message.isAllNotesOff() || message.isAllSoundOff())
        {
            localKeyboardTransposedNotes.fill(-1);
        }
        transposedLocalKeyboardMidi.addEvent(message, metadata.samplePosition);
    }
    localKeyboardMidi.swapWith(transposedLocalKeyboardMidi);
    // OS 1.700 owns touch/toggle mode and the lamps reflect its actual state.
    // Do not disguise these dedicated sources as CC80/81: MIDI assignments
    // are Performance bytes 4/5 and may coincide with each other or a wheel.
    engine.setKeyboardButtons(masterFirmware.panelLed(16), masterFirmware.panelLed(35));
    if (const auto performance = masterFirmware.currentPerformanceRecordOffset())
        engine.setKeyboardButtonControllers(
            masterFirmware.sharedProgramByte(*performance + 4u) & 0x7fu,
            masterFirmware.sharedProgramByte(*performance + 5u) & 0x7fu);
    const auto selectionVersionBefore
        = performanceInstrumentSelectionVersion.load(std::memory_order_acquire);
    auto editedSound = wave::parameters::readSnapshot(parameters);
    const auto storedPerformance = std::atomic_load_explicit(
        &factoryPerformance, std::memory_order_acquire);
    const auto selectionVersionAfter
        = performanceInstrumentSelectionVersion.load(std::memory_order_acquire);
    const auto selectionWasStable
        = selectionVersionBefore == selectionVersionAfter
          && (selectionVersionAfter & 1u) == 0u;
    const auto firmwarePerformance = masterFirmware.currentPerformanceId();
    const auto firmwareInstrument = masterFirmware.currentPerformanceInstrument();
    const auto routingContextMatches
        = selectionWasStable && storedPerformance != nullptr
          && firmwarePerformance.has_value()
          && firmwareInstrument.has_value()
          && *firmwarePerformance == currentProgram.load(std::memory_order_acquire)
          && *firmwareInstrument == storedPerformance->editableLayer;
    std::optional<wave::parameters::Snapshot> firmwareSelectedSound;
    if (routingContextMatches)
    {
        auto parameterSyncPending = false;

        // Bounded panel knobs reach the emulation through the physical ADC
        // path and are mirrored back later by firmware. A direct APVTS change,
        // however, is host automation and has no ADC edge. Commit only those
        // changes to the selected firmware record. Resetting this comparison
        // on every Sound selection is what prevents the incoming voice from
        // being overwritten by the previous panel snapshot.
        const auto resetHostTracking = resetHostPanelPotTracking.exchange(
            false, std::memory_order_acq_rel);
        const auto recallEchoGuardActive
            = programRecallPanelPotEchoGuardBlocks.load(
                  std::memory_order_acquire) > 0;
        if (resetHostTracking)
        {
            lastFirmwareOscillatorLink = -1;
            lastFirmwareOscillatorLinkLed = -1;
        }
        for (size_t index = 0; index < wave::panel::visiblePots.size(); ++index)
        {
            const auto* raw = parameters.getRawParameterValue(
                wave::panel::visiblePots[index].parameterId);
            if (raw == nullptr)
                continue;
            const auto hostValue = raw->load(std::memory_order_acquire);
            const auto staleProgramValue
                = programRecallPanelPotEchoValues[index].load(
                    std::memory_order_acquire);
            const auto isProgramRecallEcho
                = recallEchoGuardActive && !std::isnan(staleProgramValue)
                  && std::abs(hostValue - staleProgramValue) <= 1.0e-5f;
            if (isProgramRecallEcho)
            {
                // Pro Tools may replay the preceding program's cached AAX
                // parameter values immediately after an internal program
                // selection. The Wave's newly selected Sound is authoritative
                // during that acknowledgement window. Publish its value
                // again, but never turn the stale echo into a firmware edit.
                pendingFirmwarePanelPotValues[index].store(
                    decodePanelPotValue(
                        panelPotRecordBindings[index],
                        masterFirmware.currentSoundRecordByte(
                            panelPotRecordBindings[index].soundOffset)),
                    std::memory_order_release);
                parameterSyncPending = true;
            }
            else if (!resetHostTracking
                && !std::isnan(lastHostPanelPotValues[index])
                && std::abs(hostValue - lastHostPanelPotValues[index]) > 1.0e-5f)
                masterFirmware.writeCurrentSoundRecordByte(
                    panelPotRecordBindings[index].soundOffset,
                    encodePanelPotValue(panelPotRecordBindings[index], hostValue));
            lastHostPanelPotValues[index] = hostValue;
        }
        if (recallEchoGuardActive)
            programRecallPanelPotEchoGuardBlocks.fetch_sub(
                1, std::memory_order_acq_rel);

        if (applyFirmwareModifierRouting(
                editedSound, masterFirmware, lastFirmwareModulationSources,
                lastHostModulationSources, effectiveModulationSources,
                lastFirmwareModulationControls, lastHostModulationControls,
                effectiveModulationControls, lastFirmwareModulationAmounts,
                lastHostModulationAmounts, effectiveModulationAmounts,
                pendingFirmwareModulationSources,
                pendingFirmwareModulationControls,
                pendingFirmwareModulationAmounts,
                modulationRoutingResetPending.exchange(
                    false, std::memory_order_acq_rel)))
            parameterSyncPending = true;

        constexpr std::array<uint32_t, 6> glideRecordOffsets {
            233u, 234u, 235u, 236u, 237u, 238u
        };
        std::array<int, 6> glideValues {};
        for (size_t index = 0; index < glideRecordOffsets.size(); ++index)
        {
            const auto stored = static_cast<int>(
                masterFirmware.currentSoundRecordByte(glideRecordOffsets[index])
                & 0x7fu);
            glideValues[index] = index == 4 ? stored - 64 : stored;
            if (lastFirmwareGlideRecordBytes[index] == stored)
                continue;
            lastFirmwareGlideRecordBytes[index] = stored;
            pendingFirmwareGlideValues[index].store(
                glideValues[index], std::memory_order_release);
            parameterSyncPending = true;
        }
        editedSound.glideTypeMode = juce::jlimit(1, 6, glideValues[0]);
        editedSound.glideRateValue
            = static_cast<float>(juce::jlimit(0, 127, glideValues[1]));
        editedSound.glideTimeModeValue = juce::jlimit(0, 1, glideValues[2]);
        editedSound.glideRateModulationSource
            = juce::jlimit(0, 39, glideValues[3]);
        editedSound.glideRateModulationAmount
            = static_cast<float>(juce::jlimit(-64, 63, glideValues[4]));
        editedSound.glideEnabled = glideValues[5] != 0;

        auto firmwareOscillatorLink = static_cast<int>(
            masterFirmware.currentSoundRecordByte(23u) & 0x01u);
        const auto firmwareOscillatorLinkLed
            = masterFirmware.panelLed(63) ? 1 : 0;
        if (lastFirmwareOscillatorLinkLed < 0)
            lastFirmwareOscillatorLinkLed = firmwareOscillatorLinkLed;
        else if (lastFirmwareOscillatorLinkLed != firmwareOscillatorLinkLed)
        {
            // OS 1.700 drives the latched Wave-Link lamp through the panel
            // serial output. The original panel/ASIC then transfers that
            // state into Sound byte 23. Model only that absent hardware
            // transfer, after the genuine firmware edge has occurred.
            lastFirmwareOscillatorLinkLed = firmwareOscillatorLinkLed;
            firmwareOscillatorLink = firmwareOscillatorLinkLed;
            masterFirmware.writeCurrentSoundRecordByte(
                23u, static_cast<uint8_t>(firmwareOscillatorLink));
        }
        if (lastFirmwareOscillatorLink != firmwareOscillatorLink)
        {
            lastFirmwareOscillatorLink = firmwareOscillatorLink;
            pendingFirmwareOscillatorLink.store(
                firmwareOscillatorLink, std::memory_order_release);
            parameterSyncPending = true;
        }
        editedSound.oscillatorLinkEnabled = firmwareOscillatorLink != 0;

        // The live firmware Sound record, rather than the shared host/panel
        // snapshot, owns the selected Instrument. A physical potentiometer is
        // intentionally left at its old position when another Instrument is
        // selected; using APVTS here made that old panel state silently replace
        // the recalled Sound until firmware feedback reached the message
        // thread. Decode the complete record atomically on the audio thread so
        // the DSP changes source at the same boundary as OS 1.700.
        std::array<uint8_t, wave::presets::WaveFactorySet::soundSize> liveRecord {};
        for (size_t offset = 0; offset < liveRecord.size(); ++offset)
            liveRecord[offset] = masterFirmware.currentSoundRecordByte(
                static_cast<uint32_t>(offset));
        // During the initial SET import, OS 1.700 copies a nonzero Wavetable
        // selector into its live edit buffer one count lower than the original
        // Instrument-local record. Once that record is installed directly in
        // the cache (or edited from the panel), it uses the normal selector.
        // Reconcile that one import edge in the native record itself, exactly
        // once per Instrument. Keeping an adjusted DSP-only copy makes Store
        // save the unadjusted byte and recall an adjacent table. The native
        // record, private edits, DSP and Store must share the same selector.
        const auto selectedIndex = static_cast<size_t>(*firmwareInstrument);
        if (!activeInstrumentSoundRecordReconciled[selectedIndex]
            && activeInstrumentSoundPerformance == *firmwarePerformance
            && activeInstrumentSoundRecordValid[selectedIndex])
        {
            const auto seededSelector
                = activeInstrumentSoundRecords[selectedIndex][25];
            if (liveRecord[25] < 0x7fu
                && static_cast<uint8_t>(liveRecord[25] + 1u)
                       == seededSelector)
            {
                liveRecord[25] = seededSelector;
                masterFirmware.writeCurrentSoundRecordByte(25u, seededSelector);
                activeInstrumentSoundRecordReconciled[selectedIndex] = true;
            }
            else if (liveRecord[25] != seededSelector)
            {
                // A value other than the seed or its import encoding is a
                // genuine edit, so this record is already in the live/cache
                // representation from now on.
                activeInstrumentSoundRecordReconciled[selectedIndex] = true;
            }
            // Equality can be observed briefly before the OS completes its
            // import conversion. Keep waiting instead of prematurely marking
            // this Instrument reconciled.
        }
        publishHostMachineSnapshot(*firmwarePerformance, *firmwareInstrument,
                                   liveRecord);
        firmwareSelectedSound = decodeFactorySound(liveRecord);
        // Store updates the firmware's Sound bank, while the originally loaded
        // SET can still contain an older octave. Recall must follow the actual
        // selected Sound bytes, just like the other native synthesis fields.
        for (size_t oscillator = 0; oscillator < panelOscillatorOctaves.size(); ++oscillator)
            panelOscillatorOctaves[oscillator].store(
                firmwareSelectedSound->oscillatorOctaves[oscillator],
                std::memory_order_release);
        // Modifier selectors also support host automation. The reconciliation
        // above has already resolved firmware edits versus a genuine host
        // change for this block, so retain only that resolved route set rather
        // than reverting it to the pre-reconciliation record bytes.
        firmwareSelectedSound->modulationRoutes = editedSound.modulationRoutes;

        auto recordHash = 1469598103934665603ull;
        for (const auto byte : liveRecord)
        {
            recordHash ^= byte;
            recordHash *= 1099511628211ull;
        }
        if (recordHash != lastPublishedFirmwareSoundHash
            || *firmwarePerformance != lastPublishedFirmwareSoundPerformance
            || *firmwareInstrument != lastPublishedFirmwareSoundInstrument)
        {
            auto published = std::make_shared<FirmwareSelectedSoundState>();
            published->performance = *firmwarePerformance;
            published->instrument = *firmwareInstrument;
            published->sound = *firmwareSelectedSound;
            std::atomic_store_explicit(
                &latestFirmwareSelectedSound,
                std::static_pointer_cast<const FirmwareSelectedSoundState>(published),
                std::memory_order_release);
            lastPublishedFirmwareSoundHash = recordHash;
            lastPublishedFirmwareSoundPerformance = *firmwarePerformance;
            lastPublishedFirmwareSoundInstrument = *firmwareInstrument;
        }
        if (parameterSyncPending)
            triggerAsyncUpdate();
    }
    else if (masterFirmware.isLoaded())
    {
        // Program and Instrument selection are asynchronous firmware actions.
        // Until OS 1.700 has selected the same live Sound as the engine, never
        // let the outgoing record overwrite the incoming voice (or vice versa).
        modulationRoutingResetPending.store(true, std::memory_order_release);
    }
    if (const auto confirmedRate = confirmedPanelGlideRate.load(
            std::memory_order_acquire);
        confirmedRate >= 0)
        editedSound.glideRateValue = static_cast<float>(confirmedRate);
    if (const auto confirmedEnabled = confirmedPanelGlideEnabled.load(
            std::memory_order_acquire);
        confirmedEnabled >= 0)
        editedSound.glideEnabled = confirmedEnabled != 0;
    editedSound.oscillatorOctaves[0] = getFirmwareOscillatorOctave(0);
    editedSound.oscillatorOctaves[1] = getFirmwareOscillatorOctave(1);
    if (storedPerformance != nullptr)
    {
        auto performance = *storedPerformance;
        const auto editable = performance.editableLayer;
        if (routingContextMatches && firmwareSelectedSound.has_value()
            && editable >= 0 && editable < 8)
        {
            const auto& nativeSound = performance.layers[static_cast<size_t>(editable)].sound;
            auto renderedSound = *firmwareSelectedSound;

            // These are not fields in the native 256-byte Sound record. Layer
            // pan belongs to the Performance, while drive and Quick Edit are
            // host-side extensions applied after the genuine Sound decode.
            renderedSound.panAmount = nativeSound.panAmount;
            renderedSound.panModulationMode = nativeSound.panModulationMode;
            renderedSound.driveDb = editedSound.driveDb;
            renderedSound.quickEditAmounts = editedSound.quickEditAmounts;
            wave::parameters::applyQuickEdit(renderedSound);
            performance.layers[static_cast<size_t>(editable)].sound = renderedSound;
        }
        else if (!masterFirmware.isLoaded() && selectionWasStable
                 && editable >= 0 && editable < 8)
        {
            // Retain the parameter-only fallback for builds intentionally run
            // without the master firmware.
            const auto& nativeSound
                = performance.layers[static_cast<size_t>(editable)].sound;
            preserveUnexposedControlFields(editedSound, nativeSound);
            editedSound.panAmount = nativeSound.panAmount;
            editedSound.panModulationMode = nativeSound.panModulationMode;
            auto renderedSound = editedSound;
            wave::parameters::applyQuickEdit(renderedSound);
            performance.layers[static_cast<size_t>(editable)].sound = renderedSound;
        }
        performance.outputDb = editedSound.outputDb;
        performance.circuitAgeAmount = editedSound.circuitAgeAmount;
        applyPerformanceFaders(performance);
        engine.render(buffer, *performanceMidi, localKeyboardMidi, performance);
    }
    else
    {
        wave::parameters::applyQuickEdit(editedSound);
        engine.render(buffer, *performanceMidi, localKeyboardMidi, editedSound);
    }

    auto peak = 0.0f;
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        peak = juce::jmax(peak, buffer.getMagnitude(channel, 0, buffer.getNumSamples()));
    outputPeak.store(peak, std::memory_order_relaxed);
}

void WaveEmulationAudioProcessor::advanceFirmware(int samples)
{
    if (samples <= 0)
        return;
    if (masterFirmware.isLoaded())
    {
        if (pendingFilterCalibrationServiceSeed.exchange(
                false, std::memory_order_acq_rel))
            for (int voice = 0; voice < wave::dsp::WaldorfEngine::voiceCount; ++voice)
                masterFirmware.setFilterCalibrationCode(
                    voice, protectedFilterCalibrationCodes[
                               static_cast<size_t>(voice)]);
        const auto protectSharedCalibration
            = pendingDiskSetActivation.load(std::memory_order_acquire);
        if (pendingPanelGlideSwitchFeedback.exchange(
                false, std::memory_order_acq_rel))
        {
            panelGlideSwitchBaseline = static_cast<int>(
                masterFirmware.currentSoundRecordByte(238u) & 0x01u);
            panelGlideSwitchFeedbackBlocks = 96;
        }
        masterFirmware.runForAudioSamples(samples, currentSampleRate);
        if (const auto glideRate = pendingPanelGlideRate.exchange(
                -1, std::memory_order_acq_rel);
            glideRate >= 0)
        {
            const auto confirmedRate = juce::jlimit(0, 127, glideRate);
            masterFirmware.writeCurrentSoundRecordByte(
                234u, static_cast<uint8_t>(confirmedRate));
            masterFirmware.refreshCurrentScreenFromFirmware();
            confirmedPanelGlideRate.store(confirmedRate,
                                          std::memory_order_release);
            pendingFirmwareGlideValues[1].store(confirmedRate,
                                                std::memory_order_release);
            triggerAsyncUpdate();
        }
        if (panelGlideSwitchFeedbackBlocks > 0)
        {
            const auto enabled = static_cast<int>(
                masterFirmware.currentSoundRecordByte(238u) & 0x01u);
            if (enabled != panelGlideSwitchBaseline)
            {
                confirmedPanelGlideEnabled.store(enabled,
                                                 std::memory_order_release);
                pendingFirmwareGlideValues[5].store(enabled,
                                                    std::memory_order_release);
                panelGlideSwitchBaseline = enabled;
                panelGlideSwitchFeedbackBlocks = 0;
                triggerAsyncUpdate();
            }
            else if (--panelGlideSwitchFeedbackBlocks == 0)
            {
                panelGlideSwitchBaseline = -1;
            }
        }
        // The calibration belongs to the installed voice cards, while the OS
        // also uses this shared-work window as general scratch SRAM. Keep an
        // independent hardware-owned copy instead of trusting every transient
        // byte pattern written there. A complete, physically plausible table
        // is accepted only while the genuine Store -> Shift+Display 7 service
        // session is active; all ordinary operation restores the card trims.
        std::array<uint16_t, wave::dsp::WaldorfEngine::voiceCount> firmwareCodes{};
        auto plausibleServiceTable = true;
        for (int voice = 0; voice < wave::dsp::WaldorfEngine::voiceCount; ++voice)
        {
            const auto code = masterFirmware.filterCalibrationCode(voice);
            firmwareCodes[static_cast<size_t>(voice)] = code;
            plausibleServiceTable = plausibleServiceTable
                                    && code >= 0x0600u && code <= 0x0a00u;
        }
        if (!protectSharedCalibration
            && filterCalibrationServiceActive.load(std::memory_order_acquire)
            && plausibleServiceTable)
            protectedFilterCalibrationCodes = firmwareCodes;

        for (int voice = 0; voice < wave::dsp::WaldorfEngine::voiceCount; ++voice)
        {
            const auto code
                = protectedFilterCalibrationCodes[static_cast<size_t>(voice)];
            engine.setFilterCalibrationCode(voice, code);
            if (protectSharedCalibration)
                masterFirmware.setFilterCalibrationCode(voice, code);
        }
        if (filterCalibrationServiceExitPending.load(std::memory_order_acquire)
            && !masterFirmware.panelButtonPressed(70)
            && !masterFirmware.panelButtonPressed(71))
        {
            filterCalibrationServiceActive.store(false,
                                                 std::memory_order_release);
            filterCalibrationServiceExitPending.store(false,
                                                      std::memory_order_release);
        }
        auto selected = masterFirmware.currentPerformanceId().value_or(-1);
        auto refreshSelectedPerformancePage = false;
        const auto expected = pendingPanelPerformance.load(std::memory_order_acquire);
        if (expected >= 0)
        {
            if (selected == expected)
            {
                pendingPanelPerformance.store(-1, std::memory_order_release);
                refreshSelectedPerformancePage = true;
            }
        }
        else if (!pendingDiskSetActivation.load(std::memory_order_acquire)
                 && pendingManagerExitProgram.load(std::memory_order_acquire) < 0
                 && pendingFirmwareProgram.load(std::memory_order_acquire) < 0
                 && selected >= 0
                 && selected < wave::presets::WaveFactorySet::programCount
                 && selected != currentProgram.load(std::memory_order_acquire))
        {
            pendingEngineProgram.store(selected, std::memory_order_release);
            triggerAsyncUpdate();
            refreshSelectedPerformancePage = true;
        }

        // Mounting media only inserts the disk. Total Recall is complete once
        // OS 1.700 has transferred the SET through the real FDC register path
        // and closed its confirmation requester. Directory reads contribute
        // to the byte count, so that count alone can fire during the load.
        // Force one engine decode at that boundary because
        // A001 may have the same numeric index as the preceding empty INIT
        // state and therefore cannot be detected by an index-change test.
        if (pendingDiskSetActivation.load(std::memory_order_acquire)
            && masterFirmware.mountedDiskBytesRead()
                   >= pendingDiskSetBytes.load(std::memory_order_acquire)
            && !firmwareRequesterActive.load(std::memory_order_acquire)
            && !masterFirmware.panelButtonPressed(70))
        {
            pendingDiskSetActivation.store(false, std::memory_order_release);
            if (diskSetImportConfirmationPending.exchange(
                    false, std::memory_order_acq_rel)
                && diskMenuActive.exchange(false, std::memory_order_acq_rel))
            {
                // Disk/Load needs more than one OK: the first opens the file
                // selector, and the next starts Total Recall. Only the SET
                // transfer proves the operation has finished.
                panelSelectedMode.store(39, std::memory_order_release);
                panelSelectedEdit.store(-1, std::memory_order_release);
                panelModeDisplayTransitionActive.store(true,
                                                       std::memory_order_release);
                panelModeDisplayAwaitingDispatch.store(true,
                                                       std::memory_order_release);
                // Total Recall returns to the Disk manager. Exit that native
                // workspace before selecting the new bank: a Performance
                // contact alone is rejected while the manager owns input.
                selected = 0;
                pendingManagerExitProgram.store(selected, std::memory_order_release);
                pendingPanelCancel.store(true, std::memory_order_release);
            }
            if (const auto set = currentPerformanceSet(); set != nullptr)
            {
                masterFirmware.installSoundBank(set->soundBank());
                masterFirmware.installPerformanceBank(set->performanceBank());
            }
            pendingEngineProgram.store(selected, std::memory_order_release);
            triggerAsyncUpdate();
            refreshSelectedPerformancePage = true;
        }
        if (refreshSelectedPerformancePage
            && !storeMenuActive.load(std::memory_order_acquire))
        {
            installFactoryEditRecords(selected);
            masterFirmware.refreshCurrentScreenFromFirmware();
        }
        applyPendingHostMachineRestore();
        if (relativePanelPotRebasePending.load(std::memory_order_acquire)
            && pendingFirmwareProgram.load(std::memory_order_acquire) < 0
            && std::atomic_load_explicit(
                   &pendingHostMachineRestore, std::memory_order_acquire) == nullptr
            && masterFirmware.currentSoundRecordOffset().has_value())
        {
            for (const auto& pot : wave::panel::visiblePots)
                masterFirmware.rebaseRelativePanelPot(pot.diagnosticCode,
                                                      pot.adcChannel);
            relativePanelPotRebasePending.store(false,
                                                std::memory_order_release);
        }
    }
    for (int board = 0; board < static_cast<int>(voiceFirmwares.size()); ++board)
    {
        auto& runtime = voiceFirmwares[static_cast<size_t>(board)];
        if (!runtime.isLoaded())
            continue;
        runtime.runForAudioSamples(samples, currentSampleRate);
        const auto& writes = runtime.pendingHardwareWrites();
        for (const auto& write : writes)
            engine.applyFirmwareHardwareWrite(board, write.address, write.value);
        firmwareHardwareWrites.fetch_add(static_cast<uint64_t>(writes.size()),
                                         std::memory_order_relaxed);
        runtime.clearHardwareWrites();
    }
}

void WaveEmulationAudioProcessor::seedFilterCalibrationTable() noexcept
{
    if (!masterFirmware.isLoaded())
        return;
    for (int voice = 0; voice < wave::dsp::WaldorfEngine::voiceCount; ++voice)
    {
        const auto code
            = protectedFilterCalibrationCodes[static_cast<size_t>(voice)];
        masterFirmware.setFilterCalibrationCode(
            voice, code);
        engine.setFilterCalibrationCode(voice, code);
    }
}

void WaveEmulationAudioProcessor::handleAsyncUpdate()
{
    const auto selected = pendingEngineProgram.exchange(-1, std::memory_order_acq_rel);
    const auto set = currentPerformanceSet();
    if (selected >= 0 && selected < wave::presets::WaveFactorySet::programCount
        && set != nullptr && set->isLoaded())
    {
        if (selected != currentProgram.load(std::memory_order_acquire))
            armPanelPotProgramRecallGuard();
        // Reaching a new Performance through the firmware (including Total
        // Recall from disk) also retires the preceding Disk/Option context.
        // The LCD remains firmware-owned; this only synchronises subsequent
        // serial-button routing and the mutually-exclusive mode lamps.
        // Store selects its destination while its dialog still owns input.
        // Keep that context until Cancel completes the native exit.
        if (!storeMenuActive.load(std::memory_order_acquire))
        {
            diskMenuActive.store(false, std::memory_order_release);
            panelSelectedMode.store(39, std::memory_order_release);
            panelSelectedEdit.store(-1, std::memory_order_release);
        }
        currentProgram.store(selected, std::memory_order_release);
        parameters.state.setProperty("factoryProgram", selected, nullptr);
        // OS 1.700 has already performed the selection and owns the LCD. Decode
        // the same record into the DSP without feeding a second program-change
        // event back to the firmware.
        applyFactoryProgram(selected, false);
        updateHostDisplay(ChangeDetails{}.withProgramChanged(true)
                              .withNonParameterStateChanged(true));
    }

    const auto acceptFirmwareSoundFeedback
        = !firmwareSoundFeedbackSuspended.load(std::memory_order_acquire);
    for (size_t route = 0; route < pendingFirmwareModulationAmounts.size(); ++route)
    {
        const auto source = pendingFirmwareModulationSources[route].exchange(
            -1, std::memory_order_acq_rel);
        if (acceptFirmwareSoundFeedback && source >= 0)
            if (auto* parameter = parameters.getParameter(
                    wave::parameters::modulationSource[route]))
                parameter->setValueNotifyingHost(
                    parameter->convertTo0to1(static_cast<float>(source)));

        const auto control = pendingFirmwareModulationControls[route].exchange(
            -1, std::memory_order_acq_rel);
        if (acceptFirmwareSoundFeedback && control >= 0)
            if (auto* parameter = parameters.getParameter(
                    wave::parameters::modulationControl[route]))
                parameter->setValueNotifyingHost(
                    parameter->convertTo0to1(static_cast<float>(control)));

        const auto amount = pendingFirmwareModulationAmounts[route].exchange(
            -128, std::memory_order_acq_rel);
        if (!acceptFirmwareSoundFeedback || amount < -64)
            continue;
        if (auto* parameter = parameters.getParameter(
                wave::parameters::modulationAmount[route]))
            parameter->setValueNotifyingHost(
                parameter->convertTo0to1(static_cast<float>(amount)));
    }

    for (size_t pot = 0; pot < pendingFirmwarePanelPotValues.size(); ++pot)
    {
        const auto value = pendingFirmwarePanelPotValues[pot].exchange(
            std::numeric_limits<float>::quiet_NaN(), std::memory_order_acq_rel);
        if (!acceptFirmwareSoundFeedback || std::isnan(value))
            continue;
        if (auto* parameter = parameters.getParameter(
                wave::panel::visiblePots[pot].parameterId))
            parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
    }

    constexpr std::array<const char*, 6> glideParameterIds {
        wave::parameters::glideType,
        wave::parameters::glideRate,
        wave::parameters::glideTimeMode,
        wave::parameters::glideRateModSource,
        wave::parameters::glideRateModAmount,
        wave::parameters::glideActive
    };
    for (size_t index = 0; index < pendingFirmwareGlideValues.size(); ++index)
    {
        const auto value = pendingFirmwareGlideValues[index].exchange(
            -1000, std::memory_order_acq_rel);
        if (!acceptFirmwareSoundFeedback || value < -64)
            continue;
        if (auto* parameter = parameters.getParameter(glideParameterIds[index]))
            parameter->setValueNotifyingHost(
                parameter->convertTo0to1(static_cast<float>(value)));
        if (index == 1)
            confirmedPanelGlideRate.store(-1, std::memory_order_release);
        else if (index == 5)
            confirmedPanelGlideEnabled.store(-1, std::memory_order_release);
    }

    const auto oscillatorLink = pendingFirmwareOscillatorLink.exchange(
        -1, std::memory_order_acq_rel);
    if (acceptFirmwareSoundFeedback && oscillatorLink >= 0)
        if (auto* parameter = parameters.getParameter(
                wave::parameters::oscillatorLink))
            parameter->setValueNotifyingHost(
                parameter->convertTo0to1(static_cast<float>(oscillatorLink)));
}

void WaveEmulationAudioProcessor::synchroniseInstrumentSoundSeed() noexcept
{
    const auto seed = std::atomic_load_explicit(
        &instrumentSoundSeed, std::memory_order_acquire);
    if (seed == nullptr || seed == appliedInstrumentSoundSeed)
        return;
    activeInstrumentSoundPerformance = seed->performance;
    activeInstrumentSoundRecords = seed->records;
    activeInstrumentSoundRecordValid = seed->valid;
    // The imported SET is only an initial seed. Native Store updates the bank
    // in SRAM, so a new Performance must create its private Instrument edits
    // from those saved records, not resurrect the SET's older parameter bytes.
    const auto performanceOffset = 0x28000u
                                   + static_cast<uint32_t>(seed->performance) * 512u;
    if (masterFirmware.isLoaded()
        && masterFirmware.sharedProgramByte(performanceOffset + 48u) == 0x55u)
    {
        for (size_t instrument = 0; instrument < activeInstrumentSoundRecords.size(); ++instrument)
        {
            const auto assignment = performanceOffset + 64u
                                    + static_cast<uint32_t>(instrument) * 32u;
            const auto soundId = (masterFirmware.sharedProgramByte(assignment)
                                 | (masterFirmware.sharedProgramByte(assignment + 1u) << 7u)) & 0xffu;
            const auto soundOffset = 0x18000u + static_cast<uint32_t>(soundId) * 256u;
            auto& record = activeInstrumentSoundRecords[instrument];
            std::array<uint8_t, wave::presets::WaveFactorySet::soundSize> stored {};
            for (size_t byte = 0; byte < stored.size(); ++byte)
                stored[byte] = masterFirmware.sharedProgramByte(
                    soundOffset + static_cast<uint32_t>(byte));
            // An unpopulated startup slot still needs the imported seed.
            if (std::any_of(stored.begin(), stored.end(), [](uint8_t byte) { return byte != 0; }))
                record = stored;
        }
        // Unselected layers also sound immediately after recall. Refresh their
        // DSP snapshots at the same boundary as their private edit buffers.
        const auto current = std::atomic_load_explicit(&factoryPerformance, std::memory_order_acquire);
        if (current != nullptr)
        {
            auto recalled = std::make_shared<wave::dsp::WaldorfEngine::PerformanceSnapshot>(*current);
            for (size_t instrument = 0; instrument < recalled->layers.size(); ++instrument)
            {
                if (!activeInstrumentSoundRecordValid[instrument])
                    continue;
                auto& sound = recalled->layers[instrument].sound;
                auto saved = decodeFactorySound(activeInstrumentSoundRecords[instrument]);
                saved.panAmount = sound.panAmount;
                saved.panModulationMode = sound.panModulationMode;
                saved.driveDb = sound.driveDb;
                saved.quickEditAmounts = sound.quickEditAmounts;
                saved.outputDb = sound.outputDb;
                saved.circuitAgeAmount = sound.circuitAgeAmount;
                sound = saved;
            }
            std::atomic_store_explicit(&factoryPerformance,
                std::static_pointer_cast<const wave::dsp::WaldorfEngine::PerformanceSnapshot>(recalled),
                std::memory_order_release);
        }
    }
    activeInstrumentSoundRecordReconciled.fill(false);
    appliedInstrumentSoundSeed = seed;
}

void WaveEmulationAudioProcessor::publishHostMachineSnapshot(
    int performance, int selectedInstrument,
    std::span<const uint8_t, wave::presets::WaveFactorySet::soundSize>
        selectedSound) noexcept
{
    synchroniseInstrumentSoundSeed();
    if (performance < 0 || performance >= wave::presets::WaveFactorySet::programCount
        || selectedInstrument < 0 || selectedInstrument >= 8
        || activeInstrumentSoundPerformance != performance)
        return;

    const auto selected = static_cast<size_t>(selectedInstrument);
    std::copy(selectedSound.begin(), selectedSound.end(),
              activeInstrumentSoundRecords[selected].begin());
    activeInstrumentSoundRecordValid[selected] = true;

    const auto performanceOffset = masterFirmware.currentPerformanceRecordOffset();
    if (!performanceOffset.has_value())
        return;

    auto snapshot = std::make_shared<HostMachineSnapshot>();
    snapshot->performance = performance;
    snapshot->selectedInstrument = selectedInstrument;
    for (size_t offset = 0; offset < snapshot->performanceRecord.size(); ++offset)
        snapshot->performanceRecord[offset] = masterFirmware.sharedProgramByte(
            *performanceOffset + static_cast<uint32_t>(offset));
    snapshot->soundRecords = activeInstrumentSoundRecords;
    snapshot->soundRecordValid = activeInstrumentSoundRecordValid;

    auto hash = 1469598103934665603ull;
    const auto addByte = [&hash](uint8_t byte) {
        hash ^= byte;
        hash *= 1099511628211ull;
    };
    for (const auto byte : snapshot->performanceRecord)
        addByte(byte);
    for (size_t instrument = 0; instrument < snapshot->soundRecords.size(); ++instrument)
    {
        addByte(snapshot->soundRecordValid[instrument] ? 1u : 0u);
        if (snapshot->soundRecordValid[instrument])
            for (const auto byte : snapshot->soundRecords[instrument])
                addByte(byte);
    }
    addByte(static_cast<uint8_t>(selectedInstrument));
    if (hash == lastPublishedHostMachineHash)
        return;

    std::atomic_store_explicit(
        &latestHostMachineSnapshot,
        std::static_pointer_cast<const HostMachineSnapshot>(snapshot),
        std::memory_order_release);
    lastPublishedHostMachineHash = hash;
}

void WaveEmulationAudioProcessor::applyPendingHostMachineRestore() noexcept
{
    const auto pending = std::atomic_load_explicit(
        &pendingHostMachineRestore, std::memory_order_acquire);
    if (pending == nullptr || pendingFirmwareProgram.load(std::memory_order_acquire) >= 0)
        return;
    const auto selectedPerformance = masterFirmware.currentPerformanceId();
    if (!selectedPerformance.has_value()
        || *selectedPerformance != pending->performance)
        return;

    auto performanceRecord = pending->performanceRecord;
    const auto selected = juce::jlimit(0, 7, pending->selectedInstrument);
    performanceRecord[24] = static_cast<uint8_t>(selected);
    if (!masterFirmware.installCurrentPerformanceRecord(performanceRecord))
        return;
    if (pending->soundRecordValid[static_cast<size_t>(selected)])
        masterFirmware.installPerformanceInstrumentSoundRecord(
            selected, pending->soundRecords[static_cast<size_t>(selected)]);

    activeInstrumentSoundPerformance = pending->performance;
    activeInstrumentSoundRecords = pending->soundRecords;
    activeInstrumentSoundRecordValid = pending->soundRecordValid;
    // The running recall can still perform its initial selector conversion
    // after the host edit buffers are installed. Keep the same one-time
    // native-record reconciliation used by a normal Performance recall.
    activeInstrumentSoundRecordReconciled.fill(false);
    masterFirmware.refreshCurrentScreenFromFirmware();
    suspendFirmwareSoundFeedback();
    modulationRoutingResetPending.store(true, std::memory_order_release);
    std::atomic_store_explicit(
        &latestHostMachineSnapshot, pending, std::memory_order_release);
    std::atomic_store_explicit(
        &pendingHostMachineRestore, std::shared_ptr<const HostMachineSnapshot>{},
        std::memory_order_release);
}

void WaveEmulationAudioProcessor::stageFirmwareInstrumentSound(
    int targetInstrument) noexcept
{
    synchroniseInstrumentSoundSeed();
    if (targetInstrument < 0 || targetInstrument >= 8
        || activeInstrumentSoundPerformance
               != currentProgram.load(std::memory_order_acquire)
        || !activeInstrumentSoundRecordValid[static_cast<size_t>(targetInstrument)])
        return;

    const auto currentInstrument = masterFirmware.currentPerformanceInstrument();
    if (currentInstrument.has_value()
        && *currentInstrument == targetInstrument)
        return;
    if (currentInstrument.has_value() && *currentInstrument >= 0
        && *currentInstrument < 8
        && activeInstrumentSoundRecordValid[static_cast<size_t>(*currentInstrument)])
    {
        const auto currentIndex = static_cast<size_t>(*currentInstrument);
        auto& currentRecord = activeInstrumentSoundRecords[currentIndex];
        const auto previousPersistentSelector = currentRecord[25];
        for (size_t offset = 0;
             offset < wave::presets::WaveFactorySet::soundSize; ++offset)
            currentRecord[offset] = masterFirmware.currentSoundRecordByte(
                static_cast<uint32_t>(offset));
        if (!activeInstrumentSoundRecordReconciled[currentIndex]
            && currentRecord[25] < 0x7fu
            && static_cast<uint8_t>(currentRecord[25] + 1u)
                   == previousPersistentSelector)
            currentRecord[25] = previousPersistentSelector;
        activeInstrumentSoundRecordReconciled[currentIndex] = true;
    }

    // Two Instruments may intentionally assign the same stored Sound number
    // (A001 assigns a002 to both I1 and I2). The Wave nevertheless edits an
    // Instrument-local copy. Restore that copy before OS 1.700 consumes the
    // serial selection action; otherwise both Instruments alias one writable
    // bank record and every panel edit leaks across them.
    masterFirmware.installPerformanceInstrumentSoundRecord(
        targetInstrument,
        activeInstrumentSoundRecords[static_cast<size_t>(targetInstrument)]);
    activeInstrumentSoundRecordReconciled[static_cast<size_t>(targetInstrument)]
        = true;
    suspendFirmwareSoundFeedback();
    modulationRoutingResetPending.store(true, std::memory_order_release);
}

void WaveEmulationAudioProcessor::runFirmwareTimeline(const juce::MidiBuffer& midi,
                                                       int sampleCount)
{
    // The lower keyboard switches send serial press=1/release=0 events.
    // Keep each edge until the genuine callback has applied its state: OS
    // background work can consume a packet without dispatching it.
    if (activeKeyboardButtonEvent >= 0)
    {
        const auto button = static_cast<uint32_t>(activeKeyboardButtonEvent >> 1);
        const auto serial = static_cast<uint8_t>(3u + button);
        const auto down = static_cast<uint8_t>(activeKeyboardButtonEvent & 1);
        const auto pending = masterFirmware.panelEventPending(0x80u, serial, down);
        if (!pending && masterFirmware.localByte(0x58f78u + button)
                            == activeKeyboardButtonExpectedState)
        {
            if (down == 0u)
                masterFirmware.releasePanelEventLatch(serial);
            activeKeyboardButtonEvent = -1;
        }
        else if (!pending && --keyboardButtonRetryBlocks <= 0)
        {
            masterFirmware.pushPanelEvent(0x80u, serial, down);
            keyboardButtonRetryBlocks = 8;
        }
    }
    if (activeKeyboardButtonEvent < 0)
    {
        const auto event = keyboardButtonEventFifo.read(1);
        if (event.blockSize1 != 0)
        {
            const auto packed = keyboardButtonEvents[static_cast<size_t>(event.startIndex1)];
            const auto button = static_cast<uint32_t>(packed >> 1u);
            const auto down = static_cast<uint8_t>(packed & 1u);
            const auto state = masterFirmware.localByte(0x58f78u + button) != 0u;
            const auto record = masterFirmware.currentPerformanceRecordOffset();
            const auto toggle = record.has_value()
                && masterFirmware.sharedProgramByte(*record + 6u + button) != 0u;
            activeKeyboardButtonExpectedState = toggle ? (down != 0u ? !state : state)
                                                       : down != 0u;
            activeKeyboardButtonEvent = packed;
            keyboardButtonRetryBlocks = 8;
            masterFirmware.pushPanelEvent(0x80u, static_cast<uint8_t>(3u + button), down);
        }
    }
    synchroniseInstrumentSoundSeed();
    if (const auto page = masterFirmware.currentInstrumentEditPage())
        instrumentEditPage.store(*page, std::memory_order_release);
    instrumentFaderRecordBaselineValid = false;
    if (panelSelectedMode.load(std::memory_order_acquire) == 36
        && panelSelectedEdit.load(std::memory_order_acquire) < 0
        && instrumentEditPage.load(std::memory_order_acquire) <= 1)
    {
        auto packedEdit = -1;
        for (const auto& edit : pendingInstrumentFaderEdits)
            if ((packedEdit = edit.load(std::memory_order_acquire)) >= 0)
                break;
        if (packedEdit >= 0)
        {
            const auto performance = (packedEdit >> 11) & 0xff;
            const auto instrument = (packedEdit >> 8) & 0x07;
            const auto firmwarePerformance = masterFirmware.currentPerformanceId();
            const auto firmwareInstrument
                = masterFirmware.currentInstrumentEditTarget();
            const auto record = masterFirmware.currentPerformanceRecordOffset();
            if (firmwarePerformance.has_value()
                && firmwareInstrument.has_value() && record.has_value()
                && *firmwarePerformance == performance
                && *firmwareInstrument == instrument)
            {
                constexpr uint32_t instrumentTable = 64u;
                constexpr uint32_t instrumentRecordSize = 32u;
                const auto base = *record + instrumentTable
                                  + static_cast<uint32_t>(instrument)
                                        * instrumentRecordSize;
                for (size_t offset = 0;
                     offset < instrumentFaderRecordBaseline.size(); ++offset)
                    instrumentFaderRecordBaseline[offset]
                        = masterFirmware.sharedProgramByte(
                            base + static_cast<uint32_t>(offset));
                instrumentFaderBaselinePerformance = performance;
                instrumentFaderBaselineInstrument = instrument;
                instrumentFaderRecordBaselineValid = true;
            }
        }
    }
    const auto cancelStepEvents = cancelPanelStepButtonEvents.exchange(
        false, std::memory_order_acq_rel);
    const auto completedPerformanceStore
        = storeMenuActive.load(std::memory_order_acquire)
          && completedStoreMode.load(std::memory_order_acquire) == 39
          && !firmwareRequesterActive.load(std::memory_order_acquire);
    const auto performanceOwnsStepping
        = (panelSelectedMode.load(std::memory_order_acquire) == 39
           || completedPerformanceStore)
          && panelSelectedEdit.load(std::memory_order_acquire) < 0;
    if (cancelStepEvents || performanceOwnsStepping)
    {
        // +/- is contextual. An edit page can leave a serial press waiting
        // while the user returns to Performance; if it is consumed after the
        // page change, OS 1.700 treats it as a held patch selector. Retire the
        // complete old transaction, including a press already queued in the
        // genuine controller ring, before Performance owns these serials.
        // Performance clicks use exact program requests, so no +/- serial
        // hold belongs on that page. Repeat this on the render thread: a
        // paused firmware instruction can finish installing a stale latch
        // after a one-off clear, and an old edit retry can arrive later.
        pendingPanelStepButtonEvents.store(0, std::memory_order_release);
        queuedPanelStepButtonEvents = 0;
        activePanelStepButtonDiagnosticCode = -1;
        activePanelStepButtonWaitBlocks = 0;
        for (const auto diagnosticCode : { 69, 72 })
        {
            const auto matrix
                = wave::panel::matrixIndexForDiagnosticCode(diagnosticCode);
            if (matrix >= 0)
            {
                if (cancelStepEvents)
                    panelButtonDown[static_cast<size_t>(matrix)].store(
                        false, std::memory_order_release);
                masterFirmware.setPanelButton(matrix, false);
            }
            // Remove an edge which is still in the genuine controller ring;
            // clearing the repeat latch handles one which OS 1.700 had
            // already consumed before this audio callback.
            masterFirmware.discardPendingPanelButtonEvents(diagnosticCode);
            masterFirmware.releasePanelEventLatch(diagnosticCode);
        }
    }
    // On Instrument Edit's fourth (Zoning) page, the eight switches above the
    // LCD are macro display keys, not Instrument selectors. OS 1.700 installs
    // its Layer handler at the first display action while that page is live.
    // Mirror that firmware-owned page state for the message-thread wiring so
    // a Layer click cannot leave the Instrument-selection retry transaction
    // running behind its confirmation requester.
    constexpr uint32_t firstDisplayActionPressCallback = 0x56618u;
    constexpr uint32_t firstDisplayActionReleaseCallback = 0x5661cu;
    constexpr uint32_t lastDisplayActionReleaseCallback = 0x5668cu;
    constexpr uint32_t cancelActionCallback = 0x56728u;
    constexpr uint32_t okActionCallback = 0x56738u;
    const auto readFirmwareLong = [this](uint32_t address) {
        return (static_cast<uint32_t>(masterFirmware.localByte(address)) << 24u)
               | (static_cast<uint32_t>(masterFirmware.localByte(address + 1u))
                  << 16u)
               | (static_cast<uint32_t>(masterFirmware.localByte(address + 2u))
                  << 8u)
               | static_cast<uint32_t>(masterFirmware.localByte(address + 3u));
    };
    instrumentZoningPageActive.store(
        readFirmwareLong(firstDisplayActionPressCallback) == 0x0001ce84u
            && readFirmwareLong(firstDisplayActionReleaseCallback) == 0x00015014u
            && readFirmwareLong(lastDisplayActionReleaseCallback) == 0x000165f2u,
        std::memory_order_release);
    // Requesters replace the otherwise inert CANCEL and OK action callbacks
    // in the OS-owned front-panel action table. This is the firmware's modal
    // state, independent of LCD text or editor state. The requester owns
    // the panel; name editors also retain their cursor controls.
    const auto installedCancelAction
        = readFirmwareLong(cancelActionCallback);
    const auto installedOkAction = readFirmwareLong(okActionCallback);
    // Outside a requester both response contacts share the same inert action,
    // even when that action is not the boot-time default. A requester installs
    // distinct CANCEL and OK handlers, then restores a shared action when it
    // closes. This is the genuine firmware modal level, not an LCD heuristic.
    firmwareRequesterActive.store(
        installedCancelAction != installedOkAction,
        std::memory_order_release);
    if (storeSaveCompletionPending.load(std::memory_order_acquire)
        && installedCancelAction == installedOkAction
        && !masterFirmware.panelButtonPressed(70))
    {
        // The naming requester has closed after OK. Store retains its native
        // chooser for another save and redraws the preceding operating page.
        // Restore that page's lamp as well: the record type being saved does
        // not select a new operating mode in the native OS.
        storeSaveCompletionPending.store(false, std::memory_order_release);
        completedStoreMode.store(storeReturnMode.load(std::memory_order_acquire),
                                 std::memory_order_release);
    }
    // Total Recall asks about optional machine-specific calibration only after
    // the Sound/Performance banks have transferred. CANCEL here means "No";
    // it does not cancel the SET load or require reading its optional tail.
    firmwareDiskCalibrationRequesterActive.store(
        installedCancelAction == 0x0001f494u
            && installedOkAction == 0x0001f488u,
        std::memory_order_release);
    // OS 1.700 installs these Page callbacks for DOS name entry (including
    // Format's Diskname). Detect the actual editor, independent of panel mode.
    firmwareDiskNameEditorActive.store(
        readFirmwareLong(0x56698u) == 0x00013c1cu
            && readFirmwareLong(0x566a8u) == 0x00013c34u,
        std::memory_order_release);
    if (cancelInstrumentPanelEvents.exchange(false, std::memory_order_acq_rel))
    {
        constexpr std::array instrumentCodes { 22, 25, 26, 27,
                                                79, 28, 29, 30 };
        for (const auto code : instrumentCodes)
            masterFirmware.releasePanelActionBit(code);
        pendingFirmwareInstrument.store(-1, std::memory_order_release);
        pendingInstrumentPageSelection.store(-1, std::memory_order_release);
        activeInstrumentPanelDiagnosticCode = -1;
        activeInstrumentPanelTarget = -1;
        activeInstrumentPanelPressSent = false;
        activeInstrumentPanelRetryBlocks = 0;
        activeInstrumentPanelHoldBlocks = 0;
        scheduledInstrumentPageSelection = -1;
        scheduledInstrumentPageDelayBlocks = 0;
    }
    if (cancelFirmwareSoftButtonEvents.exchange(false,
                                                std::memory_order_acq_rel))
    {
        pendingFirmwareSoftButton.store(-1, std::memory_order_release);
        if (activeSoftButtonDiagnosticCode >= 0)
        {
            masterFirmware.releasePanelEventLatch(activeSoftButtonDiagnosticCode);
            masterFirmware.releasePanelActionBit(activeSoftButtonDiagnosticCode);
        }
        activeSoftButtonDiagnosticCode = -1;
        activeSoftButtonPressSent = false;
        activeSoftButtonAccepted = false;
        activeSoftButtonReleaseSent = false;
        activeSoftButtonHoldBlocks = 0;
        activeSoftButtonRetryCount = 0;
        activeSoftButtonInitialLcd.fill(0);
    }
    if (pendingPanelCancel.exchange(false, std::memory_order_acq_rel))
    {
        masterFirmware.releasePanelEventLatch(71);
        pendingFirmwareSoftButton.store(71, std::memory_order_release);
    }
    if (cancelInstrumentPageSelection.exchange(false, std::memory_order_acq_rel))
    {
        pendingInstrumentPageSelection.store(-1, std::memory_order_release);
        scheduledInstrumentPageSelection = -1;
        scheduledInstrumentPageDelayBlocks = 0;
        instrumentPageReady.store(false, std::memory_order_release);
        startInstrumentPageSelectionDelay.store(false,
                                                std::memory_order_release);
    }
    if (cancelActiveInstrumentSelection.exchange(false, std::memory_order_acq_rel))
    {
        constexpr std::array instrumentCodes { 22, 25, 26, 27,
                                                79, 28, 29, 30 };
        for (const auto code : instrumentCodes)
            masterFirmware.releasePanelActionBit(code);
        activeInstrumentPanelDiagnosticCode = -1;
        activeInstrumentPanelTarget = -1;
        activeInstrumentPanelPressSent = false;
        activeInstrumentPanelRetryBlocks = 0;
        activeInstrumentPanelHoldBlocks = 0;
    }
    const auto shiftDown = keyboardControllerShiftDown.load(std::memory_order_acquire);
    if (shiftDown != firmwareKeyboardShiftDown)
    {
        firmwareKeyboardShiftDown = shiftDown;
        firmwareKeyboardShiftRetryBlocks = 0;
    }
    if (shiftDown != (masterFirmware.localByte(0x59869u) != 0u)
        && !masterFirmware.panelEventPending(0x80u, 83u, shiftDown ? 1u : 0u)
        && --firmwareKeyboardShiftRetryBlocks <= 0)
    {
        // Serial 83 maps to native action 83, the keyboard Shift modifier.
        // The Store page can consume this edge during redraw.
        // Retry until its genuine handler acknowledges the physical state.
        masterFirmware.pushPanelEvent(0x80u, 83u, shiftDown ? 1u : 0u);
        firmwareKeyboardShiftRetryBlocks = 8;
    }

    // A newer click can arrive while the previous program-change handler
    // still owns its tagged MIDI cursor and edit records. Keep the latest
    // destination queued until that recall finishes, rather than replacing
    // its tag and stranding the held LCD frame.
    if (const auto requestedProgram = masterFirmware.performanceSelectionPending()
            ? -1 : pendingFirmwareProgram.exchange(-1, std::memory_order_acq_rel);
        requestedProgram >= 0)
    {
        pendingFirmwareGlideSwitchClicks.store(0, std::memory_order_release);
        queuedFirmwareGlideSwitchClicks = 0;
        firmwareGlideSwitchTransactionActive = false;
        firmwareGlideSwitchReleaseSent = false;
        firmwareGlideSwitchBaseline = -1;
        firmwareGlideSwitchWaitBlocks = 0;
        firmwareGlideSwitchRetryCount = 0;
        masterFirmware.discardPendingPanelButtonEvents(6);
        masterFirmware.releasePanelEventLatch(6);
        pendingFirmwareInstrument.store(-1, std::memory_order_release);
        activeInstrumentPanelDiagnosticCode = -1;
        activeInstrumentPanelTarget = -1;
        activeInstrumentPanelPressSent = false;
        activeInstrumentPanelRetryBlocks = 0;
        activeInstrumentPanelHoldBlocks = 0;
        pendingInstrumentPageSelection.store(-1, std::memory_order_release);
        scheduledInstrumentPageSelection = -1;
        scheduledInstrumentPageDelayBlocks = 0;
        instrumentPageReady.store(false, std::memory_order_release);
        startInstrumentPageSelectionDelay.store(false,
                                                std::memory_order_release);
        pendingFirmwareSoftButton.store(-1, std::memory_order_release);
        activeSoftButtonDiagnosticCode = -1;
        activeSoftButtonPressSent = false;
        activeSoftButtonAccepted = false;
        activeSoftButtonReleaseSent = false;
        activeSoftButtonHoldBlocks = 0;
        activeSoftButtonRetryCount = 0;
        activeSoftButtonInitialLcd.fill(0);
        pendingFirmwarePageButton.store(-1, std::memory_order_release);
        pendingFirmwareModeButton.store(-1, std::memory_order_release);
        panelModeDisplayTransitionActive.store(false,
                                               std::memory_order_release);
        panelModeDisplayAwaitingDispatch.store(false,
                                              std::memory_order_release);
        activeModeButtonDiagnosticCode = -1;
        activeModeButtonPressSent = false;
        activeModeButtonReleaseSent = false;
        activeModeButtonHoldBlocks = 0;
        activeModeButtonSettleBlocks = 0;
        activeModeButtonRetryCount = 0;
        activeModeButtonInitialScreenCallback = 0;
        activeModeButtonInitialLcd.fill(0);
        // Program selection can originate on the message thread while the
        // firmware CPU runs on the audio thread. Install the corresponding
        // edit records here, immediately before feeding the MIDI program
        // change to OS 1.700, so the firmware can never rasterise a mixture of
        // two Performances from a concurrently copied shared-memory record.
        const auto set = currentPerformanceSet();
        if (set == nullptr || !set->isLoaded())
            return;
        if (installFactoryEditRecords(requestedProgram))
            masterFirmware.requestPerformanceSelection(requestedProgram);
    }
    if (activeInstrumentPanelDiagnosticCode >= 0)
    {
        if (!activeInstrumentPanelPressSent)
        {
            masterFirmware.pushPanelEvent(
                0x80u, static_cast<uint8_t>(activeInstrumentPanelDiagnosticCode), 1u);
            activeInstrumentPanelPressSent = true;
            activeInstrumentPanelRetryBlocks = 8;
            activeInstrumentPanelHoldBlocks = 8;
        }
        else if (!masterFirmware.panelEventPending(
                     0x80u,
                     static_cast<uint8_t>(activeInstrumentPanelDiagnosticCode),
                     1u)
                 && (panelSelectedMode.load(std::memory_order_acquire) == 36
                         ? masterFirmware.currentInstrumentEditTarget()
                         : masterFirmware.currentPerformanceInstrument())
                 == activeInstrumentPanelTarget)
        {
            masterFirmware.releasePanelActionBit(
                activeInstrumentPanelDiagnosticCode);
            activeInstrumentPanelDiagnosticCode = -1;
            activeInstrumentPanelTarget = -1;
            activeInstrumentPanelPressSent = false;
            activeInstrumentPanelRetryBlocks = 0;
            activeInstrumentPanelHoldBlocks = 0;
            if (panelSelectedMode.load(std::memory_order_acquire) == 36)
                instrumentPageReady.store(true, std::memory_order_release);
        }
        else if (activeInstrumentPanelHoldBlocks > 0)
        {
            --activeInstrumentPanelHoldBlocks;
        }
        else if (--activeInstrumentPanelRetryBlocks <= 0)
        {
            // The OS keeps Instrument 1-8 as action bits rather than ordinary
            // auto-repeat buttons. Its main loop can consume a short press and
            // release before the Instrument page observes that bit. Re-open
            // the target bit and retry instead of blocking all later clicks.
            constexpr std::array instrumentCodes { 22, 25, 26, 27,
                                                    79, 28, 29, 30 };
            for (const auto code : instrumentCodes)
                masterFirmware.releasePanelActionBit(code);
            activeInstrumentPanelPressSent = false;
            activeInstrumentPanelHoldBlocks = 0;
        }
    }
    else if (const auto requestedInstrument = pendingFirmwareInstrument.exchange(
                 -1, std::memory_order_acq_rel);
             requestedInstrument >= 0)
    {
        stageFirmwareInstrumentSound(requestedInstrument);
        constexpr std::array instrumentCodes { 22, 25, 26, 27,
                                                79, 28, 29, 30 };
        activeInstrumentPanelDiagnosticCode
            = instrumentCodes[static_cast<size_t>(requestedInstrument)];
        activeInstrumentPanelTarget = requestedInstrument;
        activeInstrumentPanelPressSent = false;
        activeInstrumentPanelRetryBlocks = 0;
        activeInstrumentPanelHoldBlocks = 0;
        for (const auto code : instrumentCodes)
            masterFirmware.releasePanelActionBit(code);
    }
    if (activeModeButtonDiagnosticCode >= 0)
    {
        if (!activeModeButtonPressSent)
        {
            masterFirmware.pushPanelEvent(
                0x80u, static_cast<uint8_t>(activeModeButtonDiagnosticCode), 1u);
            activeModeButtonPressSent = true;
            // Edit-page handlers only need the debounced edge. Their LCD
            // callbacks should appear promptly; operating-mode changes retain
            // the longer hold required by the OS's main page dispatcher.
            activeModeButtonHoldBlocks
                = isPanelEditPageButton(activeModeButtonDiagnosticCode) ? 2 : 8;
        }
        else if (activeModeButtonHoldBlocks > 0)
        {
            --activeModeButtonHoldBlocks;
        }
        else if (!activeModeButtonReleaseSent
                 && masterFirmware.panelEventPending(
                     0x80u,
                     static_cast<uint8_t>(activeModeButtonDiagnosticCode),
                     1u))
        {
            // The firmware has not consumed the press yet. Sending release
            // now would let the delayed press become a repeating page action.
        }
        else if (!activeModeButtonReleaseSent)
        {
            // Page buttons use the controller's ordinary serial release edge.
            // Clearing the firmware latch directly can pre-empt the page
            // handler and leave the LCD blank or the action repeating.
            masterFirmware.pushPanelEvent(
                0x80u, static_cast<uint8_t>(activeModeButtonDiagnosticCode), 0u);
            activeModeButtonReleaseSent = true;
            activeModeButtonSettleBlocks = 16;
        }
        else if (masterFirmware.panelEventPending(
                     0x80u,
                     static_cast<uint8_t>(activeModeButtonDiagnosticCode),
                     0u))
        {
            // Wait until the matching release has also reached the dispatcher.
        }
        else
        {
            const auto currentScreenCallback = [&] {
                constexpr uint32_t address = 0x56bb0u;
                return (static_cast<uint32_t>(masterFirmware.localByte(address))
                        << 24u)
                       | (static_cast<uint32_t>(
                              masterFirmware.localByte(address + 1u))
                          << 16u)
                       | (static_cast<uint32_t>(
                              masterFirmware.localByte(address + 2u))
                          << 8u)
                       | static_cast<uint32_t>(
                           masterFirmware.localByte(address + 3u));
            }();
            const auto screenChanged
                = currentScreenCallback != activeModeButtonInitialScreenCallback
                  || masterFirmware.lcdVideoSnapshot()
                         != activeModeButtonInitialLcd;
            if (screenChanged
                || --activeModeButtonSettleBlocks <= 0)
            {
                constexpr auto maximumModeButtonRetries = 7;
                if (!screenChanged
                    && activeModeButtonRetryCount < maximumModeButtonRetries)
                {
                    ++activeModeButtonRetryCount;
                    // A busy firmware main loop can consume a complete serial
                    // press/release pair without running its page action. Keep
                    // one physical click alive as discrete attempts until the
                    // screen callback confirms the requested transition.
                    releasePanelPageButtonLatches(masterFirmware);
                    activeModeButtonPressSent = false;
                    activeModeButtonReleaseSent = false;
                    activeModeButtonHoldBlocks = 0;
                    activeModeButtonSettleBlocks = 0;
                }
                else
                {
                    activeModeButtonDiagnosticCode = -1;
                    activeModeButtonPressSent = false;
                    activeModeButtonReleaseSent = false;
                    activeModeButtonHoldBlocks = 0;
                    activeModeButtonSettleBlocks = 0;
                    activeModeButtonRetryCount = 0;
                    activeModeButtonInitialScreenCallback = 0;
                    activeModeButtonInitialLcd.fill(0);
                    // LCD publication has its own firmware-write completion
                    // boundary below. Button debounce finishing must not end
                    // that display transaction early.
                }
            }
        }
    }
    else if (const auto requestedModeButton = pendingFirmwareModeButton.exchange(
                 -1, std::memory_order_acq_rel);
             requestedModeButton >= 0)
    {
        if (requestedModeButton == 39)
        {
            // +/- belongs to the page that was active when its serial edge
            // was generated. Retire old Edit/Disk events exactly once at the
            // firmware-confirmed transition into Performance. Doing this for
            // every Performance increment races the following physical click
            // across audio callbacks and makes only the first + observable.
            pendingPanelStepButtonEvents.store(0, std::memory_order_release);
            queuedPanelStepButtonEvents = 0;
            activePanelStepButtonDiagnosticCode = -1;
            activePanelStepButtonWaitBlocks = 0;
            for (const auto diagnosticCode : { 69, 72 })
            {
                masterFirmware.discardPendingPanelButtonEvents(diagnosticCode);
                masterFirmware.releasePanelEventLatch(diagnosticCode);
            }
        }
        // The firmware keeps the last operating-mode action latched after the
        // page has opened. Normalise that completed action before starting a
        // new discrete mode click; otherwise the first press merely releases
        // the old page and the user has to press the new page twice.
        releasePanelPageButtonLatches(masterFirmware);
        activeModeButtonDiagnosticCode = requestedModeButton;
        activeModeButtonPressSent = false;
        activeModeButtonReleaseSent = false;
        activeModeButtonHoldBlocks = 0;
        activeModeButtonSettleBlocks = 0;
        activeModeButtonRetryCount = 0;
        constexpr uint32_t callbackAddress = 0x56bb0u;
        activeModeButtonInitialScreenCallback
            = (static_cast<uint32_t>(masterFirmware.localByte(callbackAddress))
               << 24u)
              | (static_cast<uint32_t>(
                     masterFirmware.localByte(callbackAddress + 1u))
                 << 16u)
              | (static_cast<uint32_t>(
                     masterFirmware.localByte(callbackAddress + 2u))
                 << 8u)
              | static_cast<uint32_t>(
                  masterFirmware.localByte(callbackAddress + 3u));
        activeModeButtonInitialLcd = masterFirmware.lcdVideoSnapshot();
        modeDisplayWriteStartCount = masterFirmware.lcdVideoWriteCount();
        modeDisplayLastWriteCount = modeDisplayWriteStartCount;
        modeDisplayQuietSamples = 0;
        modeDisplayObservedWrites = false;
        modeDisplayObservedCallbackChange = false;
        panelModeDisplayAwaitingDispatch.store(false,
                                              std::memory_order_release);
    }
    if (const auto requestedPageButton = pendingFirmwarePageButton.exchange(
            -1, std::memory_order_acq_rel);
        requestedPageButton >= 0)
    {
        // OS 1.700 installs a pair of native callbacks for each direction,
        // including name-cursor movement in Store. Invoke the pair once so
        // a brief host click cannot be lost in the key-repeat scheduler.
        masterFirmware.navigatePageWithFirmware(requestedPageButton == 23);
    }
    if (activeSoftButtonDiagnosticCode >= 0)
    {
        if (!activeSoftButtonPressSent)
        {
            masterFirmware.pushPanelEvent(
                0x80u, static_cast<uint8_t>(activeSoftButtonDiagnosticCode), 1u);
            activeSoftButtonPressSent = true;
            activeSoftButtonAccepted = false;
            activeSoftButtonHoldBlocks = 0;
        }
        else if (!activeSoftButtonAccepted
                 && (masterFirmware.panelActionBitActive(activeSoftButtonDiagnosticCode)
                     || (activeSoftButtonDiagnosticCode == 71
                         && (masterFirmware.localByte(0x58feau) & 2u) != 0u)))
        {
            // The display-action bit is set only after OS 1.700 has accepted
            // this softkey and entered the current Wave Edit menu handler.
            activeSoftButtonAccepted = true;
            // CANCEL is dispatched on release. Its press is acknowledged in
            // the OK/CANCEL chord latch, not the display-softkey action bits.
            activeSoftButtonHoldBlocks = activeSoftButtonDiagnosticCode == 71 ? 0 : 32;
        }
        else if (!activeSoftButtonAccepted
                 && masterFirmware.panelEventPending(
                     0x80u,
                     static_cast<uint8_t>(activeSoftButtonDiagnosticCode),
                     1u))
        {
            // The press has not reached the menu handler yet.
        }
        else if (!activeSoftButtonAccepted
                 && ++activeSoftButtonHoldBlocks >= 8)
        {
            // A busy Wave Edit screen can remove a press from the serial ring
            // before setting its action bit. That edge was demonstrably
            // rejected, so it is safe to try the same physical click again.
            if (++activeSoftButtonRetryCount < 8)
            {
                activeSoftButtonPressSent = false;
                activeSoftButtonHoldBlocks = 0;
            }
            else
            {
                activeSoftButtonDiagnosticCode = -1;
                activeSoftButtonPressSent = false;
                activeSoftButtonAccepted = false;
                activeSoftButtonReleaseSent = false;
                activeSoftButtonHoldBlocks = 0;
                activeSoftButtonRetryCount = 0;
                activeSoftButtonInitialLcd.fill(0);
            }
        }
        else if (activeSoftButtonAccepted && !activeSoftButtonReleaseSent
                 && (masterFirmware.lcdVideoSnapshot()
                         != activeSoftButtonInitialLcd
                     || --activeSoftButtonHoldBlocks <= 0))
        {
            const auto changed
                = masterFirmware.lcdVideoSnapshot() != activeSoftButtonInitialLcd;
            if (changed
                && readFirmwareLong(cancelActionCallback)
                       != readFirmwareLong(okActionCallback))
            {
                // A requester may reuse the handlers installed by the
                // preceding requester. The LCD transition produced by a newly
                // accepted softkey is therefore the second genuine open edge.
                firmwareRequesterActive.store(true,
                                              std::memory_order_release);
                if (storeMenuActive.load(std::memory_order_acquire))
                    completedStoreMode.store(-1, std::memory_order_release);
            }
            masterFirmware.pushPanelEvent(
                0x80u, static_cast<uint8_t>(activeSoftButtonDiagnosticCode), 0u);
            activeSoftButtonReleaseSent = true;
        }
        else if (activeSoftButtonReleaseSent
                 && masterFirmware.panelEventPending(
                     0x80u,
                     static_cast<uint8_t>(activeSoftButtonDiagnosticCode),
                     0u))
        {
            // Let the firmware consume its normal release edge first.
        }
        else if (activeSoftButtonReleaseSent)
        {
            // A redraw can reject the serial release just as it rejects a
            // press. The menu action has already completed, so clearing any
            // surviving display bit here cannot duplicate or cancel it.
            masterFirmware.releasePanelActionBit(
                activeSoftButtonDiagnosticCode);
            if (activeSoftButtonDiagnosticCode == 71
                && storeExitModePending.exchange(false, std::memory_order_acq_rel))
            {
                panelModeDisplayTransitionActive.store(true, std::memory_order_release);
                panelModeDisplayAwaitingDispatch.store(true, std::memory_order_release);
                const auto mode = storeExitMode.load(std::memory_order_acquire);
                pendingFirmwareModeButton.store(mode, std::memory_order_release);
                if (mode == 36)
                    startInstrumentPageSelectionDelay.store(true, std::memory_order_release);
            }
            if (activeSoftButtonDiagnosticCode == 71)
            {
                if (const auto program = pendingManagerExitProgram.exchange(
                        -1, std::memory_order_acq_rel); program >= 0)
                {
                    // A newer browsing click can arrive as Cancel retires.
                    // Preserve that newer request rather than replacing it.
                    auto empty = -1;
                    pendingFirmwareProgram.compare_exchange_strong(
                        empty, program, std::memory_order_release,
                        std::memory_order_relaxed);
                }
            }
            activeSoftButtonDiagnosticCode = -1;
            activeSoftButtonPressSent = false;
            activeSoftButtonAccepted = false;
            activeSoftButtonReleaseSent = false;
            activeSoftButtonHoldBlocks = 0;
            activeSoftButtonRetryCount = 0;
            activeSoftButtonInitialLcd.fill(0);
        }
    }
    else if (const auto requestedSoftButton = pendingFirmwareSoftButton.exchange(
                 -1, std::memory_order_acq_rel);
             requestedSoftButton >= 0)
    {
        activeSoftButtonDiagnosticCode = requestedSoftButton;
        activeSoftButtonPressSent = false;
        activeSoftButtonAccepted = false;
        activeSoftButtonReleaseSent = false;
        activeSoftButtonHoldBlocks = 0;
        activeSoftButtonRetryCount = 0;
        activeSoftButtonInitialLcd = masterFirmware.lcdVideoSnapshot();
        // Clear a stale OS action bit before producing a new discrete edge.
        masterFirmware.releasePanelActionBit(activeSoftButtonDiagnosticCode);
    }
    if (startInstrumentPageSelectionDelay.exchange(false,
                                                    std::memory_order_acq_rel))
        // The Instrument page needs the mode press to reach the OS before its
        // first Instrument soft-key is sent. Eight blocks match the physical
        // mode-button hold above; the former 32-block guard left the complete
        // LCD unnecessarily waiting for roughly a third of a second.
        scheduledInstrumentPageDelayBlocks = 8;
    if (const auto requestedSelection = pendingInstrumentPageSelection.exchange(
            -1, std::memory_order_acq_rel);
        requestedSelection >= 0)
        scheduledInstrumentPageSelection = requestedSelection;
    if (scheduledInstrumentPageSelection >= 0
        && panelSelectedMode.load(std::memory_order_acquire) == 36
        && !storeExitModePending.load(std::memory_order_acquire)
        && activeSoftButtonDiagnosticCode < 0
        && pendingFirmwareSoftButton.load(std::memory_order_acquire) < 0)
    {
        if (scheduledInstrumentPageDelayBlocks > 0)
            --scheduledInstrumentPageDelayBlocks;
        else if (masterFirmware.currentInstrumentEditPage().has_value())
        {
            // A Store exit can take longer than the nominal mode-button hold.
            // Its chooser owns these same eight softkeys until OS 1.700 has
            // actually installed Instrument Edit's callbacks.
            pendingFirmwareInstrument.store(scheduledInstrumentPageSelection,
                                            std::memory_order_release);
            scheduledInstrumentPageSelection = -1;
        }
    }
    sendPendingPerformanceFadersToFirmware();
    sendPendingPanelEncodersToFirmware();
    sendPendingPanelGlideSwitchesToFirmware();
    // The ASCII side of the controller repeats while the physical key is held.
    if (shiftDown)
        masterFirmware.pushKeyboardByte(0x52u);

    auto cursor = 0;
    for (const auto metadata : midi)
    {
        const auto eventSample = juce::jlimit(0, sampleCount, metadata.samplePosition);
        advanceFirmware(eventSample - cursor);
        const auto& message = metadata.getMessage();
        const auto* bytes = message.getRawData();
        for (int byte = 0; byte < message.getRawDataSize(); ++byte)
            masterFirmware.pushMidiByte(0, bytes[byte]);
        cursor = eventSample;
    }
    advanceFirmware(sampleCount - cursor);
    // The missing ASIC transfers contextual Instrument-fader values after the
    // CPU has sampled the ADC. Commit at that same boundary; writing before
    // the scan lets the OS finish processing the preceding fader position and
    // replace the newly committed Performance byte with its stale value.
    sendPendingInstrumentFadersToFirmware();
    sendPendingGlideFadersToFirmware();
    // Instrument Edit pages beyond Page 1 are interpreted by the real OS.
    // Feed the resulting native Instrument record back into the audio engine;
    // the panel never supplies DSP values directly and the LCD remains solely
    // firmware-driven.
    synchronisePerformanceInstrumentsFromFirmware();

    // A mode button's serial press/release transaction remains alive after
    // its LCD page has finished rasterising. Publish as soon as the firmware
    // has completed a full write pass and then gone quiet for 512 samples,
    // independent of the host's audio-buffer size, instead of making the
    // monitor wait for the unrelated debounce/retry tail. The LCD still
    // receives only genuine firmware VRAM.
    if (panelModeDisplayTransitionActive.load(std::memory_order_acquire)
        && !panelModeDisplayAwaitingDispatch.load(std::memory_order_acquire))
    {
        const auto instrumentPageActionPending
            = panelSelectedMode.load(std::memory_order_acquire) == 36
              && (pendingFirmwareInstrument.load(std::memory_order_acquire) >= 0
                  || scheduledInstrumentPageSelection >= 0
                  || activeInstrumentPanelDiagnosticCode >= 0);
        constexpr uint32_t callbackAddress = 0x56bb0u;
        const auto currentScreenCallback
            = (static_cast<uint32_t>(masterFirmware.localByte(callbackAddress))
               << 24u)
              | (static_cast<uint32_t>(
                     masterFirmware.localByte(callbackAddress + 1u))
                 << 16u)
              | (static_cast<uint32_t>(
                     masterFirmware.localByte(callbackAddress + 2u))
                 << 8u)
              | static_cast<uint32_t>(
                  masterFirmware.localByte(callbackAddress + 3u));
        modeDisplayObservedCallbackChange
            = modeDisplayObservedCallbackChange
              || currentScreenCallback != activeModeButtonInitialScreenCallback
              || masterFirmware.lcdVideoSnapshot()
                     != activeModeButtonInitialLcd;
        const auto writes = masterFirmware.lcdVideoWriteCount();
        if (writes != modeDisplayLastWriteCount)
        {
            modeDisplayLastWriteCount = writes;
            modeDisplayQuietSamples = 0;
            modeDisplayObservedWrites = true;
        }
        else
        {
            modeDisplayQuietSamples += sampleCount;
            const auto completeWritePass
                = modeDisplayObservedWrites
                  && modeDisplayObservedCallbackChange
                  && writes - modeDisplayWriteStartCount >= 512u
                  && modeDisplayQuietSamples >= 512;
            // A requester can return to a page whose callback is already
            // installed and update fewer than 512 VRAM bytes. Once the serial
            // transaction has retired and VRAM has remained quiet for half a
            // second, retaining the old frame is less faithful than publishing
            // the completed firmware-owned page. This remains a read boundary:
            // the engine neither constructs nor writes LCD pixels.
            const auto serialTransactionRetired
                = activeModeButtonDiagnosticCode < 0
                  && pendingFirmwareModeButton.load(std::memory_order_acquire) < 0;
            const auto quietFallbackSamples = juce::jmax(
                512, juce::roundToInt(currentSampleRate * 0.5));
            const auto completedShortPass
                = serialTransactionRetired
                  && modeDisplayQuietSamples >= quietFallbackSamples;
            if (!instrumentPageActionPending
                && (completeWritePass || completedShortPass))
                panelModeDisplayTransitionActive.store(
                    false, std::memory_order_release);
        }
    }
    if (panelSelectedMode.load(std::memory_order_acquire) == 36
        && instrumentPageReady.load(std::memory_order_acquire))
    {
        // The page target still belongs to the preceding visit until the
        // queued Instrument selection is acknowledged. Reading it during
        // mode entry changes the engine's layer (and Sound) behind the LCD.
        const auto firmwareInstrument = masterFirmware.currentInstrumentEditTarget();
        if (firmwareInstrument.has_value()
            && *firmwareInstrument != getSelectedPerformanceInstrument())
            selectPerformanceInstrument(*firmwareInstrument);
    }
    // Queue +/- after this block's matrix scanning has completed. This keeps a
    // freshly pressed Edit switch ahead of its following step event.
    sendPendingPanelStepButtonsToFirmware();
    sendPendingStoreDestinationStepsToFirmware();
    captureFirmwarePanelPotValues();
}

void WaveEmulationAudioProcessor::sendPendingPerformanceFadersToFirmware()
{
    if (!masterFirmware.isLoaded())
        return;

    const auto recordByte = [this](size_t offset) {
        return masterFirmware.sharedProgramByte(
            firmwareEditPerformanceOffset + static_cast<uint32_t>(offset));
    };
    for (size_t fader = 0; fader < pendingFirmwareFaderValues.size(); ++fader)
    {
        const auto value = pendingFirmwareFaderValues[fader].exchange(
            -1, std::memory_order_acq_rel);
        if (value < 0)
            continue;

        masterFirmware.setPerformanceFaderValue(static_cast<int>(fader),
                                                static_cast<uint8_t>(value));
        const auto destination = static_cast<int>(recordByte(8 + fader * 2) & 0x7fu);
        const auto assignment = static_cast<int>(recordByte(9 + fader * 2) & 0x7fu);
        if (destination < 0 || destination >= 8 || assignment > 116)
            continue;

        const auto layerOffset = static_cast<size_t>(64 + destination * 32);
        const auto storedChannel = static_cast<int>(recordByte(layerOffset + 2) & 0x7fu);
        const auto midiChannel = juce::jlimit(1, 16, storedChannel == 0 ? 1 : storedChannel);
        masterFirmware.pushMidiByte(0, static_cast<uint8_t>(0xb0 | (midiChannel - 1)));
        masterFirmware.pushMidiByte(0, static_cast<uint8_t>(assignment + 1));
        masterFirmware.pushMidiByte(0, static_cast<uint8_t>(juce::jlimit(0, 127, value)));
    }
}

void WaveEmulationAudioProcessor::sendPendingInstrumentFadersToFirmware()
{
    if (!masterFirmware.isLoaded())
        return;
    const auto firmwarePage = masterFirmware.currentInstrumentEditPage();
    if (firmwarePage.has_value())
        instrumentEditPage.store(*firmwarePage, std::memory_order_release);
    if (!firmwarePage.has_value()
        && panelSelectedMode.load(std::memory_order_acquire) == 36
        && panelSelectedEdit.load(std::memory_order_acquire) < 0
        && (panelModeDisplayTransitionActive.load(std::memory_order_acquire)
            || !instrumentPageReady.load(std::memory_order_acquire)))
        return; // Retain the edge until the native page has finished opening.
    if (panelSelectedMode.load(std::memory_order_acquire) != 36
        || panelSelectedEdit.load(std::memory_order_acquire) >= 0
        || !firmwarePage.has_value() || *firmwarePage > 1)
    {
        for (auto& edit : pendingInstrumentFaderEdits)
            edit.store(-1, std::memory_order_release);
        return;
    }

    static constexpr std::array<uint32_t, 8> pageOneRecordOffsets {
        4u, 5u, 7u, 12u, 9u, 10u, 2u, 3u // firmware table 0x14BCA
    };
    static constexpr std::array<uint32_t, 8> pageTwoRecordOffsets {
        4u, 5u, 7u, 21u, 22u, 15u, 16u, 11u
    };
    std::array<bool, 32> committedRecordOffsets {};
    auto committedAnyFader = false;
    auto current = std::atomic_load_explicit(
        &factoryPerformance, std::memory_order_acquire);
    std::shared_ptr<wave::dsp::WaldorfEngine::PerformanceSnapshot> changed;
    const auto firmwarePerformance = masterFirmware.currentPerformanceId();
    const auto firmwareInstrument = masterFirmware.currentInstrumentEditTarget();

    for (size_t fader = 0; fader < pendingInstrumentFaderEdits.size(); ++fader)
    {
        const auto packed = pendingInstrumentFaderEdits[fader].load(
            std::memory_order_acquire);
        if (packed < 0)
            continue;
        const auto program = (packed >> 11) & 0xff;
        const auto instrument = (packed >> 8) & 0x07;
        const auto page = (packed >> 19) & 0x03;
        const auto physical = packed & 0x7f;
        const auto adc = (packed >> 21) & 0xff;
        if (program != currentProgram.load(std::memory_order_acquire))
        {
            pendingInstrumentFaderEdits[fader].store(-1,
                                                     std::memory_order_release);
            continue;
        }
        if (!firmwarePerformance.has_value() || !firmwareInstrument.has_value()
            || *firmwarePerformance != program || *firmwareInstrument != instrument)
        {
            // Instrument selection is a firmware transaction. Preserve a
            // fader edge until the OS and engine agree on its destination;
            // never let it spill into whichever Instrument happened to be
            // selected during the transition.
            continue;
        }
        if (page != *firmwarePage)
        {
            // An edge queued for a page that has since closed cannot be
            // applied to a field occupying the same fader on the new page.
            pendingInstrumentFaderEdits[fader].store(-1, std::memory_order_release);
            continue;
        }

        auto stored = physical;
        if (page == 0)
        {
            switch (fader)
            {
                case 3: stored = (physical * 3 + 63) / 127; break;
                case 4: stored = juce::jlimit(4, 124, physical); break;
                case 5: stored = juce::jlimit(14, 114, physical); break;
                case 6: stored = (physical * 16 + 63) / 127; break;
                case 7: stored = (physical * 3 + 63) / 127; break;
                default: break;
            }
        }
        else
        {
            switch (fader)
            {
                case 3: stored = (physical * 11 + 63) / 127; break;
                case 4:
                {
                    // Match the firmware's displayed TuneTable transition at
                    // each raw ADC value, including odd values that share the
                    // same seven-bit position as their even neighbour.
                    static constexpr std::array<int, 12> thresholds {
                        21, 41, 61, 81, 101, 121,
                        139, 159, 179, 199, 219, 239
                    };
                    stored = static_cast<int>(std::upper_bound(
                        thresholds.begin(), thresholds.end(), adc)
                                              - thresholds.begin());
                    break;
                }
                case 5: stored = (physical * 22 + 63) / 127; break;
                case 6: stored = (physical * 4 + 63) / 127; break;
                case 7: stored = (physical * 3 + 63) / 127; break;
                default: break;
            }
        }
        stored = juce::jlimit(0, 127, stored);
        const auto recordOffset = page == 0 ? pageOneRecordOffsets[fader]
                                            : pageTwoRecordOffsets[fader];
        if (!masterFirmware.writePerformanceInstrumentByte(
                instrument, recordOffset, static_cast<uint8_t>(stored)))
            continue;
        committedRecordOffsets[recordOffset] = true;
        committedAnyFader = true;
        // Page 2 is decoded below from the resulting native Instrument record.
        // Page 1 keeps its existing immediate mirror for compatibility with
        // the firmware's first-page transfer timing.
        if (page != 0)
            continue;
        if (current == nullptr
            || !current->layers[static_cast<size_t>(instrument)].enabled)
            continue;
        const auto& existing = current->layers[static_cast<size_t>(instrument)];
        auto needsSnapshot = false;
        switch (fader)
        {
            case 0:
                needsSnapshot = std::abs(existing.gain
                                         - static_cast<float>(stored) / 127.0f)
                                > 1.0e-6f;
                break;
            case 1:
                needsSnapshot = std::abs(
                    existing.sound.panAmount
                    - juce::jlimit(-1.0f, 1.0f,
                                   static_cast<float>(stored - 64) / 64.0f))
                                > 1.0e-6f;
                break;
            case 2:
                needsSnapshot = std::abs(existing.auxGain
                                         - static_cast<float>(stored) / 127.0f)
                                > 1.0e-6f;
                break;
            case 3: needsSnapshot = existing.audioOutput != stored; break;
            case 4: needsSnapshot = existing.transposeSemitones != stored - 64; break;
            case 5:
                needsSnapshot = existing.detuneCents != static_cast<float>(stored - 64);
                break;
            case 6: needsSnapshot = existing.midiChannel != stored; break;
            case 7: needsSnapshot = existing.source != stored; break;
            default: break;
        }
        if (!needsSnapshot)
            continue;
        if (changed == nullptr)
            changed = std::make_shared<wave::dsp::WaldorfEngine::PerformanceSnapshot>(
                *current);
        auto& layer = changed->layers[static_cast<size_t>(instrument)];
        switch (fader)
        {
            case 0: layer.gain = static_cast<float>(stored) / 127.0f; break;
            case 1:
                layer.sound.panAmount = juce::jlimit(
                    -1.0f, 1.0f, static_cast<float>(stored - 64) / 64.0f);
                break;
            case 2: layer.auxGain = static_cast<float>(stored) / 127.0f; break;
            case 3: layer.audioOutput = stored; break;
            case 4: layer.transposeSemitones = stored - 64; break;
            case 5: layer.detuneCents = static_cast<float>(stored - 64); break;
            case 6: layer.midiChannel = stored; break;
            case 7: layer.source = stored; break;
            default: break;
        }
    }

    if (committedAnyFader && instrumentFaderRecordBaselineValid)
    {
        const auto currentFirmwarePerformance
            = masterFirmware.currentPerformanceId();
        const auto currentFirmwareInstrument
            = masterFirmware.currentInstrumentEditTarget();
        if (currentFirmwarePerformance.has_value()
            && currentFirmwareInstrument.has_value()
            && *currentFirmwarePerformance == instrumentFaderBaselinePerformance
            && *currentFirmwareInstrument == instrumentFaderBaselineInstrument)
            for (size_t offset = 0;
                 offset < instrumentFaderRecordBaseline.size(); ++offset)
                if (!committedRecordOffsets[offset])
                    masterFirmware.writePerformanceInstrumentByte(
                        instrumentFaderBaselineInstrument,
                        static_cast<uint32_t>(offset),
                        instrumentFaderRecordBaseline[offset]);
    }
    instrumentFaderRecordBaselineValid = false;

    if (changed != nullptr)
        std::atomic_store_explicit(
            &factoryPerformance,
            std::static_pointer_cast<const wave::dsp::WaldorfEngine::PerformanceSnapshot>(
                changed),
            std::memory_order_release);
}

void WaveEmulationAudioProcessor::synchronisePerformanceInstrumentsFromFirmware()
    noexcept
{
    if (!masterFirmware.isLoaded())
        return;

    // A cold start has no active bank. The OS's safe INIT records may enable
    // Instruments, but must not repopulate the intentionally empty DSP bank.
    const auto set = currentPerformanceSet();
    if (set == nullptr || !set->isLoaded())
        return;

    const auto current = std::atomic_load_explicit(
        &factoryPerformance, std::memory_order_acquire);
    const auto firmwarePerformance = masterFirmware.currentPerformanceId();
    const auto firmwareInstrument = panelSelectedMode.load(std::memory_order_acquire) == 36
        ? masterFirmware.currentInstrumentEditTarget()
        : masterFirmware.currentPerformanceInstrument();
    const auto performanceRecord = masterFirmware.currentPerformanceRecordOffset();
    if (current == nullptr || !firmwarePerformance.has_value()
        || !firmwareInstrument.has_value() || !performanceRecord.has_value()
        || *firmwarePerformance != currentProgram.load(std::memory_order_acquire)
        || *firmwareInstrument < 0 || *firmwareInstrument >= 8)
        return;

    // Store can replace every Instrument in the destination Performance.
    // Synchronise all eight records so inactive source slots also clear the
    // destination's playback layers and panel indicators.
    std::shared_ptr<wave::dsp::WaldorfEngine::PerformanceSnapshot> changed;
    for (int instrument = 0; instrument < 8; ++instrument)
    {
        constexpr uint32_t instrumentTable = 64u;
        constexpr uint32_t instrumentRecordSize = 32u;
        const auto base = *performanceRecord + instrumentTable
                          + static_cast<uint32_t>(instrument)
                                * instrumentRecordSize;
        const auto byte = [this, base](uint32_t offset) {
            return static_cast<int>(masterFirmware.sharedProgramByte(base + offset)
                                    & 0x7fu);
        };

        const auto source = juce::jlimit(0, 3, byte(3));
        const auto enabled = source != 0 && byte(13) == 0;
        const auto midiChannel = juce::jlimit(0, 16, byte(2));
        const auto gain = static_cast<float>(byte(4)) / 127.0f;
        const auto pan = juce::jlimit(
            -1.0f, 1.0f, static_cast<float>(byte(5) - 64) / 64.0f);
        const auto panMode = juce::jlimit(0, 2, byte(6));
        const auto auxGain = static_cast<float>(byte(7)) / 127.0f;
        const auto audioOutput = juce::jlimit(0, 3, byte(12));
        const auto transpose = byte(9) - 64;
        const auto detune = static_cast<float>(byte(10) - 64);
        const auto keyLow = byte(17);
        const auto keyHigh = byte(18);
        const auto velocityLow = juce::jmax(1, byte(19));
        const auto velocityHigh = juce::jmax(1, byte(20));
        const auto velocityTable = juce::jlimit(0, 11, byte(21));
        const auto tuningTable = juce::jlimit(0, 12, byte(22));
        const auto allocationMode = juce::jlimit(0, 22, byte(15));

        const auto& existing
            = current->layers[static_cast<size_t>(instrument)];
        if (existing.enabled == enabled && existing.source == source
            && existing.midiChannel == midiChannel
            && std::abs(existing.gain - gain) <= 1.0e-6f
            && std::abs(existing.sound.panAmount - pan) <= 1.0e-6f
            && existing.sound.panModulationMode == panMode
            && std::abs(existing.auxGain - auxGain) <= 1.0e-6f
            && existing.audioOutput == audioOutput
            && existing.transposeSemitones == transpose
            && std::abs(existing.detuneCents - detune) <= 1.0e-6f
            && existing.keyLow == keyLow && existing.keyHigh == keyHigh
            && existing.velocityLow == velocityLow
            && existing.velocityHigh == velocityHigh
            && existing.velocityTable == velocityTable
            && existing.tuningTable == tuningTable
            && existing.allocationMode == allocationMode)
            continue;

        if (changed == nullptr)
            changed = std::make_shared<wave::dsp::WaldorfEngine::PerformanceSnapshot>(*current);
        auto& layer = changed->layers[static_cast<size_t>(instrument)];
        if (enabled && !existing.enabled)
        {
            if (const auto record = masterFirmware.performanceInstrumentSoundRecordOffset(instrument))
            {
                std::array<uint8_t, wave::presets::WaveFactorySet::soundSize> sound {};
                for (size_t offset = 0; offset < sound.size(); ++offset)
                    sound[offset] = masterFirmware.sharedProgramByte(
                        *record + static_cast<uint32_t>(offset));
                layer.sound = decodeFactorySound(sound);
            }
        }
        layer.enabled = enabled;
        layer.source = source;
        layer.midiChannel = midiChannel;
        layer.gain = gain;
        layer.sound.panAmount = pan;
        layer.sound.panModulationMode = panMode;
        layer.auxGain = auxGain;
        layer.audioOutput = audioOutput;
        layer.transposeSemitones = transpose;
        layer.detuneCents = detune;
        layer.keyLow = keyLow;
        layer.keyHigh = keyHigh;
        layer.velocityLow = velocityLow;
        layer.velocityHigh = velocityHigh;
        layer.velocityTable = velocityTable;
        layer.tuningTable = tuningTable;
        layer.allocationMode = allocationMode;
    }
    if (changed != nullptr)
        std::atomic_store_explicit(
            &factoryPerformance,
            std::static_pointer_cast<const wave::dsp::WaldorfEngine::PerformanceSnapshot>(
                changed),
            std::memory_order_release);
}

void WaveEmulationAudioProcessor::sendPendingGlideFadersToFirmware()
{
    if (panelSelectedEdit.load(std::memory_order_acquire) != 11)
    {
        for (auto& edit : pendingGlideFaderEdits)
            edit.store(-1, std::memory_order_release);
        return;
    }
    if (panelModeDisplayTransitionActive.load(std::memory_order_acquire)
        || !masterFirmware.panelLed(75))
        return;
    const auto program = masterFirmware.currentPerformanceId();
    const auto instrument = masterFirmware.currentPerformanceInstrument();
    auto changed = false;
    for (const auto fader : { 0, 1, 4, 5 })
    {
        const auto packed = pendingGlideFaderEdits[static_cast<size_t>(fader)].load(
            std::memory_order_acquire);
        if (packed < 0)
            continue;
        if (((packed >> 11) & 0xff) != currentProgram.load(std::memory_order_acquire))
        {
            pendingGlideFaderEdits[static_cast<size_t>(fader)].store(-1,
                                                                  std::memory_order_release);
            continue;
        }
        if (!program.has_value() || !instrument.has_value()
            || *program != ((packed >> 11) & 0xff)
            || *instrument != ((packed >> 8) & 7))
            continue;
        pendingGlideFaderEdits[static_cast<size_t>(fader)].store(-1,
                                                              std::memory_order_release);
        const auto physical = packed & 0x7f;
        const auto offset = fader == 0 ? 233u : fader == 1 ? 235u
                                  : fader == 4 ? 236u : 237u;
        const auto value = fader == 0 ? 1 + physical * 6 / 128
                         : fader == 1 ? physical * 2 / 128
                         : fader == 4 ? physical * 39 / 128 : physical;
        // As with Instrument Edit, model the absent ASIC's contextual
        // analogue-to-record transfer on the audio thread. The OS owns the
        // page, Sound destination, display, and subsequent voice commands.
        changed |= masterFirmware.writeCurrentSoundRecordByte(
            offset, static_cast<uint8_t>(value));
    }
    if (changed)
        masterFirmware.refreshCurrentScreenFromFirmware();
}

void WaveEmulationAudioProcessor::sendPendingPanelEncodersToFirmware()
{
    for (size_t encoder = 0; encoder < pendingPanelEncoderSteps.size(); ++encoder)
    {
        auto remaining = pendingPanelEncoderSteps[encoder].exchange(
            0, std::memory_order_acq_rel);
        const auto input = wave::panel::encoderSerialCodes[encoder];
        while (remaining != 0)
        {
            const auto delta = juce::jlimit(-63, 63, remaining);
            masterFirmware.pushPanelEvent(
                0x90u, static_cast<uint8_t>(input),
                static_cast<uint8_t>(juce::jlimit(1, 127, 64 + delta)));
            remaining -= delta;
        }
    }
}

void WaveEmulationAudioProcessor::sendPendingPanelGlideSwitchesToFirmware()
{
    constexpr auto glideDiagnosticCode = 6;
    constexpr auto maximumGlideSwitchRetries = 7;
    constexpr auto acknowledgementWaitBlocks = 16;

    queuedFirmwareGlideSwitchClicks += pendingFirmwareGlideSwitchClicks.exchange(
        0, std::memory_order_acq_rel);

    if (firmwareGlideSwitchTransactionActive)
    {
        const auto enabled = static_cast<int>(
            masterFirmware.currentSoundRecordByte(238u) & 0x01u);
        if (enabled != firmwareGlideSwitchBaseline)
        {
            if (!firmwareGlideSwitchReleaseSent)
            {
                masterFirmware.pushPanelEvent(0x80u, glideDiagnosticCode, 0u);
                firmwareGlideSwitchReleaseSent = true;
                firmwareGlideSwitchWaitBlocks = 0;
                return;
            }
            if (masterFirmware.panelEventPending(
                    0x80u, glideDiagnosticCode, 0u))
                return;

            masterFirmware.releasePanelEventLatch(glideDiagnosticCode);
            firmwareGlideSwitchTransactionActive = false;
            firmwareGlideSwitchReleaseSent = false;
            firmwareGlideSwitchBaseline = -1;
            firmwareGlideSwitchWaitBlocks = 0;
            firmwareGlideSwitchRetryCount = 0;
            queuedFirmwareGlideSwitchClicks
                = juce::jmax(0, queuedFirmwareGlideSwitchClicks - 1);
            return;
        }

        if (masterFirmware.panelEventPending(
                0x80u, glideDiagnosticCode,
                firmwareGlideSwitchReleaseSent ? 0u : 1u))
            return;

        if (!firmwareGlideSwitchReleaseSent
            && ++firmwareGlideSwitchWaitBlocks < acknowledgementWaitBlocks)
            return;

        if (!firmwareGlideSwitchReleaseSent)
        {
            // The current page can consume an edge while its action table is
            // being replaced. Complete that physical attempt before retrying.
            masterFirmware.pushPanelEvent(0x80u, glideDiagnosticCode, 0u);
            firmwareGlideSwitchReleaseSent = true;
            firmwareGlideSwitchWaitBlocks = 0;
            return;
        }

        masterFirmware.releasePanelEventLatch(glideDiagnosticCode);
        if (firmwareGlideSwitchRetryCount++ < maximumGlideSwitchRetries)
        {
            firmwareGlideSwitchReleaseSent = false;
            firmwareGlideSwitchBaseline = enabled;
            firmwareGlideSwitchWaitBlocks = 0;
            pendingPanelGlideSwitchFeedback.store(true,
                                                  std::memory_order_release);
            masterFirmware.pushPanelEvent(0x80u, glideDiagnosticCode, 1u);
            return;
        }

        firmwareGlideSwitchTransactionActive = false;
        firmwareGlideSwitchReleaseSent = false;
        firmwareGlideSwitchBaseline = -1;
        firmwareGlideSwitchWaitBlocks = 0;
        firmwareGlideSwitchRetryCount = 0;
        queuedFirmwareGlideSwitchClicks
            = juce::jmax(0, queuedFirmwareGlideSwitchClicks - 1);
        return;
    }

    if (queuedFirmwareGlideSwitchClicks <= 0
        || activeModeButtonDiagnosticCode >= 0
        || pendingFirmwareModeButton.load(std::memory_order_acquire) >= 0)
        return;

    // A mouse tap may begin and end entirely between two host callbacks. Feed
    // one complete physical serial transaction to OS 1.700 and retain it
    // until the firmware's own Sound record acknowledges the toggle.
    masterFirmware.discardPendingPanelButtonEvents(glideDiagnosticCode);
    masterFirmware.releasePanelEventLatch(glideDiagnosticCode);
    firmwareGlideSwitchBaseline = static_cast<int>(
        masterFirmware.currentSoundRecordByte(238u) & 0x01u);
    firmwareGlideSwitchTransactionActive = true;
    firmwareGlideSwitchReleaseSent = false;
    firmwareGlideSwitchWaitBlocks = 0;
    firmwareGlideSwitchRetryCount = 0;
    pendingPanelGlideSwitchFeedback.store(true, std::memory_order_release);
    masterFirmware.pushPanelEvent(0x80u, glideDiagnosticCode, 1u);
}

void WaveEmulationAudioProcessor::sendPendingPanelStepButtonsToFirmware()
{
    queuedPanelStepButtonEvents += pendingPanelStepButtonEvents.exchange(
        0, std::memory_order_acq_rel);
    if (activePanelStepButtonDiagnosticCode >= 0)
    {
        if (masterFirmware.releasePanelEventLatch(
                activePanelStepButtonDiagnosticCode))
        {
            activePanelStepButtonDiagnosticCode = -1;
            activePanelStepButtonWaitBlocks = 0;
        }
        else if (!masterFirmware.panelEventPending(
                     0x80u,
                     static_cast<uint8_t>(activePanelStepButtonDiagnosticCode), 1u)
                 && ++activePanelStepButtonWaitBlocks >= 16)
        {
            // A busy page may consume and reject the edge before installing
            // its repeat latch. Retry instead of permanently blocking every
            // later queued click behind that unacknowledged event.
            masterFirmware.pushPanelEvent(
                0x80u,
                static_cast<uint8_t>(activePanelStepButtonDiagnosticCode), 1u);
            activePanelStepButtonWaitBlocks = 0;
        }
        return;
    }

    if (queuedPanelStepButtonEvents == 0)
        return;
    if (masterFirmware.stepDiskMenuWithFirmware(queuedPanelStepButtonEvents > 0))
    {
        queuedPanelStepButtonEvents += queuedPanelStepButtonEvents < 0 ? 1 : -1;
        return;
    }
    activePanelStepButtonDiagnosticCode
        = queuedPanelStepButtonEvents < 0 ? 69 : 72;
    activePanelStepButtonWaitBlocks = 0;
    queuedPanelStepButtonEvents += queuedPanelStepButtonEvents < 0 ? 1 : -1;
    masterFirmware.pushPanelEvent(
        0x80u, static_cast<uint8_t>(activePanelStepButtonDiagnosticCode), 1u);
}

void WaveEmulationAudioProcessor::sendPendingStoreDestinationStepsToFirmware()
{
    queuedStoreDestinationStepEvents
        += pendingStoreDestinationStepEvents.exchange(0,
                                                       std::memory_order_acq_rel);
    if (queuedStoreDestinationStepEvents == 0)
        return;
    masterFirmware.stepStoreDestinationWithFirmware(
        queuedStoreDestinationStepEvents > 0);
    queuedStoreDestinationStepEvents
        += queuedStoreDestinationStepEvents < 0 ? 1 : -1;
}

void WaveEmulationAudioProcessor::setPanelPotValue(const juce::String& parameterId,
                                                    float normalised) noexcept
{
    panelWavetableDataDialActive.store(false, std::memory_order_release);
    if (parameterId == wave::parameters::glideRate)
    {
        // The keyboard controller's Glide Rate pot does not enter the upper
        // panel ADC multiplexer. The unavailable keyboard ASIC transfers its
        // 7-bit value to byte 234 of the firmware-selected Sound record. Keep
        // that hardware boundary here: the UI supplies only the physical pot
        // position, while firmware record feedback remains the sole source of
        // APVTS, DSP and LCD state.
        pendingPanelGlideRate.store(
            juce::roundToInt(juce::jlimit(0.0f, 1.0f, normalised) * 127.0f),
            std::memory_order_release);
        return;
    }
    for (const auto& pot : wave::panel::visiblePots)
    {
        if (parameterId == pot.parameterId)
        {
            // The Wave panel pots are wired with clockwise travel lowering
            // the ADC voltage. OS 1.700 owns the conversion, hysteresis, and
            // Knob Mode calculation after this physical input boundary.
            masterFirmware.setPanelAnalog(
                pot.adcChannel,
                1.0f - juce::jlimit(0.0f, 1.0f, normalised));
            return;
        }
    }
}

std::optional<float> WaveEmulationAudioProcessor::getPanelPotValue(
    const juce::String& parameterId) const noexcept
{
    for (const auto& pot : wave::panel::visiblePots)
        if (parameterId == pot.parameterId)
        {
            // The front-panel potentiometers are electrically reversed: the
            // clockwise artwork position is one minus the ADC voltage.
            return 1.0f
                   - static_cast<float>(masterFirmware.panelAnalogByte(
                         pot.adcChannel))
                         / 255.0f;
        }
    return std::nullopt;
}

void WaveEmulationAudioProcessor::captureFirmwarePanelPotValues()
{
    if (resetPanelPotSoundRecordOffset.exchange(
            false, std::memory_order_acq_rel))
        lastPanelPotSoundRecordOffset.reset();
    const auto recordOffset = masterFirmware.currentSoundRecordOffset();
    const auto performance = std::atomic_load_explicit(
        &factoryPerformance, std::memory_order_acquire);
    const auto selectionVersion
        = performanceInstrumentSelectionVersion.load(std::memory_order_acquire);
    const auto firmwarePerformance = masterFirmware.currentPerformanceId();
    const auto firmwareInstrument = masterFirmware.currentPerformanceInstrument();
    const auto contextMatches
        = recordOffset.has_value() && performance != nullptr
          && (selectionVersion & 1u) == 0u
          && firmwarePerformance.has_value() && firmwareInstrument.has_value()
          && *firmwarePerformance == currentProgram.load(std::memory_order_acquire)
          && *firmwareInstrument == performance->editableLayer;
    if (!contextMatches)
    {
        firmwareSoundFeedbackSuspended.store(true, std::memory_order_release);
        return;
    }

    if (lastPanelPotSoundRecordOffset != recordOffset)
    {
        // Selecting a different Sound can leave message-thread feedback from
        // the previous edit record queued. Discard it and allow one complete
        // audio block for the firmware cache record to settle before exposing
        // the new Sound to APVTS or the engine-facing panel mirror.
        lastPanelPotSoundRecordOffset = recordOffset;
        lastFirmwarePanelPotRecordBytes.fill(-1);
        suspendFirmwareSoundFeedback();
        resetPanelPotSoundRecordOffset.store(false,
                                             std::memory_order_release);
        return;
    }

    if (firmwareSoundFeedbackSuspended.exchange(
            false, std::memory_order_acq_rel))
        lastFirmwarePanelPotRecordBytes.fill(-1);

    auto changed = false;
    for (size_t index = 0; index < panelPotRecordBindings.size(); ++index)
    {
        const auto& binding = panelPotRecordBindings[index];
        const auto storedValue = masterFirmware.currentSoundRecordByte(
            binding.soundOffset);
        if (lastFirmwarePanelPotRecordBytes[index]
            == static_cast<int>(storedValue))
            continue;
        lastFirmwarePanelPotRecordBytes[index] = static_cast<int>(storedValue);
        pendingFirmwarePanelPotValues[index].store(
            decodePanelPotValue(binding, storedValue),
            std::memory_order_release);
        changed = true;
    }
    if (changed)
        triggerAsyncUpdate();
}

void WaveEmulationAudioProcessor::suspendFirmwareSoundFeedback() noexcept
{
    firmwareSoundFeedbackSuspended.store(true, std::memory_order_release);
    resetPanelPotSoundRecordOffset.store(true, std::memory_order_release);
    resetHostPanelPotTracking.store(true, std::memory_order_release);
    for (auto& value : pendingFirmwarePanelPotValues)
        value.store(std::numeric_limits<float>::quiet_NaN(),
                    std::memory_order_release);
    for (auto& value : pendingFirmwareModulationSources)
        value.store(-1, std::memory_order_release);
    for (auto& value : pendingFirmwareModulationControls)
        value.store(-1, std::memory_order_release);
    for (auto& value : pendingFirmwareModulationAmounts)
        value.store(-128, std::memory_order_release);
    for (auto& value : pendingFirmwareGlideValues)
        value.store(-1000, std::memory_order_release);
    pendingFirmwareOscillatorLink.store(-1, std::memory_order_release);
    confirmedPanelGlideRate.store(-1, std::memory_order_release);
    confirmedPanelGlideEnabled.store(-1, std::memory_order_release);
}

void WaveEmulationAudioProcessor::armPanelPotProgramRecallGuard() noexcept
{
    for (size_t index = 0; index < wave::panel::visiblePots.size(); ++index)
    {
        const auto* raw = parameters.getRawParameterValue(
            wave::panel::visiblePots[index].parameterId);
        programRecallPanelPotEchoValues[index].store(
            raw != nullptr ? raw->load(std::memory_order_acquire)
                           : std::numeric_limits<float>::quiet_NaN(),
            std::memory_order_release);
    }
    // AAX can deliver its cached parameter acknowledgement several callbacks
    // after the program notification. Only the exact preceding values are
    // suppressed; new automation values and real panel motion remain active.
    programRecallPanelPotEchoGuardBlocks.store(256,
                                               std::memory_order_release);
}

void WaveEmulationAudioProcessor::synchronisePanelPotInputsToParameters() noexcept
{
    const auto selectedLfo = juce::jlimit(
        0, 1, panelSelectedLfo.load(std::memory_order_acquire));
    for (const auto& pot : wave::panel::visiblePots)
    {
        const auto id = juce::String(pot.parameterId);
        const auto belongsToLfo0
            = id == wave::parameters::lfoRate[0]
              || id == wave::parameters::modulationAmount[
                           wave::parameters::lfo1RateMod]
              || id == wave::parameters::modulationAmount[
                           wave::parameters::lfo1LevelMod];
        const auto belongsToLfo1
            = id == wave::parameters::lfoRate[1]
              || id == wave::parameters::modulationAmount[
                           wave::parameters::lfo2RateMod]
              || id == wave::parameters::modulationAmount[
                           wave::parameters::lfo2LevelMod];
        if ((belongsToLfo0 && selectedLfo != 0)
            || (belongsToLfo1 && selectedLfo != 1))
            continue;
        if (const auto* parameter = parameters.getParameter(pot.parameterId))
            masterFirmware.setPanelAnalog(pot.adcChannel, 1.0f - parameter->getValue());
    }
}

bool WaveEmulationAudioProcessor::setPanelButton(int buttonId, bool pressed) noexcept
{
    const auto diagnosticCode
        = wave::panel::diagnosticCodeForMatrixIndex(buttonId);
    if (diagnosticCode == 3 || diagnosticCode == 4)
    {
        auto& down = panelButtonDown[static_cast<size_t>(buttonId)];
        if (down.load(std::memory_order_acquire) == pressed)
            return true;
        const auto event = keyboardButtonEventFifo.write(1);
        if (event.blockSize1 == 0)
            return false;
        keyboardButtonEvents[static_cast<size_t>(event.startIndex1)]
            = static_cast<uint8_t>(((diagnosticCode - 3) << 1) | (pressed ? 1 : 0));
        down.store(pressed, std::memory_order_release);
        return true;
    }
    if (pressed && (diagnosticCode == 12 || diagnosticCode == 73))
    {
        pendingKeyboardOctaveRestore.store(2, std::memory_order_release);
        keyboardOctaveRestoreWaitBlocks.store(0, std::memory_order_release);
    }
    const auto storeCancel = diagnosticCode == 71
        && (storeMenuActive.load(std::memory_order_acquire)
            || storeExitModePending.load(std::memory_order_acquire));
    const auto requesterActive
        = firmwareRequesterActive.load(std::memory_order_acquire);
    const auto storeDestinationStep
        = requesterActive && storeMenuActive.load(std::memory_order_acquire)
          && (diagnosticCode == 69 || diagnosticCode == 72);
    const auto nameCursorStep
        = requesterActive
          && (storeMenuActive.load(std::memory_order_acquire)
              || firmwareDiskNameEditorActive.load(std::memory_order_acquire))
          && (diagnosticCode == 21 || diagnosticCode == 23);
    if (pressed && requesterActive && diagnosticCode != 70
        && diagnosticCode != 71 && !storeDestinationStep
        && !nameCursorStep)
    {
        // A genuine OS requester owns the front panel until CANCEL or OK. The
        // Store requester additionally accepts -/+ for its destination, and
        // Store/DOS name editors accept Page arrows. Reject other contacts before
        // any local mode/edit/LED state is changed; the UI receives this
        // result as engine feedback and never infers modal state from pixels.
        if (buttonId >= 0 && buttonId < static_cast<int>(panelButtonDown.size()))
            panelButtonDown[static_cast<size_t>(buttonId)].store(
                false, std::memory_order_release);
        masterFirmware.setPanelButton(buttonId, false);
        return false;
    }
    // An open requester owns the page (handled above). Once a Store save has
    // closed its requester, the OS accepts another operating-mode button as
    // an exit from Store. Keep our mode and Store lamp on that same path;
    // previously the OS changed pages but our exclusive-mode guard left the
    // Store lamp lit and could expose the old requester over the new page.
    constexpr std::array exclusivePageCodes { 38, 33, 35, 34, 32,
                                              37, 36, 39, 58, 57 };
    const auto currentMode = panelSelectedMode.load(std::memory_order_acquire);
    const auto requestedOperatingPage
        = std::find(exclusivePageCodes.begin(), exclusivePageCodes.end(),
                    diagnosticCode) != exclusivePageCodes.end();
    const auto completedStoreCanExitByMode
        = currentMode == 57 && !requesterActive
          && !storeExitModePending.load(std::memory_order_acquire)
          && diagnosticCode != 57 && diagnosticCode != 35
          && requestedOperatingPage;
    const auto exclusiveModeOwnsSelection
        = (currentMode == 34 || currentMode == 32
           || currentMode == 58 || currentMode == 57)
          && diagnosticCode != currentMode && requestedOperatingPage
          && !completedStoreCanExitByMode;
    if (pressed && exclusiveModeOwnsSelection)
    {
        if (buttonId >= 0 && buttonId < static_cast<int>(panelButtonDown.size()))
            panelButtonDown[static_cast<size_t>(buttonId)].store(
                false, std::memory_order_release);
        masterFirmware.setPanelButton(buttonId, false);
        return false;
    }
    if (pressed && completedStoreCanExitByMode)
    {
        // The saved record's page is visible, but Store still owns the native
        // Instrument chooser. Retire it before delivering the new mode and
        // its Instrument softkey, which would otherwise open another save.
        storeExitMode.store(diagnosticCode, std::memory_order_release);
        storeExitModePending.store(true, std::memory_order_release);
        pendingPanelCancel.store(true, std::memory_order_release);
    }
    if (storeDestinationStep)
    {
        // Store owns this destination field. Queue the genuine serial action
        // on the firmware thread and do not let the Performance-page shortcut
        // change the currently playing program.
        panelWavetableDataDialActive.store(false, std::memory_order_release);
        if (buttonId >= 0 && buttonId < static_cast<int>(panelButtonDown.size()))
            panelButtonDown[static_cast<size_t>(buttonId)].store(
                pressed, std::memory_order_release);
        masterFirmware.setPanelButton(buttonId, false);
        if (pressed)
            pendingStoreDestinationStepEvents.fetch_add(
                diagnosticCode == 69 ? -1 : 1, std::memory_order_release);
        return true;
    }
    if (pressed && (diagnosticCode == 70 || diagnosticCode == 71))
    {
        if (diagnosticCode == 70 && requesterActive
            && storeMenuActive.load(std::memory_order_acquire))
            storeSaveCompletionPending.store(true, std::memory_order_release);
        else if (diagnosticCode == 71)
            storeSaveCompletionPending.store(false, std::memory_order_release);

        // Once a display softkey has opened a requester, its transaction is
        // complete. CANCEL/OK must retire it so the next Zoning softkey can
        // create a fresh edge after the requester closes.
        cancelFirmwareSoftButtonEvents.store(true, std::memory_order_release);
        firmwareRequesterActive.store(false, std::memory_order_release);

        // CANCEL leaves the Disk/Import workspace, and OK completes a pending
        // SET import. Keep either firmware exit in the engine-side panel state
        // so the mutually-exclusive mode lamps cannot retain the Option/Disk
        // selection after the OS has returned to Performance.
        const auto diskWorkspaceActive
            = diskMenuActive.load(std::memory_order_acquire);
        const auto skippingDiskCalibration
            = diagnosticCode == 71
              && diskSetImportConfirmationPending.load(std::memory_order_acquire)
              && firmwareDiskCalibrationRequesterActive.load(std::memory_order_acquire);
        if (skippingDiskCalibration)
            pendingDiskSetBytes.store(0, std::memory_order_release);
        const auto leavingDisk
            = diagnosticCode == 71 && !skippingDiskCalibration
              && diskMenuActive.exchange(false, std::memory_order_acq_rel);
        const auto leavingStore
            = diagnosticCode == 71
              && storeMenuActive.load(std::memory_order_acquire);
        const auto selectedMode
            = panelSelectedMode.load(std::memory_order_acquire);
        const auto diskImportAwaitingConfirmation
            = diskSetImportConfirmationPending.load(std::memory_order_acquire);
        const auto diskImportWorkspace
            = !diskWorkspaceActive && (selectedMode == 34 || selectedMode == 32);
        const auto completingDiskImport
            = diagnosticCode == 70
              && diskImportAwaitingConfirmation && diskImportWorkspace;
        if (completingDiskImport)
        {
            diskSetImportConfirmationPending.store(
                false, std::memory_order_release);
            diskMenuActive.store(false, std::memory_order_release);
            // Wait for the matching OK release before sending the Performance
            // contact. Dispatching it while OK is physically held leaves the
            // requester owning the serial action table and forces retries.
            returnToPerformanceAfterDiskImport.store(
                true, std::memory_order_release);
        }
        else if (diagnosticCode == 71 && !skippingDiskCalibration)
        {
            if (diskImportAwaitingConfirmation
                && (diskImportWorkspace || diskWorkspaceActive))
            {
                diskSetImportConfirmationPending.store(
                    false, std::memory_order_release);
                pendingDiskSetActivation.store(false, std::memory_order_release);
            }
            returnToPerformanceAfterDiskImport.store(
                false, std::memory_order_release);
        }
        if ((diagnosticCode == 71 && !skippingDiskCalibration
             && (leavingDisk
                 || selectedMode == 34
                 || selectedMode == 32))
            || leavingStore || completingDiskImport)
        {
            panelSelectedMode.store(39, std::memory_order_release);
            panelSelectedEdit.store(-1, std::memory_order_release);
        }
        if (leavingStore)
        {
            // Store's Manager callbacks need a native Cancel release before
            // returning to Performance. The audio-thread transaction supplies
            // both edges even when the whole click falls between callbacks.
            storeExitMode.store(39, std::memory_order_release);
            storeExitModePending.store(
                true, std::memory_order_release);
            pendingPanelCancel.store(true, std::memory_order_release);
        }

        if (leavingStore)
            storeMenuActive.store(false, std::memory_order_release);
    }
    const auto schedulePerformanceAfterDiskOkRelease
        = !pressed && diagnosticCode == 70
          && returnToPerformanceAfterDiskImport.exchange(
              false, std::memory_order_acq_rel);
    if (pressed && diagnosticCode != 69 && diagnosticCode != 72)
        panelWavetableDataDialActive.store(false, std::memory_order_release);
    auto firmwareButtonEventHandled = storeCancel;
    if (buttonId >= 0 && buttonId < static_cast<int>(panelButtonDown.size()))
        panelButtonDown[static_cast<size_t>(buttonId)].store(
            pressed, std::memory_order_release);
    if (diagnosticCode == 6)
    {
        // The audio-thread transaction owns both edges. Queue on mouse-down;
        // mouse-up only restores the visible physical switch state.
        firmwareButtonEventHandled = true;
        if (pressed)
            pendingFirmwareGlideSwitchClicks.fetch_add(
                1, std::memory_order_release);
    }

    if (!pressed && (buttonId == 70 || buttonId == 71)
        && filterCalibrationServiceActive.load(std::memory_order_acquire))
        filterCalibrationServiceExitPending.store(true,
                                                  std::memory_order_release);

    if (pressed)
    {
        constexpr auto operatingModeCodes = exclusivePageCodes;
        if (!exclusiveModeOwnsSelection && diagnosticCode == 58) // Disk
        {
            storeMenuActive.store(false, std::memory_order_release);
            diskMenuActive.store(true, std::memory_order_release);
            panelSelectedMode.store(58, std::memory_order_release);
            panelSelectedEdit.store(-1, std::memory_order_release);
        }
        else if (!exclusiveModeOwnsSelection && diagnosticCode == 57) // Store
        {
            diskMenuActive.store(false, std::memory_order_release);
            storeMenuActive.store(true, std::memory_order_release);
            if (currentMode != 57)
                storeReturnMode.store(currentMode, std::memory_order_release);
            completedStoreMode.store(-1, std::memory_order_release);
            storeSaveCompletionPending.store(false, std::memory_order_release);
            panelSelectedMode.store(57, std::memory_order_release);
            panelSelectedEdit.store(-1, std::memory_order_release);
        }
        else if (storeMenuActive.load(std::memory_order_acquire)
                 && keyboardControllerShiftDown.load(std::memory_order_acquire)
                 && diagnosticCode == 29) // Display button 7
        {
            filterCalibrationServiceActive.store(true,
                                                 std::memory_order_release);
            filterCalibrationServiceExitPending.store(false,
                                                      std::memory_order_release);
            pendingFilterCalibrationServiceSeed.store(true,
                                                      std::memory_order_release);
            storeMenuActive.store(false, std::memory_order_release);
        }
        else if (!exclusiveModeOwnsSelection
                 && std::find(operatingModeCodes.begin(),
                              operatingModeCodes.end(), diagnosticCode)
                        != operatingModeCodes.end())
        {
            diskMenuActive.store(false, std::memory_order_release);
            storeMenuActive.store(false, std::memory_order_release);
            filterCalibrationServiceActive.store(false,
                                                 std::memory_order_release);
            filterCalibrationServiceExitPending.store(false,
                                                      std::memory_order_release);
            if (diagnosticCode == 39)
                cancelPanelStepButtonEvents.store(true,
                                                  std::memory_order_release);
        }

        const auto cycleParameter = [this](const char* parameterId, int count) {
            if (auto* parameter = parameters.getParameter(parameterId))
            {
                const auto plain = juce::roundToInt(
                    parameter->convertFrom0to1(parameter->getValue()));
                parameter->setValueNotifyingHost(parameter->convertTo0to1(
                    static_cast<float>((plain + 1) % count)));
            }
        };

        if (diagnosticCode == 0 || diagnosticCode == 1)
        {
            const auto oscillator = static_cast<size_t>(diagnosticCode);
            const auto previous = panelOscillatorOctaves[oscillator].load(
                std::memory_order_acquire);
            panelOscillatorOctaves[oscillator].store(
                previous >= 2 ? -2 : previous + 1,
                std::memory_order_release);
        }
        else if (diagnosticCode == 2)
        {
            panelSelectedLfo.store(
                1 - panelSelectedLfo.load(std::memory_order_acquire),
                std::memory_order_release);
        }
        else if (diagnosticCode == 5)
        {
            const auto selected = juce::jlimit(
                0, 1, panelSelectedLfo.load(std::memory_order_acquire));
            cycleParameter(wave::parameters::lfoShape[static_cast<size_t>(selected)], 6);
        }
        else if (diagnosticCode == 7)
        {
            const auto selected = juce::jlimit(
                0, 1, panelSelectedLfo.load(std::memory_order_acquire));
            cycleParameter(wave::parameters::lfoSync[static_cast<size_t>(selected)], 3);
        }
        else if (diagnosticCode == 14)
        {
            panelWaveEnvelopePage.store(
                (panelWaveEnvelopePage.load(std::memory_order_acquire) + 1) % 3,
                std::memory_order_release);
        }
        else if (diagnosticCode == 43)
        {
            cycleParameter(wave::parameters::filterMode, 4);
        }
        else if (diagnosticCode == 75)
        {
            panelFilterSelection.store(
                1 - panelFilterSelection.load(std::memory_order_acquire),
                std::memory_order_release);
        }

        const auto selectedPanelMode
            = panelSelectedMode.load(std::memory_order_acquire);
        const auto performanceMode
            = selectedPanelMode == 39
              && panelSelectedEdit.load(std::memory_order_acquire) < 0;
        const auto set = currentPerformanceSet();
        const auto hasPerformanceSet = set != nullptr && set->isLoaded();
        constexpr std::array instrumentCodes { 22, 25, 26, 27,
                                                79, 28, 29, 30 };
        const auto instrument = std::find(instrumentCodes.begin(),
                                          instrumentCodes.end(),
                                          diagnosticCode);
        const auto instrumentControlsActive
            = performanceMode || selectedPanelMode == 36;
        if (instrumentControlsActive && hasPerformanceSet && diagnosticCode == 24)
            togglePerformanceMuteMode();
        else if (instrumentControlsActive && hasPerformanceSet && diagnosticCode == 78)
            togglePerformanceSoloMode();
        else if (selectedPanelMode == 36 && hasPerformanceSet
                 && instrument != instrumentCodes.end())
        {
            firmwareButtonEventHandled = true;
            if (instrumentZoningPageActive.load(std::memory_order_acquire))
            {
                // Layer, Split, Key Window and the velocity macros are genuine
                // display actions on this page. Give the serial transaction
                // ownership of the complete press/release pair and stop any
                // earlier Instrument retry before it can reopen the requester.
                cancelInstrumentPanelEvents.store(true,
                                                  std::memory_order_release);
                pendingFirmwareSoftButton.store(diagnosticCode,
                                                std::memory_order_release);
            }
            else
            {
                const auto selectedInstrument = static_cast<int>(
                    std::distance(instrumentCodes.begin(), instrument));
                if (isPerformanceInstrumentActive(selectedInstrument))
                {
                    if (instrumentPageReady.load(std::memory_order_acquire))
                    {
                        cancelInstrumentPageSelection.store(
                            true, std::memory_order_release);
                        cancelActiveInstrumentSelection.store(
                            true, std::memory_order_release);
                        pendingFirmwareInstrument.store(
                            selectedInstrument, std::memory_order_release);
                    }
                    else
                    {
                        pendingInstrumentPageSelection.store(
                            selectedInstrument, std::memory_order_release);
                    }
                }
                else
                {
                    // An OFF Instrument must still reach the OS: its Source
                    // fader is how the Wave activates it. The engine will
                    // mirror the native record after Source is changed.
                    cancelInstrumentPageSelection.store(
                        true, std::memory_order_release);
                    cancelActiveInstrumentSelection.store(
                        true, std::memory_order_release);
                    pendingFirmwareInstrument.store(
                        selectedInstrument, std::memory_order_release);
                }
            }
        }
        else if (!performanceMode && selectedPanelMode != 36
                 && instrument != instrumentCodes.end())
        {
            // Outside Performance and Instrument Edit these eight switches
            // are display soft keys. Send an explicit firmware press/release
            // pair so action bits cannot remain latched and auto-repeat menus.
            firmwareButtonEventHandled = true;
            pendingFirmwareSoftButton.store(diagnosticCode,
                                            std::memory_order_release);
        }
        else if (performanceMode && hasPerformanceSet)
        {
            if (instrument != instrumentCodes.end())
            {
                const auto selectedInstrument = static_cast<int>(
                    std::distance(instrumentCodes.begin(), instrument));
                if (pressPerformanceInstrumentButton(selectedInstrument))
                {
                    // Own the complete serial action on the firmware/audio
                    // thread so the target Instrument's private edit record is
                    // installed before OS 1.700 consumes the switch.
                    firmwareButtonEventHandled = true;
                    cancelActiveInstrumentSelection.store(
                        true, std::memory_order_release);
                    pendingFirmwareInstrument.store(
                        selectedInstrument, std::memory_order_release);
                }
            }

            if (diagnosticCode == 44)
            {
                const auto selected = currentProgram.load(std::memory_order_acquire);
                const auto destination = (1 - selected / 128) * 128
                                         + selected % 128;
                currentProgram.store(destination, std::memory_order_release);
                pendingPanelPerformance.store(destination, std::memory_order_release);
                parameters.state.setProperty("factoryProgram", destination, nullptr);
                applyFactoryProgram(destination, false);
            }
        }

        // OS 1.700 has no resident Sequencer operation page. Its Sequencer
        // key is still delivered to the firmware, but it must not displace the
        // currently active engine/UI mode when the firmware leaves the screen
        // unchanged.
        constexpr std::array modeCodes { 38, 33, 34, 32, 37, 36, 39 };
        if (!exclusiveModeOwnsSelection
            && std::find(modeCodes.begin(), modeCodes.end(), diagnosticCode)
                   != modeCodes.end())
        {
            panelSelectedMode.store(diagnosticCode, std::memory_order_release);
            panelSelectedEdit.store(-1, std::memory_order_release);
            if (diagnosticCode == 36)
            {
                instrumentPageReady.store(false, std::memory_order_release);
                startInstrumentPageSelectionDelay.store(
                    true, std::memory_order_release);
                auto selectedInstrument = getSelectedPerformanceInstrument();
                if (selectedInstrument < 0 || selectedInstrument >= 8
                    || !isPerformanceInstrumentActive(selectedInstrument))
                {
                    selectedInstrument = 0;
                    while (selectedInstrument < 8
                           && !isPerformanceInstrumentActive(selectedInstrument))
                        ++selectedInstrument;
                }
                if (selectedInstrument >= 0 && selectedInstrument < 8)
                    pendingInstrumentPageSelection.store(
                        selectedInstrument, std::memory_order_release);
            }
            else
            {
                instrumentPageReady.store(false, std::memory_order_release);
                cancelInstrumentPanelEvents.store(true,
                                                  std::memory_order_release);
            }
        }

        const auto opensEditPage = isPanelEditPageButton(diagnosticCode);
        if (opensEditPage)
        {
            const auto previous = panelSelectedEdit.load(std::memory_order_acquire);
            panelSelectedEdit.store(previous == diagnosticCode ? -1 : diagnosticCode,
                                    std::memory_order_release);
        }

        const auto opensOperatingPage
            = !exclusiveModeOwnsSelection
              && std::find(panelOperatingPageButtonCodes.begin(),
                           panelOperatingPageButtonCodes.end(), diagnosticCode)
                     != panelOperatingPageButtonCodes.end();
        // The real Group Edit switch is only accepted from Instrument Edit or
        // External Edit.  Queue its genuine serial 31 transaction so a brief
        // UI click cannot disappear between firmware scans; OS 1.700 remains
        // solely responsible for changing the LCD page.
        const auto opensGroupEditPage
            = diagnosticCode == 31
              && (selectedPanelMode == 36 || selectedPanelMode == 37);
        if (opensOperatingPage || opensEditPage || opensGroupEditPage)
        {
            // The audio-thread firmware transaction owns the complete edge.
            // This prevents quick Edit clicks from disappearing while the
            // current page is completing an LCD redraw.
            firmwareButtonEventHandled = true;
            panelModeDisplayTransitionActive.store(true,
                                                   std::memory_order_release);
            panelModeDisplayAwaitingDispatch.store(true,
                                                  std::memory_order_release);
            if (!storeExitModePending.load(std::memory_order_acquire))
                pendingFirmwareModeButton.store(diagnosticCode,
                                                std::memory_order_release);
        }
        if (diagnosticCode == 21 || diagnosticCode == 23)
        {
            // The audio-thread transaction owns the complete physical press;
            // mouse-up must not shorten it before OS 1.700 scans the matrix.
            firmwareButtonEventHandled = true;
            pendingFirmwarePageButton.store(diagnosticCode,
                                            std::memory_order_release);
        }
    }
    if (firmwareButtonEventHandled)
        masterFirmware.setPanelButton(buttonId, false);
    else if ((diagnosticCode == 69 || diagnosticCode == 72) && pressed)
    {
        const auto performancePage
            = (panelSelectedMode.load(std::memory_order_acquire) == 39
               || (storeMenuActive.load(std::memory_order_acquire)
                   && completedStoreMode.load(std::memory_order_acquire) == 39))
              && panelSelectedEdit.load(std::memory_order_acquire) < 0;
        if (performancePage)
        {
            // Patch stepping is one discrete panel action per mouse press. A
            // time-stretched matrix contact leaves OS 1.700's independent
            // auto-repeat action live after mouse-up, causing another patch
            // change several seconds later. Clear any preceding action and
            // request the exact adjacent Performance through the existing
            // firmware program-selection transaction.
            // Queue cleanup with the program request. The UI thread must
            // not write firmware repeat state while its CPU is executing.
            stepFactoryPerformance(diagnosticCode == 69 ? -1 : 1);
        }
        else
        {
            // Edit-page +/- arrive as one debounced press/release pair. A
            // time-stretched matrix contact is sampled repeatedly there and
            // races the selected value to its limit.
            const auto wavetableDataDialWasLast
                = panelWavetableDataDialActive.load(std::memory_order_acquire);
            if (wavetableDataDialWasLast)
            {
                // The large Data dial is attached to APVTS as well as the
                // firmware encoder, so while it remains the selected field
                // +/- must take the same host-parameter path immediately.
                if (auto* parameter
                    = parameters.getParameter(wave::parameters::wavetable))
                {
                    const auto current = juce::roundToInt(
                        parameter->convertFrom0to1(parameter->getValue()));
                    const auto stepped = juce::jlimit(
                        1, 128, current + (diagnosticCode == 69 ? -1 : 1));
                    parameter->setValueNotifyingHost(
                        parameter->convertTo0to1(static_cast<float>(stepped)));
                }
            }
            pendingPanelStepButtonEvents.fetch_add(
                diagnosticCode == 69 ? -1 : 1, std::memory_order_release);
        }
    }
    else if (diagnosticCode != 69 && diagnosticCode != 72)
        masterFirmware.setPanelButton(buttonId, pressed);
    if (schedulePerformanceAfterDiskOkRelease)
    {
        // The unavailable panel/disk glue returns the operating page after
        // Total Recall. Queue the genuine Performance serial contact only
        // after the firmware has received OK's release, so OS 1.700 installs
        // the page callback and redraws its own LCD without requester races.
        panelModeDisplayTransitionActive.store(true,
                                               std::memory_order_release);
        panelModeDisplayAwaitingDispatch.store(true,
                                              std::memory_order_release);
        pendingFirmwareModeButton.store(39, std::memory_order_release);
    }
    return true;
}

void WaveEmulationAudioProcessor::setKeyboardControllerButton(
    uint8_t asciiCode, bool pressed) noexcept
{
    // The keyboard controller sends an action byte on the press edge; it does
    // not share the upper front-panel matrix or write to LCD memory.
    if (asciiCode == 0x52u)
        keyboardControllerShiftDown.store(pressed, std::memory_order_release);
    if (pressed)
        masterFirmware.pushKeyboardByte(asciiCode);
}

wave::dsp::WaldorfEngine::VoiceProbe
WaveEmulationAudioProcessor::probeCurrentVoice(int voice, int midiNote,
                                                float velocity, int samples)
{
    const auto performance = std::atomic_load_explicit(
        &factoryPerformance, std::memory_order_acquire);
    if (performance == nullptr)
        return {};
    const auto layer = std::find_if(
        performance->layers.begin(), performance->layers.end(), [](const auto& candidate) {
            return candidate.enabled && !candidate.muted;
        });
    if (layer == performance->layers.end())
        return {};
    return engine.probeVoice(
        voice, *layer, midiNote, juce::jlimit(0.0f, 1.0f, velocity), samples,
        static_cast<int>(std::distance(performance->layers.begin(), layer)));
}

wave::dsp::WaldorfEngine::VoiceProbe
WaveEmulationAudioProcessor::probeCurrentLayerVoice(
    int voice, int layer, int midiNote, float velocity, int samples,
    uint64_t order)
{
    const auto performance = std::atomic_load_explicit(
        &factoryPerformance, std::memory_order_acquire);
    if (performance == nullptr || layer < 0
        || layer >= static_cast<int>(performance->layers.size()))
        return {};
    const auto& selected = performance->layers[static_cast<size_t>(layer)];
    if (!selected.enabled || selected.muted)
        return {};
    return engine.probeVoice(voice, selected, midiNote,
                             juce::jlimit(0.0f, 1.0f, velocity), samples,
                             layer, order);
}

int WaveEmulationAudioProcessor::getFirmwareOscillatorOctave(int oscillator) const noexcept
{
    if (oscillator < 0 || oscillator >= 2)
        return 0;
    // This mirror is refreshed from the selected firmware Sound record, whose
    // octave bytes are also used by the DSP. The panel's serial LED latch can
    // retain a previous Instrument's lamp and is not parameter feedback.
    return panelOscillatorOctaves[static_cast<size_t>(oscillator)].load(
        std::memory_order_acquire);
}

void WaveEmulationAudioProcessor::setPanelAnalog(int controlId, float normalised) noexcept
{
    panelWavetableDataDialActive.store(false, std::memory_order_release);
    masterFirmware.setPanelAnalog(controlId, normalised);
}

void WaveEmulationAudioProcessor::turnPanelEncoder(int encoderId, int steps) noexcept
{
    if (encoderId < 0 || encoderId > 8 || steps == 0)
        return;
    panelWavetableDataDialActive.store(encoderId == 8,
                                       std::memory_order_release);
    pendingPanelEncoderSteps[static_cast<size_t>(encoderId)].fetch_add(
        steps, std::memory_order_release);
}

void WaveEmulationAudioProcessor::beginPanelFaderGesture(int faderIndex,
                                                         bool performanceMode)
{
    if (!performanceMode)
        return;
    if (faderIndex < 0
        || faderIndex >= static_cast<int>(wave::parameters::performanceFader.size()))
        return;
    if (auto* parameter = parameters.getParameter(
            wave::parameters::performanceFader[static_cast<size_t>(faderIndex)]))
        parameter->beginChangeGesture();
}

void WaveEmulationAudioProcessor::setPanelFader(int faderIndex, int controlId,
                                                 float normalised,
                                                 bool performanceMode)
{
    panelWavetableDataDialActive.store(false, std::memory_order_release);
    const auto value = juce::jlimit(0.0f, 1.0f, normalised);
    masterFirmware.setPanelAnalog(controlId, value);
    if (!performanceMode)
    {
        if (panelSelectedEdit.load(std::memory_order_acquire) == 11
            && (faderIndex == 0 || faderIndex == 1
                || faderIndex == 4 || faderIndex == 5))
        {
            const auto instrument = getSelectedPerformanceInstrument();
            const auto program = currentProgram.load(std::memory_order_acquire);
            if (instrument >= 0 && instrument < 8 && program >= 0 && program < 256)
                pendingGlideFaderEdits[static_cast<size_t>(faderIndex)].store(
                    (program << 11) | (instrument << 8) | juce::roundToInt(value * 127.0f),
                    std::memory_order_release);
            return;
        }
        if (panelSelectedMode.load(std::memory_order_acquire) == 36
            && panelSelectedEdit.load(std::memory_order_acquire) < 0
            && instrumentEditPage.load(std::memory_order_acquire) <= 1
            && faderIndex >= 0 && faderIndex < 8)
        {
            const auto instrument = getSelectedPerformanceInstrument();
            const auto program = currentProgram.load(std::memory_order_acquire);
            const auto page = instrumentEditPage.load(std::memory_order_acquire);
            if (instrument >= 0 && instrument < 8 && program >= 0 && program < 256)
            {
                const auto physical = juce::roundToInt(value * 127.0f);
                const auto adc = juce::roundToInt(value * 255.0f);
                pendingInstrumentFaderEdits[static_cast<size_t>(faderIndex)].store(
                    (adc << 21) | (page << 19) | (program << 11) | (instrument << 8)
                        | physical,
                    std::memory_order_release);
            }
            return;
        }
        // On the hardware, the custom ASIC transfers the two contextual
        // modulation-amount faders into the Sound record selected by the OS.
        // Keep the genuine firmware responsible for the page, cursor and +/-
        // selectors; emulate only that absent analogue-to-record transfer.
        if (const auto route = contextualFaderRoute(faderIndex))
            for (const auto& layout : modulationRecordLayouts)
                if (layout.route == *route)
                {
                    masterFirmware.writeCurrentSoundRecordByte(
                        static_cast<uint32_t>(layout.amountOffset),
                        static_cast<uint8_t>(juce::roundToInt(value * 127.0f)));
                    break;
                }
        return;
    }
    if (faderIndex < 0
        || faderIndex >= static_cast<int>(wave::parameters::performanceFader.size()))
        return;
    if (auto* parameter = parameters.getParameter(
            wave::parameters::performanceFader[static_cast<size_t>(faderIndex)]))
        parameter->setValueNotifyingHost(value);
    performanceFadersTouched.fetch_or(
        static_cast<uint8_t>(1u << static_cast<unsigned int>(faderIndex)),
        std::memory_order_release);
    const auto sequence
        = controllerInputSequence.fetch_add(1, std::memory_order_acq_rel) + 1;
    performanceFaderInputSequences[static_cast<size_t>(faderIndex)].store(
        sequence, std::memory_order_release);
    pendingFirmwareFaderValues[static_cast<size_t>(faderIndex)].store(
        juce::roundToInt(value * 127.0f), std::memory_order_release);
}

std::optional<wave::parameters::ModulationRouteIndex>
WaveEmulationAudioProcessor::contextualFaderRoute(int faderIndex) const noexcept
{
    if (faderIndex != 5 && faderIndex != 7)
        return std::nullopt;

    const auto second = faderIndex == 7;
    using namespace wave::parameters;
    switch (panelSelectedEdit.load(std::memory_order_acquire))
    {
        case 8:  return second ? osc1PitchMod2 : osc1PitchMod1;
        case 9:  return second ? osc2PitchMod2 : osc2PitchMod1;
        case 17: return second ? wave1Mod2 : wave1Mod1;
        case 18: return second ? wave2Mod2 : wave2Mod1;
        case 60:
            if (panelFilterSelection.load(std::memory_order_acquire) == 0)
                return second ? filterMod2 : filterMod1;
            return second ? highpassMod2 : highpassMod1;
        case 67: return second ? amplifierMod2 : amplifierMod1;
        case 61: return second ? panMod2 : panMod1;
        case 10:
            if (panelSelectedLfo.load(std::memory_order_acquire) == 0)
                return second ? lfo1LevelMod : lfo1RateMod;
            return second ? lfo2LevelMod : lfo2RateMod;
        default: return std::nullopt;
    }
}

void WaveEmulationAudioProcessor::endPanelFaderGesture(int faderIndex,
                                                       bool performanceMode)
{
    if (!performanceMode)
        return;
    if (faderIndex < 0
        || faderIndex >= static_cast<int>(wave::parameters::performanceFader.size()))
        return;
    if (auto* parameter = parameters.getParameter(
            wave::parameters::performanceFader[static_cast<size_t>(faderIndex)]))
        parameter->endChangeGesture();
}

float WaveEmulationAudioProcessor::getPanelFaderValue(int faderIndex) const noexcept
{
    if (faderIndex < 0
        || faderIndex >= static_cast<int>(wave::parameters::performanceFader.size()))
        return 0.0f;
    if (const auto* parameter = parameters.getParameter(
            wave::parameters::performanceFader[static_cast<size_t>(faderIndex)]))
        return parameter->getValue();
    return 0.0f;
}

void WaveEmulationAudioProcessor::applyPerformanceFaders(
    wave::dsp::WaldorfEngine::PerformanceSnapshot& performance) const
{
    const auto set = currentPerformanceSet();
    if (set == nullptr || !set->isLoaded())
        return;
    const auto selected = currentProgram.load(std::memory_order_acquire);
    const auto factoryRecord = set->performance(selected / 128, selected % 128);
    const auto recordByte = [this, &factoryRecord](size_t offset) {
        return masterFirmware.isLoaded()
                   ? masterFirmware.sharedProgramByte(
                         firmwareEditPerformanceOffset
                         + static_cast<uint32_t>(offset))
                   : factoryRecord[offset];
    };

    // These controller aliases and all eight fader assignments belong to the
    // editable Performance record. Reading the immutable SET here made every
    // assignment selected on the genuine firmware page audibly ineffective.
    performance.controlXController = juce::jlimit(
        0, 120, static_cast<int>(recordByte(28) & 0x7fu));
    performance.controlYController = juce::jlimit(
        0, 120, static_cast<int>(recordByte(29) & 0x7fu));
    const auto touched = performanceFadersTouched.load(std::memory_order_acquire);
    for (size_t fader = 0; fader < wave::parameters::performanceFader.size(); ++fader)
    {
        if ((touched & static_cast<uint8_t>(1u << static_cast<unsigned int>(fader))) == 0u)
            continue;
        const auto* parameter = parameters.getRawParameterValue(
            wave::parameters::performanceFader[fader]);
        if (parameter == nullptr)
            continue;
        const auto normalised = juce::jlimit(0.0f, 1.0f,
                                             parameter->load(std::memory_order_relaxed));
        const auto destination = static_cast<int>(recordByte(8 + fader * 2) & 0x7fu);
        const auto assignment = static_cast<int>(recordByte(9 + fader * 2) & 0x7fu);
        if (destination < 0 || destination >= 8)
            continue; // External destinations transmit MIDI only on the hardware.
        auto& layer = performance.layers[static_cast<size_t>(destination)];
        if (!layer.enabled)
            continue;

        // Performance records store MIDI controllers 1...117 as 0...116,
        // followed by the Wave's dedicated fader assignments.
        if (assignment <= 116)
        {
            // Performance faders store MIDI controllers 1...117 as 0...116.
            // Control X/Y are aliases for the controller numbers selected in
            // the Performance record, so one fader movement may intentionally
            // feed both Modwheel and Control X (as in the factory INIT setup).
            const auto controller = assignment + 1;
            // The panel fader is a physical controller, not a permanent DSP
            // override.  Once the keyboard sends CC1, that newer movement owns
            // Modwheel (and any X/Y alias mapped to CC1) until this particular
            // fader is moved again.
            if (controller == 1
                && performanceFaderInputSequences[fader].load(
                       std::memory_order_acquire)
                       <= modWheelInputSequence.load(std::memory_order_acquire))
                continue;
            if (controller == 1)
                layer.performanceModWheel = normalised;
            if (controller == performance.controlXController)
                layer.performanceControlX = normalised;
            if (controller == performance.controlYController)
                layer.performanceControlY = normalised;
        }
        else if (assignment == 117) // Bend +
            layer.performancePitchBend = normalised * 2.0f;
        else if (assignment == 118) // Bend -
            layer.performancePitchBend = (normalised - 1.0f) * 2.0f;
        else if (assignment == 119) // Bipolar bend
            layer.performancePitchBend = (normalised * 2.0f - 1.0f) * 2.0f;
        else if (assignment == 120) // Channel pressure
            layer.performanceChannelPressure = normalised;
        else if (assignment == 121) // Dedicated Control X
            layer.performanceControlX = normalised;
        else if (assignment == 122) // Dedicated Control Y
            layer.performanceControlY = normalised;
        else if (assignment == 123) // Dedicated Instrument Detune
            layer.detuneCents = static_cast<float>(juce::roundToInt(normalised * 127.0f) - 64);
        else if (assignment == 124) // Dedicated Instrument Volume
            layer.gain = normalised;
        else if (assignment == 125) // Dedicated Instrument Pan
            layer.sound.panAmount = juce::jlimit(-1.0f, 1.0f,
                                                 normalised * 2.0f - 1.0f);
    }
}

bool WaveEmulationAudioProcessor::getPanelLed(int ledId) const noexcept
{
    // The Instrument status LEDs are bi-colour and belong to the decoded
    // Performance state. Handle them before the firmware output latch: an
    // incomplete periodic firmware scan must not leave stale red/green bits
    // asserted over a newly loaded Performance.
    const auto instrumentMode = getInstrumentButtonMode();
    if (ledId == 43)
        return instrumentMode == InstrumentButtonMode::mute;
    if (ledId == 59)
        return instrumentMode == InstrumentButtonMode::solo;
    if (ledId >= 96 && ledId <= 111)
    {
        const auto instrument = (ledId - 96) / 2;
        if (!isPerformanceInstrumentActive(instrument))
            return false;
        const auto green = (ledId & 1) == 0;
        if (instrumentMode != InstrumentButtonMode::normal)
            return green ? isPerformanceInstrumentAudible(instrument)
                         : !isPerformanceInstrumentAudible(instrument);

        const auto selected = getSelectedPerformanceInstrument() == instrument;
        if (!selected)
            return green;

        // A selected display softkey flashes the bi-colour lamp orange/off.
        // Both dies must follow the same phase: leaving the green die steady
        // would incorrectly produce the former orange/green indication.
        const auto orangePhase
            = (juce::Time::getMillisecondCounter() / 350u) % 2u != 0u;
        return orangePhase;
    }

    // These four LEDs are mutually exclusive positions of one selector. The
    // firmware writes the multiplexed output banks a byte at a time, so
    // exposing those in-progress writes here makes the visible selection race
    // through Wave 1-4, Wave 5-8, and Free while no control is being touched.
    // Use the stable panel state before consulting the raw firmware output.
    if (ledId == 34 || ledId == 40 || ledId == 23 || ledId == 39)
    {
        if (panelSelectedMode.load(std::memory_order_acquire) == 38)
            return ledId == 34;
        constexpr std::array<int, 3> pageLeds { 39, 23, 40 };
        const auto page = juce::jlimit(
            0, 2, panelWaveEnvelopePage.load(std::memory_order_acquire));
        return ledId == pageLeds[static_cast<size_t>(page)];
    }

    // Operating modes are also mutually exclusive. Firmware LED banks are
    // updated independently from the serial page event, so consulting a stale
    // bank first can illuminate both the old and newly requested modes.
    constexpr std::array modeCodes { 38, 33, 35, 34, 32, 37, 36, 39 };
    constexpr std::array modeLeds { 64, 69, 21, 74, 26, 10, 22, 51 };
    auto lampMode = panelSelectedMode.load(std::memory_order_acquire);
    if (storeMenuActive.load(std::memory_order_acquire))
        if (const auto completed = completedStoreMode.load(std::memory_order_acquire);
            completed >= 0)
            lampMode = completed;
    for (size_t index = 0; index < modeCodes.size(); ++index)
        if (ledId == modeLeds[index])
            return lampMode == modeCodes[index];

    // An Edit overlay has exactly one selected section. The 6522 output banks
    // are written independently and the raw firmware latch can briefly retain
    // the previous section while asserting the next one. Resolve every Edit
    // lamp from the processor's engine-side selected overlay before exposing
    // that transient hardware latch to the editor.
    for (const auto& indicator : wave::panel::editIndicators)
        if (ledId == indicator.ledSerialCode)
            return panelSelectedEdit.load(std::memory_order_acquire)
                   == indicator.buttonDiagnosticCode;

    // Until the 6522 timer/priority encoder delivers the genuine periodic LED
    // refresh, keep the output board model in the engine. The editor only
    // reads this API; it no longer invents LED state from its own controls.
    for (size_t oscillator = 0;
         oscillator < wave::panel::oscillatorOctaveLedSerialCodes.size();
         ++oscillator)
        for (size_t position = 0;
             position
                 < wave::panel::oscillatorOctaveLedSerialCodes[oscillator].size();
             ++position)
            if (ledId
                == wave::panel::oscillatorOctaveLedSerialCodes[oscillator][position])
                return getFirmwareOscillatorOctave(static_cast<int>(oscillator))
                       == 2 - static_cast<int>(position);

    const auto selectedLfo = juce::jlimit(
        0, 1, panelSelectedLfo.load(std::memory_order_acquire));
    if (ledId == 24 || ledId == 8)
        return selectedLfo == (ledId == 24 ? 1 : 0);

    constexpr std::array<int, 6> lfoShapeLeds { 67, 19, 72, 66, 2, 49 };
    if (std::find(lfoShapeLeds.begin(), lfoShapeLeds.end(), ledId)
        != lfoShapeLeds.end())
    {
        const auto* value = parameters.getRawParameterValue(
            wave::parameters::lfoShape[static_cast<size_t>(selectedLfo)]);
        const auto shape = value != nullptr
                               ? juce::jlimit(0, 5, juce::roundToInt(value->load()))
                               : 0;
        return ledId == lfoShapeLeds[static_cast<size_t>(shape)];
    }
    if (ledId == 17 || ledId == 56)
    {
        const auto* value = parameters.getRawParameterValue(
            wave::parameters::lfoSync[static_cast<size_t>(selectedLfo)]);
        const auto sync = value != nullptr
                              ? juce::jlimit(0, 2, juce::roundToInt(value->load()))
                              : 0;
        return (ledId == 17 && sync == 1) || (ledId == 56 && sync == 2);
    }

    if (ledId == 45 || ledId == 37 || ledId == 5 || ledId == 0)
    {
        const auto* value = parameters.getRawParameterValue(
            wave::parameters::filterMode);
        const auto mode = value != nullptr
                              ? juce::jlimit(0, 3, juce::roundToInt(value->load()))
                              : 0;
        constexpr std::array<int, 4> filterModeLeds { 0, 5, 37, 45 };
        return ledId == filterModeLeds[static_cast<size_t>(mode)];
    }
    if (ledId == 53 || ledId == 41)
        return panelFilterSelection.load(std::memory_order_acquire)
               == (ledId == 53 ? 1 : 0);
    if (ledId == 85 || ledId == 84)
        return (currentProgram.load(std::memory_order_acquire) / 128 == 1)
               == (ledId == 85);

    // Disk stays lit while its workspace is active. Store gives way to the
    // preceding operating-mode lamp after a successful save, while the
    // native chooser remains available for another record.
    if (ledId == 52)
        return diskMenuActive.load(std::memory_order_acquire);
    if (ledId == 87)
        return storeMenuActive.load(std::memory_order_acquire)
               && completedStoreMode.load(std::memory_order_acquire) < 0;

    struct MomentaryLed
    {
        int led;
        int diagnosticCode;
    };
    constexpr std::array momentaryLeds {
        MomentaryLed { 33, 16 },
        MomentaryLed { 3, -1 },
        MomentaryLed { 82, -2 }, MomentaryLed { 83, 42 },
        MomentaryLed { 62, 41 }, MomentaryLed { 46, 40 },
        MomentaryLed { 88, 47 }
    };
    for (const auto& item : momentaryLeds)
    {
        if (ledId != item.led)
            continue;
        const auto matrix = item.diagnosticCode == -1
                                ? 71
                                : item.diagnosticCode == -2
                                      ? 70
                                      : wave::panel::matrixIndexForDiagnosticCode(
                                            item.diagnosticCode);
        return matrix >= 0
               && panelButtonDown[static_cast<size_t>(matrix)].load(
                   std::memory_order_acquire);
    }

    // Only unmodelled outputs fall through to the raw 6522 latch. Every
    // selector above must reject stale bits as well as accept the selected bit;
    // consulting the latch first can illuminate both its stored and new state.
    return masterFirmware.panelLed(ledId);
}

void WaveEmulationAudioProcessor::noteOnFromUi(int midiNote, float velocity)
{
    uiMidiCollector.addMessageToQueue(
        juce::MidiMessage::noteOn(1, juce::jlimit(0, 127, midiNote),
                                  juce::jlimit(0.0f, 1.0f, velocity)));
}

void WaveEmulationAudioProcessor::noteOffFromUi(int midiNote)
{
    uiMidiCollector.addMessageToQueue(
        juce::MidiMessage::noteOff(1, juce::jlimit(0, 127, midiNote)));
}

void WaveEmulationAudioProcessor::allSoundOffFromUi()
{
    uiMidiCollector.addMessageToQueue(juce::MidiMessage::allSoundOff(1));
}

void WaveEmulationAudioProcessor::setPitchWheelFromUi(float normalised)
{
    const auto value = juce::jlimit(
        0, 16383, juce::roundToInt(juce::jlimit(0.0f, 1.0f, normalised)
                                   * 16383.0f));
    keyboardPitchWheel.store(value, std::memory_order_release);
    uiMidiCollector.addMessageToQueue(juce::MidiMessage::pitchWheel(1, value));
}

void WaveEmulationAudioProcessor::setModWheelFromUi(float normalised)
{
    const auto value = juce::jlimit(
        0, 127, juce::roundToInt(juce::jlimit(0.0f, 1.0f, normalised)
                                 * 127.0f));
    keyboardModWheel.store(value, std::memory_order_release);
    uiMidiCollector.addMessageToQueue(
        juce::MidiMessage::controllerEvent(1, 1, value));
}

void WaveEmulationAudioProcessor::setFreeWheelFromUi(float normalised)
{
    const auto value = juce::jlimit(
        0, 127, juce::roundToInt(juce::jlimit(0.0f, 1.0f, normalised)
                                 * 127.0f));
    keyboardFreeWheel.store(value, std::memory_order_release);
    // The physical keyboard controller has a dedicated Free Wheel path. CC16
    // is reserved internally here so the three authentic Free Wheel modifier
    // sources remain distinct from Modwheel and Control X/Y.
    uiMidiCollector.addMessageToQueue(
        juce::MidiMessage::controllerEvent(1, 16, value));
}

juce::AudioProcessorEditor* WaveEmulationAudioProcessor::createEditor()
{
    return new WaveEmulationAudioProcessorEditor(*this);
}

void WaveEmulationAudioProcessor::getStateInformation(juce::MemoryBlock& destinationData)
{
    // AU hosts may save/restore from a non-render thread without taking this
    // lock. The firmware, disk and calibration state must remain one snapshot.
    const juce::ScopedLock callbackLock(getCallbackLock());
    auto state = parameters.copyState();
    state.removeProperty("ecoMode", nullptr);
    // These are regenerated from the current machine. copyState() may still
    // contain snapshots from an earlier project or a previously loaded disk.
    for (const auto* key : { "machineActiveSetData", "machineDiskImageData",
                             "machineDiskImageName", "mountedDiskImage",
                             "machinePerformanceSnapshot", "machineFirmwareEditBuffers" })
        state.removeProperty(key, nullptr);
    state.setProperty("machineStateSchema", machineStateSchemaVersion, nullptr);
    state.setProperty("factoryProgram",
                      currentProgram.load(std::memory_order_acquire), nullptr);
    state.setProperty("factoryStateSchema", factoryStateSchemaVersion, nullptr);
    state.setProperty("performanceFadersTouched",
                      static_cast<int>(performanceFadersTouched.load(std::memory_order_acquire)),
                      nullptr);
    if (firmware.getDirectory().isDirectory())
        state.setProperty("firmwareDirectory", firmware.getDirectory().getFullPathName(), nullptr);
    if (masterFirmware.hasMountedDiskImage())
    {
        state.setProperty("mountedDiskImage",
                          masterFirmware.mountedDiskImageFile().getFullPathName(), nullptr);
        state.setProperty("machineDiskImageName",
                          masterFirmware.mountedDiskImageFile().getFileName(), nullptr);
        const auto diskImage = masterFirmware.mountedDiskImageSnapshot();
        if (!diskImage.isEmpty())
            state.setProperty("machineDiskImageData", juce::var(diskImage), nullptr);
    }

    const auto activeSet = currentPerformanceSet();
    state.setProperty("machineHasActiveSet",
                      activeSet != nullptr && activeSet->isLoaded(), nullptr);
    if (activeSet != nullptr && activeSet->isLoaded()
        && !activeSet->sourceImage().isEmpty())
    {
        auto setImage = activeSet->sourceImage();
        if (masterFirmware.isLoaded()
            && !pendingDiskSetActivation.load(std::memory_order_acquire))
        {
            // Store changes native SRAM, not the imported SET. Persist both
            // complete banks alongside the separate current edit buffers.
            std::vector<uint8_t> sounds(activeSet->soundBank().size());
            std::vector<uint8_t> performances(activeSet->performanceBank().size());
            for (size_t byte = 0; byte < sounds.size(); ++byte)
                sounds[byte] = masterFirmware.sharedProgramByte(
                    0x18000u + static_cast<uint32_t>(byte));
            for (size_t byte = 0; byte < performances.size(); ++byte)
                performances[byte] = masterFirmware.sharedProgramByte(
                    0x28000u + static_cast<uint32_t>(byte));
            setImage = activeSet->imageWithStoredBanks(sounds, performances);
        }
        state.setProperty("machineActiveSetData",
                          juce::var(setImage), nullptr);
    }

    auto persistentPerformance = std::atomic_load_explicit(
        &factoryPerformance, std::memory_order_acquire);
    if (persistentPerformance != nullptr
        && !pendingDiskSetActivation.load(std::memory_order_acquire))
    {
        auto exactPerformance = *persistentPerformance;
        const auto editable = exactPerformance.editableLayer;
        const auto confirmed = std::atomic_load_explicit(
            &latestFirmwareSelectedSound, std::memory_order_acquire);
        if (editable >= 0 && editable < 8 && confirmed != nullptr
            && confirmed->performance == currentProgram.load(std::memory_order_acquire)
            && confirmed->instrument == editable)
        {
            const auto extensions
                = exactPerformance.layers[static_cast<size_t>(editable)].sound;
            auto exactSound = confirmed->sound;
            preserveUnexposedControlFields(exactSound, extensions);
            exactSound.panAmount = extensions.panAmount;
            exactSound.panModulationMode = extensions.panModulationMode;
            exactSound.driveDb = extensions.driveDb;
            exactSound.quickEditAmounts = extensions.quickEditAmounts;
            exactSound.outputDb = extensions.outputDb;
            exactSound.circuitAgeAmount = extensions.circuitAgeAmount;
            for (size_t oscillator = 0; oscillator < panelOscillatorOctaves.size();
                 ++oscillator)
                exactSound.oscillatorOctaves[oscillator]
                    = panelOscillatorOctaves[oscillator].load(
                        std::memory_order_acquire);
            exactPerformance.layers[static_cast<size_t>(editable)].sound
                = exactSound;
        }
        static_assert(std::is_trivially_copyable_v<
                      wave::dsp::WaldorfEngine::PerformanceSnapshot>);
        state.setProperty(
            "machinePerformanceSnapshot",
            juce::var(juce::MemoryBlock(&exactPerformance,
                                        sizeof(exactPerformance))), nullptr);
    }

    auto editBuffers = std::atomic_load_explicit(
        &latestHostMachineSnapshot, std::memory_order_acquire);
    if (editBuffers == nullptr
        || editBuffers->performance
               != currentProgram.load(std::memory_order_acquire))
    {
        if (activeSet != nullptr && activeSet->isLoaded())
        {
            auto fallback = std::make_shared<HostMachineSnapshot>();
            fallback->performance = currentProgram.load(std::memory_order_acquire);
            const auto performance = activeSet->performance(
                fallback->performance / 128, fallback->performance % 128);
            std::copy(performance.begin(), performance.end(),
                      fallback->performanceRecord.begin());
            const auto seed = std::atomic_load_explicit(
                &instrumentSoundSeed, std::memory_order_acquire);
            if (seed != nullptr && seed->performance == fallback->performance)
            {
                fallback->soundRecords = seed->records;
                fallback->soundRecordValid = seed->valid;
            }
            fallback->selectedInstrument
                = persistentPerformance != nullptr
                      ? persistentPerformance->editableLayer
                      : static_cast<int>(performance[24]);
            editBuffers = std::static_pointer_cast<const HostMachineSnapshot>(fallback);
        }
    }
    if (editBuffers != nullptr && activeSet != nullptr && activeSet->isLoaded()
        && !pendingDiskSetActivation.load(std::memory_order_acquire))
    {
        juce::MemoryBlock binary;
        juce::MemoryOutputStream stream(binary, false);
        stream.writeInt(machineEditBufferMagic);
        stream.writeInt(machineStateSchemaVersion);
        stream.writeInt(editBuffers->performance);
        stream.writeInt(editBuffers->selectedInstrument);
        stream.write(editBuffers->performanceRecord.data(),
                     editBuffers->performanceRecord.size());
        for (size_t instrument = 0; instrument < editBuffers->soundRecords.size();
             ++instrument)
        {
            stream.writeByte(editBuffers->soundRecordValid[instrument] ? 1 : 0);
            stream.write(editBuffers->soundRecords[instrument].data(),
                         editBuffers->soundRecords[instrument].size());
        }
        state.setProperty("machineFirmwareEditBuffers", juce::var(binary), nullptr);
    }

    juce::MemoryBlock calibration;
    calibration.setSize(protectedFilterCalibrationCodes.size() * 2u, false);
    auto* calibrationBytes = static_cast<uint8_t*>(calibration.getData());
    for (size_t voice = 0; voice < protectedFilterCalibrationCodes.size(); ++voice)
    {
        const auto code = protectedFilterCalibrationCodes[voice];
        calibrationBytes[voice * 2u] = static_cast<uint8_t>(code >> 8u);
        calibrationBytes[voice * 2u + 1u] = static_cast<uint8_t>(code);
    }
    state.setProperty("machineVcfCalibration", juce::var(calibration), nullptr);
    state.setProperty("machineSelectedLfo", panelSelectedLfo.load(), nullptr);
    state.setProperty("machineWaveEnvelopePage", panelWaveEnvelopePage.load(), nullptr);
    state.setProperty("machineFilterSelection", panelFilterSelection.load(), nullptr);
    state.setProperty("machineSelectedMode", panelSelectedMode.load(), nullptr);
    state.setProperty("machineSelectedEdit", panelSelectedEdit.load(), nullptr);
    state.setProperty("machineInstrumentEditPage", instrumentEditPage.load(), nullptr);
    state.setProperty("machineInstrumentButtonMode", instrumentButtonMode.load(), nullptr);
    state.setProperty("machineOscillatorOctave1", panelOscillatorOctaves[0].load(), nullptr);
    state.setProperty("machineOscillatorOctave2", panelOscillatorOctaves[1].load(), nullptr);
    state.setProperty("machineMidiIsLocalKeyboard", midiInputActsAsLocalKeyboard.load(), nullptr);
    state.setProperty("machineKeyboardOctaveShift", keyboardOctaveShift.load(), nullptr);
    state.setProperty("machineKeyboardModWheel", keyboardModWheel.load(), nullptr);
    state.setProperty("machineKeyboardFreeWheel", keyboardFreeWheel.load(), nullptr);

    // Sound parameters and physical panel positions are distinct state on a
    // Wave. Relative Knob Mode relies on that difference, so do not let the
    // host's APVTS parameter snapshot stand in for the analogue inputs.
    juce::MemoryBlock panelPots;
    panelPots.setSize(wave::panel::visiblePots.size(), false);
    auto* panelPotBytes = static_cast<uint8_t*>(panelPots.getData());
    for (size_t index = 0; index < wave::panel::visiblePots.size(); ++index)
        panelPotBytes[index] = masterFirmware.panelAnalogByte(
            wave::panel::visiblePots[index].adcChannel);
    state.setProperty("machinePanelPotAdcValues", juce::var(panelPots), nullptr);

    if (const auto xml = state.createXml())
        copyXmlToBinary(*xml, destinationData);
}

void WaveEmulationAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    // Rebooting the emulated CPUs and replacing shared SRAM must not overlap
    // this instance's audio callback, even though different instances can run
    // concurrently. JUCE's AU RestoreState does not acquire this lock for us.
    const juce::ScopedLock callbackLock(getCallbackLock());
    if (const auto xml = getXmlFromBinary(data, sizeInBytes))
    {
        if (xml->hasTagName(parameters.state.getType()))
        {
            auto restoredState = juce::ValueTree::fromXml(*xml);
            // Retired reduced-rate Eco settings must not degrade recalled audio.
            restoredState.removeProperty("ecoMode", nullptr);
            // Edit buffers belong to the incoming project, not the instance's
            // previously selected disk or Performance with the same number.
            std::atomic_store_explicit(&latestFirmwareSelectedSound,
                std::shared_ptr<const FirmwareSelectedSoundState>{}, std::memory_order_release);
            std::atomic_store_explicit(&latestHostMachineSnapshot,
                std::shared_ptr<const HostMachineSnapshot>{}, std::memory_order_release);
            std::atomic_store_explicit(&pendingHostMachineRestore,
                std::shared_ptr<const HostMachineSnapshot>{}, std::memory_order_release);
            const auto exactMachineState
                = static_cast<int>(restoredState.getProperty(
                      "machineStateSchema", 0)) == machineStateSchemaVersion;
            const auto restoreEditableFactorySound
                = static_cast<int>(restoredState.getProperty("factoryStateSchema", 0))
                  == factoryStateSchemaVersion;
            const auto restoredPerformanceFadersTouched = static_cast<uint8_t>(
                juce::jlimit(0, 255, static_cast<int>(restoredState.getProperty(
                                           "performanceFadersTouched", 0))));
            std::shared_ptr<const wave::presets::WaveFactorySet> restoredExactSet;
            const auto activeSetData = restoredState.getProperty("machineActiveSetData");
            if (exactMachineState)
            {
                const auto hasActiveSet = static_cast<bool>(restoredState.getProperty(
                    "machineHasActiveSet", true));
                if (const auto* bytes = activeSetData.getBinaryData();
                    hasActiveSet && bytes != nullptr)
                {
                    auto set = std::make_shared<wave::presets::WaveFactorySet>();
                    if (set->loadStateSnapshot(*bytes).validLayout)
                        restoredExactSet = std::static_pointer_cast<
                            const wave::presets::WaveFactorySet>(set);
                }
                if (restoredExactSet != nullptr)
                    setActivePerformanceSet(restoredExactSet);
                else if (!hasActiveSet)
                    resetToColdStart();
            }

            parameters.replaceState(restoredState.createCopy());
            const auto restoredSet = currentPerformanceSet();
            const auto restoredProgram
                = restoredSet != nullptr && restoredSet->isLoaded()
                      ? juce::jlimit(
                            0, wave::presets::WaveFactorySet::programCount - 1,
                            static_cast<int>(parameters.state.getProperty(
                                "factoryProgram", 0)))
                      : 0;
            currentProgram.store(restoredProgram, std::memory_order_release);
            performanceFadersTouched.store(0, std::memory_order_release);
            const auto savedDirectory = parameters.state.getProperty("firmwareDirectory").toString();
            if (savedDirectory.isNotEmpty())
            {
                const auto report = loadFirmware(juce::File(savedDirectory), false);
                if (!report.hasBothImages() && !loadRememberedFirmware())
                    loadEmbeddedPrivateRoms();
            }
            // Embedded-firmware sessions can restore without rebooting.
            // Reinstall the native bank even if this instance previously
            // imported a PPG image, then resolve this SET's user references.
            if (engine.loadWaveFactoryRom(firmware.getMasterImage())
                && restoredSet != nullptr)
                engine.loadWaveSetUserTables(restoredSet->sourceImage());
            removeRestoredHostStateDisk();
            (void) masterFirmware.ejectDiskImage();
            auto restoredEmbeddedDisk = false;
            const auto diskData = restoredState.getProperty("machineDiskImageData");
            if (exactMachineState)
                if (const auto* bytes = diskData.getBinaryData();
                    bytes != nullptr && !bytes->isEmpty())
                {
                    auto name = juce::File::createLegalFileName(
                        restoredState.getProperty("machineDiskImageName",
                                                  "Session Disk.img").toString());
                    if (name.isEmpty())
                        name = "Session Disk.img";
                    const auto directory = juce::File::getSpecialLocation(
                                               juce::File::tempDirectory)
                                               .getChildFile(hostStateDirectoryName);
                    directory.createDirectory();
                    restoredHostStateDirectory
                        = directory.getChildFile(hostStateInstanceId);
                    restoredHostStateDirectory.createDirectory();
                    const auto sessionDisk
                        = restoredHostStateDirectory.getChildFile(name);
                    if (sessionDisk.replaceWithData(bytes->getData(), bytes->getSize())
                        && masterFirmware.mountDiskImage(sessionDisk).wasOk())
                    {
                        parameters.state.setProperty(
                            "mountedDiskImage", sessionDisk.getFullPathName(), nullptr);
                        restoredEmbeddedDisk = true;
                    }
                    else
                        removeRestoredHostStateDisk();
                }
            if (!restoredEmbeddedDisk)
            {
                const auto savedDisk
                    = parameters.state.getProperty("mountedDiskImage").toString();
                if (savedDisk.isNotEmpty())
                    mountDiskImage(juce::File(savedDisk));
            }
            // Mounting a disk must not replace the self-contained SET snapshot
            // captured by the host, nor arm a later Total Recall activation.
            if (restoredExactSet != nullptr)
                setActivePerformanceSet(restoredExactSet);
            pendingDiskSetActivation.store(false, std::memory_order_release);
            pendingManagerExitProgram.store(-1, std::memory_order_release);
            pendingDiskSetBytes.store(0, std::memory_order_release);
            diskSetImportConfirmationPending.store(
                false, std::memory_order_release);
            const auto set = currentPerformanceSet();
            if (set != nullptr && set->isLoaded())
            {
                // Embedded firmware may already be running, so restoring a
                // SET does not necessarily reboot through startLoadedFirmware.
                // Install the full banks before selecting the saved program.
                masterFirmware.installSoundBank(set->soundBank());
                masterFirmware.installPerformanceBank(set->performanceBank());
                // Reconstruct all eight factory Performance elements, then restore
                // current-version edits over that template. Older standalone state
                // predates complete Performance layering and may contain a stale pan
                // for the selected element; retain its patch number but migrate the
                // sound to the exact factory data.
                applyFactoryProgram(restoredProgram, false);
                if (restoreEditableFactorySound)
                    parameters.replaceState(restoredState.createCopy());
                else
                    parameters.state.setProperty("factoryStateSchema",
                                                 factoryStateSchemaVersion, nullptr);
                if (exactMachineState)
                {
                    const auto performanceData = restoredState.getProperty(
                        "machinePerformanceSnapshot");
                    if (const auto* bytes = performanceData.getBinaryData();
                        bytes != nullptr
                        && bytes->getSize()
                               == sizeof(wave::dsp::WaldorfEngine::PerformanceSnapshot))
                    {
                        auto restoredPerformance = std::make_shared<
                            wave::dsp::WaldorfEngine::PerformanceSnapshot>();
                        std::memcpy(restoredPerformance.get(), bytes->getData(),
                                    sizeof(*restoredPerformance));
                        restoredPerformance->editableLayer = juce::jlimit(
                            -1, 7, restoredPerformance->editableLayer);
                        std::atomic_store_explicit(
                            &factoryPerformance,
                            std::static_pointer_cast<const wave::dsp::WaldorfEngine::PerformanceSnapshot>(
                                restoredPerformance),
                            std::memory_order_release);
                    }

                    const auto editData = restoredState.getProperty(
                        "machineFirmwareEditBuffers");
                    if (const auto* bytes = editData.getBinaryData(); bytes != nullptr)
                    {
                        juce::MemoryInputStream stream(*bytes, false);
                        if (stream.readInt() == machineEditBufferMagic
                            && stream.readInt() == machineStateSchemaVersion)
                        {
                            auto machine = std::make_shared<HostMachineSnapshot>();
                            machine->performance = stream.readInt();
                            machine->selectedInstrument = stream.readInt();
                            auto valid = stream.read(
                                machine->performanceRecord.data(),
                                static_cast<int>(machine->performanceRecord.size()))
                                         == static_cast<int>(
                                             machine->performanceRecord.size());
                            for (size_t instrument = 0;
                                 valid && instrument < machine->soundRecords.size();
                                 ++instrument)
                            {
                                machine->soundRecordValid[instrument]
                                    = stream.readByte() != 0;
                                valid = stream.read(
                                            machine->soundRecords[instrument].data(),
                                            static_cast<int>(machine->soundRecords[
                                                instrument].size()))
                                        == static_cast<int>(machine->soundRecords[
                                            instrument].size());
                            }
                            if (valid && machine->performance == restoredProgram
                                && machine->selectedInstrument >= 0
                                && machine->selectedInstrument < 8)
                            {
                                auto seed = std::make_shared<
                                    PerformanceInstrumentSoundSeed>();
                                seed->performance = machine->performance;
                                seed->records = machine->soundRecords;
                                seed->valid = machine->soundRecordValid;
                                std::atomic_store_explicit(
                                    &instrumentSoundSeed,
                                    std::static_pointer_cast<const PerformanceInstrumentSoundSeed>(seed),
                                    std::memory_order_release);
                                const auto immutable = std::static_pointer_cast<
                                    const HostMachineSnapshot>(machine);
                                std::atomic_store_explicit(
                                    &latestHostMachineSnapshot, immutable,
                                    std::memory_order_release);
                                std::atomic_store_explicit(
                                    &pendingHostMachineRestore, immutable,
                                    std::memory_order_release);
                            }
                        }
                    }
                }
                // Shared firmware records are installed by the audio-thread
                // transaction in runFirmwareTimeline(), never concurrently
                // from this state/message thread.
                pendingFirmwareProgram.store(restoredProgram, std::memory_order_release);
            }
            if (exactMachineState)
            {
                const auto calibration = restoredState.getProperty(
                    "machineVcfCalibration");
                if (const auto* bytes = calibration.getBinaryData();
                    bytes != nullptr
                    && bytes->getSize()
                           == protectedFilterCalibrationCodes.size() * 2u)
                {
                    const auto* source = static_cast<const uint8_t*>(bytes->getData());
                    for (size_t voice = 0;
                         voice < protectedFilterCalibrationCodes.size(); ++voice)
                        protectedFilterCalibrationCodes[voice]
                            = static_cast<uint16_t>(
                                (static_cast<uint16_t>(source[voice * 2u]) << 8u)
                                | source[voice * 2u + 1u]);
                }
                panelSelectedLfo.store(juce::jlimit(
                    0, 1, static_cast<int>(restoredState.getProperty(
                              "machineSelectedLfo", 0))));
                panelWaveEnvelopePage.store(juce::jlimit(
                    0, 2, static_cast<int>(restoredState.getProperty(
                              "machineWaveEnvelopePage", 0))));
                panelFilterSelection.store(juce::jlimit(
                    0, 1, static_cast<int>(restoredState.getProperty(
                              "machineFilterSelection", 0))));
                // Sound, Performance, disk and panel-control values are
                // persistent machine state. A page selector is not safe to
                // restore by itself: the freshly booted OS is on Performance,
                // and its screen callbacks/action table have not entered the
                // saved edit page. Claiming that old mode in the engine makes
                // contextual +/- serials reach a different firmware page and
                // become a held Performance auto-repeat. Start every restored
                // firmware session on its genuine Performance page.
                panelSelectedMode.store(39, std::memory_order_release);
                panelSelectedEdit.store(-1, std::memory_order_release);
                cancelPanelStepButtonEvents.store(true,
                                                  std::memory_order_release);
                instrumentEditPage.store(juce::jlimit(
                    0, 3, static_cast<int>(restoredState.getProperty(
                              "machineInstrumentEditPage", 0))));
                instrumentButtonMode.store(juce::jlimit(
                    static_cast<int>(InstrumentButtonMode::normal),
                    static_cast<int>(InstrumentButtonMode::solo),
                    static_cast<int>(restoredState.getProperty(
                        "machineInstrumentButtonMode", 0))));
                panelOscillatorOctaves[0].store(juce::jlimit(
                    -2, 2, static_cast<int>(restoredState.getProperty(
                               "machineOscillatorOctave1", 0))));
                panelOscillatorOctaves[1].store(juce::jlimit(
                    -2, 2, static_cast<int>(restoredState.getProperty(
                               "machineOscillatorOctave2", 0))));
                midiInputActsAsLocalKeyboard.store(static_cast<bool>(
                    restoredState.getProperty("machineMidiIsLocalKeyboard", false)));
                const auto restoredKeyboardOctave = juce::jlimit(
                    -1, 1, static_cast<int>(restoredState.getProperty(
                                "machineKeyboardOctaveShift", 0)));
                keyboardOctaveShift.store(restoredKeyboardOctave,
                                          std::memory_order_release);
                pendingKeyboardOctaveRestore.store(restoredKeyboardOctave,
                                                   std::memory_order_release);
                keyboardOctaveRestoreWaitBlocks.store(
                    0, std::memory_order_release);
                const auto restoredModWheel = juce::jlimit(
                    0, 127, static_cast<int>(restoredState.getProperty(
                                "machineKeyboardModWheel", 0)));
                const auto restoredFreeWheel = juce::jlimit(
                    0, 127, static_cast<int>(restoredState.getProperty(
                                "machineKeyboardFreeWheel", 64)));
                setModWheelFromUi(static_cast<float>(restoredModWheel) / 127.0f);
                setFreeWheelFromUi(static_cast<float>(restoredFreeWheel) / 127.0f);
                setPitchWheelFromUi(8192.0f / 16383.0f);
            }
            // Standalone state is also the emulated panel's persistent memory.
            // Keep both the physical fader positions and the touched mask so a
            // Control-X/Y-gated modulation route behaves identically after a
            // relaunch instead of silently reopening with its controller at 0.
            performanceFadersTouched.store(restoredPerformanceFadersTouched,
                                           std::memory_order_release);
            auto restoredPhysicalPanel = false;
            if (const auto* panelPots
                = restoredState.getProperty("machinePanelPotAdcValues")
                      .getBinaryData();
                panelPots != nullptr
                && panelPots->getSize() == wave::panel::visiblePots.size())
            {
                const auto* bytes = static_cast<const uint8_t*>(
                    panelPots->getData());
                for (size_t index = 0; index < wave::panel::visiblePots.size();
                     ++index)
                    masterFirmware.setPanelAnalog(
                        wave::panel::visiblePots[index].adcChannel,
                        static_cast<float>(bytes[index]) / 255.0f);
                restoredPhysicalPanel = true;
                relativePanelPotRebasePending.store(true,
                                                    std::memory_order_release);
            }
            // Older sessions did not distinguish the physical panel from the
            // sound snapshot. Retain their previous migration behaviour while
            // all newly saved sessions restore the independent ADC positions.
            if (!restoredPhysicalPanel)
                synchronisePanelPotInputsToParameters();
            seedFilterCalibrationTable();
        }
    }
}

bool WaveEmulationAudioProcessor::loadRememberedFirmware()
{
    const auto path = firmwarePreferenceFile.loadFileAsString().trim();
    if (!juce::File::isAbsolutePath(path) || !juce::File(path).isDirectory())
        return false;
    return loadFirmware(juce::File(path), false).hasBothImages();
}

wave::firmware::Bundle::Report WaveEmulationAudioProcessor::loadFirmware(
    const juce::File& directoryOrImage, bool rememberForNewInstances)
{
    // Validate before replacing a working pair, including on stale session paths.
    wave::firmware::Bundle candidate;
    auto report = candidate.load(directoryOrImage);
    if (!report.hasBothImages())
        return report;
    firmware = std::move(candidate);
    report = startLoadedFirmware(report, true);
    if (rememberForNewInstances)
    {
        const auto created = firmwarePreferenceFile.getParentDirectory().createDirectory();
        if (created.failed() || !firmwarePreferenceFile.replaceWithText(
                                   firmware.getDirectory().getFullPathName()))
            report.detail += " The firmware loaded, but its folder could not be remembered for new instances.";
    }
    return report;
}

wave::firmware::Bundle::Report WaveEmulationAudioProcessor::startLoadedFirmware(
    const wave::firmware::Bundle::Report& report, bool rememberDirectory)
{
    if (report.hasBothImages())
    {
        // The Wave's own factory routines supersede the PPG fallback and
        // previously remembered PPG imports on every firmware/state reload.
        engine.loadWaveFactoryRom(firmware.getMasterImage());
        sharedFirmwareMemory.clear();
        if (rememberDirectory && firmware.getDirectory().isDirectory())
            parameters.state.setProperty("firmwareDirectory",
                                         firmware.getDirectory().getFullPathName(), nullptr);
        if (masterFirmware.loadAndStart(firmware.getMasterImage()))
        {
            masterFirmware.runCycles(2000000);
            if (report.authenticity == wave::firmware::Bundle::Authenticity::verifiedOs1700)
                masterFirmware.runOs1700InitialisationFileLoad();
        }
        auto allVoiceBoardsLoaded = true;
        for (auto& runtime : voiceFirmwares)
            allVoiceBoardsLoaded = runtime.loadAndReset(firmware.getVoiceImage())
                                   && allVoiceBoardsLoaded;
        if (allVoiceBoardsLoaded)
        {
            for (auto& runtime : voiceFirmwares)
                for (int slice = 0;
                     slice < 20 && !runtime.waitingForMasterAcknowledgement();
                     ++slice)
                    runtime.runCycles(100000);
            const auto allWaiting = std::all_of(
                voiceFirmwares.begin(), voiceFirmwares.end(), [](const auto& runtime) {
                    return runtime.waitingForMasterAcknowledgement();
                });
            if (report.authenticity == wave::firmware::Bundle::Authenticity::verifiedOs1700
                && allWaiting
                && masterFirmware.runOs1700VoiceBoardLoaderHandoff(
                    static_cast<int>(voiceFirmwares.size())))
            {
                for (int slice = 0; slice < 20; ++slice)
                {
                    for (auto& runtime : voiceFirmwares)
                        if (!runtime.reachedServiceLoop())
                            runtime.runCycles(500000);
                    if (std::all_of(voiceFirmwares.begin(), voiceFirmwares.end(),
                                    [](const auto& runtime) {
                                        return runtime.reachedServiceLoop();
                                    }))
                        break;
                }
                const auto allServing = std::all_of(
                    voiceFirmwares.begin(), voiceFirmwares.end(), [](const auto& runtime) {
                        return runtime.reachedServiceLoop();
                    });
                if (allServing)
                {
                    masterFirmware.completeOs1700VoiceBoardServiceHandoff();
                    // Finish the genuine one-time initialisation after
                    // returning from the WDV loader. OS 1.700 immediately
                    // performs a second WDV oscillator-initialisation
                    // handshake, so both CPUs must continue to run here.
                    for (int slice = 0; slice < 40; ++slice)
                    {
                        masterFirmware.runCycles(50000);
                        for (auto& runtime : voiceFirmwares)
                            runtime.runCycles(100000);
                    }
                }
            }
        }
        if (!engine.getWavetableBank().hasOriginalWaveFactoryTables()
            && firmware.getDirectory().isDirectory())
            discoverWavetableRom(firmware.getDirectory());
        seedFilterCalibrationTable();
        if (const auto set = currentPerformanceSet(); set != nullptr)
        {
            engine.loadWaveSetUserTables(set->sourceImage());
            masterFirmware.installSoundBank(set->soundBank());
            masterFirmware.installPerformanceBank(set->performanceBank());
        }
    }
    return report;
}

void WaveEmulationAudioProcessor::loadEmbeddedPrivateRoms()
{
#if WAVE_HAS_PRIVATE_ROMS
    const juce::MemoryBlock master(WaveAssets::w2sys_bin,
                                   static_cast<size_t>(WaveAssets::w2sys_binSize));
    const juce::MemoryBlock voice(WaveAssets::wdv_sys,
                                  static_cast<size_t>(WaveAssets::wdv_sysSize));
    startLoadedFirmware(firmware.loadImages(master, voice), false);
    if (!engine.getWavetableBank().hasOriginalWaveFactoryTables())
        engine.loadWavetableRom(juce::MemoryBlock(
            WaveAssets::ppgwave2_3v6wavetables_rom,
            static_cast<size_t>(WaveAssets::ppgwave2_3v6wavetables_romSize)));
    if (const auto set = currentPerformanceSet())
        engine.loadWaveSetUserTables(set->sourceImage());
#endif
}

void WaveEmulationAudioProcessor::setActivePerformanceSet(
    std::shared_ptr<const wave::presets::WaveFactorySet> set) noexcept
{
    if (set != nullptr && set->isLoaded())
        engine.loadWaveSetUserTables(set->sourceImage());
    std::atomic_store_explicit(&activeFactorySet, std::move(set),
                               std::memory_order_release);
}

void WaveEmulationAudioProcessor::loadEmbeddedFactorySet()
{
#if WAVE_HAS_FACTORY_SET
    auto set = std::make_shared<wave::presets::WaveFactorySet>();
    set->load(WaveAssets::wave_set, static_cast<size_t>(WaveAssets::wave_setSize));
    if (!set->isLoaded())
        return;
    embeddedFactorySet
        = std::static_pointer_cast<const wave::presets::WaveFactorySet>(set);
    setActivePerformanceSet(embeddedFactorySet);

    const auto performance = set->performance(0, 0);
    for (int instrument = 0; instrument < 8; ++instrument)
    {
        const auto offset = static_cast<size_t>(64 + instrument * 32);
        if (performance[offset + 3] != 0 && performance[offset + 13] == 0)
        {
            masterFirmware.setInitialisationRecords(
                set->sound(performance[offset + 1], performance[offset]), performance);
            break;
        }
    }
#endif
}

bool WaveEmulationAudioProcessor::installFactoryEditRecords(int programIndex) noexcept
{
    const auto set = currentPerformanceSet();
    if (set == nullptr || !set->isLoaded() || programIndex < 0
        || programIndex >= wave::presets::WaveFactorySet::programCount)
        return false;

    const auto performance = set->performance(programIndex / 128,
                                              programIndex % 128);
    const auto isActive = [&performance](int instrument) {
        if (instrument < 0 || instrument >= 8)
            return false;
        const auto offset = static_cast<size_t>(64 + instrument * 32);
        return performance[offset + 3] != 0 && performance[offset + 13] == 0;
    };
    auto instrument = juce::jlimit(0, 7, static_cast<int>(performance[24]));
    if (!isActive(instrument))
        for (instrument = 0; instrument < 8 && !isActive(instrument); ++instrument)
        {
        }
    if (instrument >= 8)
    {
        // Empty/quarantined slots still need a native recall. Leaving the
        // previous edit records installed keeps both its LCD and Sound live.
        const std::array<uint8_t, wave::presets::WaveFactorySet::soundSize> emptySound{};
        return masterFirmware.installEditRecords(emptySound, performance);
    }

    const auto offset = static_cast<size_t>(64 + instrument * 32);
    const auto sound = set->sound(performance[offset + 1], performance[offset]);
    return masterFirmware.installEditRecords(sound, performance);
}

void WaveEmulationAudioProcessor::applyFactoryProgram(int index, bool notifyFirmware)
{
    const auto set = currentPerformanceSet();
    if (set == nullptr || !set->isLoaded())
        return;

    suspendFirmwareSoundFeedback();

    const auto bank = index / 128;
    const auto program = index % 128;
    const auto performance = set->performance(bank, program);
    size_t instrumentOffset = 64;
    const auto isActiveInstrument = [&](int candidate) {
        if (candidate < 0 || candidate >= 8)
            return false;
        const auto offset = static_cast<size_t>(64 + candidate * 32);
        return performance[offset + 3] != 0 && performance[offset + 13] == 0;
    };
    auto instrument = juce::jlimit(0, 7, static_cast<int>(performance[24]));
    if (!isActiveInstrument(instrument))
    {
        for (instrument = 0; instrument < 8 && !isActiveInstrument(instrument); ++instrument)
        {
        }
    }
    if (instrument == 8)
    {
        auto emptyPerformance
            = std::make_shared<wave::dsp::WaldorfEngine::PerformanceSnapshot>();
        emptyPerformance->editableLayer = -1;
        auto emptySeed = std::make_shared<PerformanceInstrumentSoundSeed>();
        emptySeed->performance = index;
        std::atomic_store_explicit(
            &factoryPerformance,
            std::static_pointer_cast<const wave::dsp::WaldorfEngine::PerformanceSnapshot>(
                emptyPerformance), std::memory_order_release);
        std::atomic_store_explicit(
            &instrumentSoundSeed,
            std::static_pointer_cast<const PerformanceInstrumentSoundSeed>(emptySeed),
            std::memory_order_release);
        modulationRoutingResetPending.store(true, std::memory_order_release);
        if (notifyFirmware && masterFirmware.isLoaded())
            pendingFirmwareProgram.store(index, std::memory_order_release);
        return;
    }
    instrumentOffset = static_cast<size_t>(64 + instrument * 32);

    const auto soundBank = static_cast<int>(performance[instrumentOffset + 1]);
    const auto soundProgram = static_cast<int>(performance[instrumentOffset]);
    const auto sound = set->sound(soundBank, soundProgram);
    const auto setPlainValue = [this](const char* id, float value) {
        if (auto* parameter = parameters.getParameter(id))
            parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
    };
    for (const auto* id : wave::parameters::quickEdit)
        setPlainValue(id, 0.0f);
    const auto sevenBit = [](uint8_t value) { return static_cast<int>(value & 0x7fu); };
    const auto signedAmount = [&sevenBit](uint8_t value) {
        return static_cast<float>(sevenBit(value) - 64);
    };
    const auto setRoute = [&](wave::parameters::ModulationRouteIndex route,
                              int sourceOffset, int controlOffset, int amountOffset,
                              bool volumeAmount = false) {
        const auto routeIndex = static_cast<size_t>(route);
        const auto sourceIndex = static_cast<size_t>(sourceOffset);
        const auto amountIndex = static_cast<size_t>(amountOffset);
        setPlainValue(wave::parameters::modulationSource[routeIndex],
                      static_cast<float>(juce::jlimit(0, 39, sevenBit(sound[sourceIndex]))));
        setPlainValue(wave::parameters::modulationControl[routeIndex],
                      controlOffset >= 0
                          ? static_cast<float>(juce::jlimit(
                                0, 39, sevenBit(sound[static_cast<size_t>(controlOffset)])))
                          : 38.0f);
        setPlainValue(wave::parameters::modulationAmount[routeIndex],
                      volumeAmount
                          ? static_cast<float>(sevenBit(sound[amountIndex]) / 8 - 8)
                          : signedAmount(sound[amountIndex]));
    };
    const auto envelopeTime = [](uint8_t value) {
        return wave::dsp::WdvEnvelope::timeConstantForRate(value);
    };
    const auto filterFrequency = [](uint8_t value) {
        // WDV's 255-entry frequency table advances in quarter tones; sound
        // cutoff bytes address every second entry, yielding semitone steps.
        return wave::parameters::cutoffFrequencyForStep(static_cast<float>(value));
    };

    const auto waveOneLevel = static_cast<float>(sound[59]) / 112.0f;
    const auto waveTwoLevel = static_cast<float>(sound[60]) / 112.0f;
    const auto oscillatorLevel = waveOneLevel + waveTwoLevel;
    setPlainValue(wave::parameters::waveLevel[0], waveOneLevel);
    setPlainValue(wave::parameters::waveLevel[1], waveTwoLevel);
    setPlainValue(wave::parameters::wavetable,
                  static_cast<float>(static_cast<int>(sound[25] & 0x7fu) + 1));
    setPlainValue(wave::parameters::oscillatorLink,
                  (sound[23] & 0x7fu) == 0 ? 0.0f : 1.0f);
    setPlainValue(wave::parameters::position, static_cast<float>(sound[26]));
    setPlainValue(wave::parameters::scan, static_cast<float>(sound[30]) - 64.0f);
    setPlainValue(wave::parameters::position2, static_cast<float>(sound[42]));
    setPlainValue(wave::parameters::scan2, static_cast<float>(sound[46]) - 64.0f);
    setPlainValue(wave::parameters::oscillatorSemitone[0], signedAmount(sound[1]) / 4.0f);
    setPlainValue(wave::parameters::oscillatorSemitone[1], signedAmount(sound[13]) / 4.0f);
    setPlainValue(wave::parameters::oscillatorDetune[0], signedAmount(sound[2]));
    setPlainValue(wave::parameters::oscillatorDetune[1], signedAmount(sound[14]));
    setPlainValue(wave::parameters::wavePhase[0],
                  static_cast<float>(sevenBit(sound[27])));
    setPlainValue(wave::parameters::wavePhase[1],
                  static_cast<float>(sevenBit(sound[43])));
    setPlainValue(wave::parameters::waveEnvelopeVelocity[0],
                  signedAmount(sound[31]));
    setPlainValue(wave::parameters::waveEnvelopeVelocity[1],
                  signedAmount(sound[47]));
    setPlainValue(wave::parameters::waveKeytrack[0], signedAmount(sound[32]));
    setPlainValue(wave::parameters::waveKeytrack[1], signedAmount(sound[48]));
    for (size_t point = 0; point < wave::parameters::waveEnvelopeTime.size(); ++point)
    {
        setPlainValue(wave::parameters::waveEnvelopeTime[point],
                      static_cast<float>(sound[135 + point * 2]));
        setPlainValue(wave::parameters::waveEnvelopeLevel[point],
                      static_cast<float>(sound[136 + point * 2]));
    }
    setPlainValue(wave::parameters::waveEnvelopeKeyOff,
                  static_cast<float>(juce::jlimit(0, 7, static_cast<int>(sound[155])) + 1));
    setPlainValue(wave::parameters::waveEnvelopeLoopStart,
                  static_cast<float>(juce::jlimit(0, 7, static_cast<int>(sound[156])) + 1));
    setPlainValue(wave::parameters::waveEnvelopeLoopEnabled,
                  sound[157] == 0 ? 0.0f : 1.0f);
    for (size_t lfo = 0; lfo < 2; ++lfo)
    {
        const auto base = static_cast<size_t>(172 + lfo * 10);
        setPlainValue(wave::parameters::lfoRate[lfo],
                      static_cast<float>(sevenBit(sound[base])));
        setPlainValue(wave::parameters::lfoShape[lfo],
                      static_cast<float>(juce::jlimit(0, 5, sevenBit(sound[base + 1]))));
        setPlainValue(wave::parameters::lfoSymmetry[lfo], signedAmount(sound[base + 2]));
        setPlainValue(wave::parameters::lfoHumanize[lfo],
                      static_cast<float>(juce::jlimit(0, 7, sevenBit(sound[base + 3]))));
        setPlainValue(wave::parameters::lfoSync[lfo],
                      static_cast<float>(juce::jlimit(0, 2, sevenBit(sound[base + 9]))));
        const auto phaseOffset = lfo == 0 ? 225u : 232u;
        setPlainValue(wave::parameters::lfoPhase[lfo],
                      static_cast<float>(juce::jlimit(0, 90,
                                                       sevenBit(sound[phaseOffset])) * 4));
    }
    setPlainValue(wave::parameters::glideType,
                  static_cast<float>(juce::jlimit(1, 6, sevenBit(sound[233]))));
    setPlainValue(wave::parameters::glideRate,
                  static_cast<float>(sevenBit(sound[234])));
    setPlainValue(wave::parameters::glideTimeMode,
                  static_cast<float>(juce::jlimit(0, 1, sevenBit(sound[235]))));
    setPlainValue(wave::parameters::glideRateModSource,
                  static_cast<float>(juce::jlimit(0, 39, sevenBit(sound[236]))));
    setPlainValue(wave::parameters::glideRateModAmount,
                  signedAmount(sound[237]));
    setPlainValue(wave::parameters::glideActive,
                  sevenBit(sound[238]) == 0 ? 0.0f : 1.0f);

    setRoute(wave::parameters::osc1PitchMod1, 5, 6, 7);
    setRoute(wave::parameters::osc1PitchMod2, 8, -1, 9);
    setRoute(wave::parameters::osc2PitchMod1, 17, 18, 19);
    setRoute(wave::parameters::osc2PitchMod2, 20, -1, 21);
    setRoute(wave::parameters::wave1StartMod, 28, -1, 29);
    setRoute(wave::parameters::wave1Mod1, 34, 35, 36);
    setRoute(wave::parameters::wave1Mod2, 37, -1, 38);
    setRoute(wave::parameters::wave2StartMod, 44, -1, 45);
    setRoute(wave::parameters::wave2Mod1, 50, 51, 52);
    setRoute(wave::parameters::wave2Mod2, 53, -1, 54);
    setRoute(wave::parameters::wave1VolumeMod, 62, -1, 63, true);
    setRoute(wave::parameters::wave2VolumeMod, 64, -1, 65, true);
    setRoute(wave::parameters::noiseVolumeMod, 66, -1, 67, true);
    setRoute(wave::parameters::amplifierMod1, 72, 73, 74);
    setRoute(wave::parameters::amplifierMod2, 75, -1, 76);
    setRoute(wave::parameters::filterMod1, 85, 86, 87);
    setRoute(wave::parameters::filterMod2, 88, -1, 89);
    setRoute(wave::parameters::resonanceMod, 90, 91, 92);
    setRoute(wave::parameters::highpassMod1, 99, 100, 101);
    setRoute(wave::parameters::highpassMod2, 102, -1, 103);
    setRoute(wave::parameters::panMod1, 194, 195, 196);
    setRoute(wave::parameters::panMod2, 197, -1, 198);
    setRoute(wave::parameters::lfo1RateMod, 176, -1, 177);
    setRoute(wave::parameters::lfo1LevelMod, 178, 179, 180);
    setRoute(wave::parameters::lfo2RateMod, 186, -1, 187);
    setRoute(wave::parameters::lfo2LevelMod, 188, 189, 190);
    setPlainValue(wave::parameters::balance,
                  oscillatorLevel > 0.0f ? waveTwoLevel / oscillatorLevel : 0.5f);
    setPlainValue(wave::parameters::detune,
                  juce::jlimit(-50.0f, 50.0f,
                               static_cast<float>(sound[14])
                                   - static_cast<float>(sound[2])));
    setPlainValue(wave::parameters::noise,
                  juce::jlimit(0.0f, 1.0f, static_cast<float>(sound[61]) / 112.0f));
    setPlainValue(wave::parameters::cutoff, filterFrequency(sound[79]));
    setPlainValue(wave::parameters::filterMode,
                  static_cast<float>(juce::jlimit(0, 3, sevenBit(sound[78]))));
    setPlainValue(wave::parameters::resonance,
                  static_cast<float>(sound[80]) / 127.0f);
    setPlainValue(wave::parameters::filterEnv,
                  static_cast<float>(sound[81]) - 64.0f);
    setPlainValue(wave::parameters::filterVelocity,
                  static_cast<float>(sound[82]) - 64.0f);
    setPlainValue(wave::parameters::filterKeytrack,
                  static_cast<float>(sound[83]) - 64.0f);
    setPlainValue(wave::parameters::filterKeyCenter,
                  static_cast<float>(sound[84]));
    setPlainValue(
        wave::parameters::attack,
        wave::dsp::WdvEnvelope::amplifierAttackTimeConstantForRate(sound[106]));
    setPlainValue(wave::parameters::decay, envelopeTime(sound[107]));
    setPlainValue(wave::parameters::sustain, static_cast<float>(sound[108]) / 127.0f);
    setPlainValue(wave::parameters::release, envelopeTime(sound[109]));
    setPlainValue(wave::parameters::filterDelay,
                  sound[119] == 0 ? 0.0f : envelopeTime(sound[119]));
    setPlainValue(wave::parameters::filterAttack, envelopeTime(sound[120]));
    setPlainValue(wave::parameters::filterDecay, envelopeTime(sound[121]));
    setPlainValue(wave::parameters::filterSustain,
                  static_cast<float>(sound[122]) / 127.0f);
    setPlainValue(wave::parameters::filterRelease, envelopeTime(sound[123]));
    for (size_t stage = 0; stage < wave::parameters::filterEnvelopeModSource.size(); ++stage)
    {
        setPlainValue(wave::parameters::filterEnvelopeModSource[stage],
                      static_cast<float>(juce::jlimit(
                          0, 39, sevenBit(sound[126 + stage * 2]))));
        setPlainValue(wave::parameters::filterEnvelopeModAmount[stage],
                      signedAmount(sound[127 + stage * 2]));
    }
    setPlainValue(wave::parameters::highpassCutoff, filterFrequency(sound[93]));
    setPlainValue(wave::parameters::highpassEnvelopeSelect,
                  static_cast<float>(juce::jlimit(0, 3, sevenBit(sound[94]))));
    setPlainValue(wave::parameters::highpassEnvelopeAmount, signedAmount(sound[95]));
    setPlainValue(wave::parameters::highpassVelocity, signedAmount(sound[96]));
    setPlainValue(wave::parameters::highpassKeytrack, signedAmount(sound[97]));
    setPlainValue(wave::parameters::highpassKeyCenter,
                  static_cast<float>(sevenBit(sound[98])));
    setPlainValue(wave::parameters::bandpassBandwidth,
                  static_cast<float>(sevenBit(sound[104])));
    for (size_t point = 0; point < wave::parameters::freeEnvelopeTime.size(); ++point)
    {
        setPlainValue(wave::parameters::freeEnvelopeTime[point],
                      static_cast<float>(sevenBit(sound[159 + point * 2])));
        setPlainValue(wave::parameters::freeEnvelopeLevel[point],
                      signedAmount(sound[160 + point * 2]));
    }
    setPlainValue(wave::parameters::freeEnvelopeZeroAxis, signedAmount(sound[171]));

    auto decodedPerformance = std::make_shared<wave::dsp::WaldorfEngine::PerformanceSnapshot>();
    auto soundSeed = std::make_shared<PerformanceInstrumentSoundSeed>();
    soundSeed->performance = index;
    decodedPerformance->editableLayer = instrument;
    decodedPerformance->controlXController = juce::jlimit(
        0, 120, sevenBit(performance[28]));
    decodedPerformance->controlYController = juce::jlimit(
        0, 120, sevenBit(performance[29]));
    auto activeLayerCount = 0;
    for (int layer = 0; layer < 8; ++layer)
    {
        const auto layerOffset = static_cast<size_t>(64 + layer * 32);
        if (!isActiveInstrument(layer))
            continue;

        auto& destination = decodedPerformance->layers[static_cast<size_t>(layer)];
        destination.enabled = true;
        destination.source = sevenBit(performance[layerOffset + 3]);
        destination.midiChannel = juce::jlimit(
            0, 16, sevenBit(performance[layerOffset + 2]));
        destination.transposeSemitones = sevenBit(performance[layerOffset + 9]) - 64;
        destination.detuneCents = static_cast<float>(sevenBit(performance[layerOffset + 10]) - 64);
        destination.gain = static_cast<float>(sevenBit(performance[layerOffset + 4])) / 127.0f;
        destination.auxGain
            = static_cast<float>(sevenBit(performance[layerOffset + 7])) / 127.0f;
        destination.audioOutput
            = juce::jlimit(0, 3, sevenBit(performance[layerOffset + 12]));
        destination.keyLow = sevenBit(performance[layerOffset + 17]);
        destination.keyHigh = sevenBit(performance[layerOffset + 18]);
        destination.velocityLow = juce::jmax(1, sevenBit(performance[layerOffset + 19]));
        destination.velocityHigh = juce::jmax(1, sevenBit(performance[layerOffset + 20]));
        destination.velocityTable = juce::jlimit(
            0, 11, sevenBit(performance[layerOffset + 21]));
        destination.tuningTable = juce::jlimit(
            0, 12, sevenBit(performance[layerOffset + 22]));
        destination.allocationMode = juce::jlimit(
            0, 22, sevenBit(performance[layerOffset + 15]));
        const auto nativeSound = set->sound(
            sevenBit(performance[layerOffset + 1]),
            sevenBit(performance[layerOffset]));
        destination.sound = decodeFactorySound(nativeSound);
        std::copy(nativeSound.begin(), nativeSound.end(),
                  soundSeed->records[static_cast<size_t>(layer)].begin());
        soundSeed->valid[static_cast<size_t>(layer)] = true;
        destination.sound.panAmount = juce::jlimit(
            -1.0f, 1.0f,
            static_cast<float>(sevenBit(performance[layerOffset + 5]) - 64) / 64.0f);
        destination.sound.panModulationMode = juce::jlimit(
            0, 2, sevenBit(performance[layerOffset + 6]));
        ++activeLayerCount;
    }
    if (activeLayerCount == 0)
        return;

    setPlainValue(wave::parameters::pan,
                  juce::jlimit(-1.0f, 1.0f,
                               static_cast<float>(sevenBit(performance[instrumentOffset + 5]) - 64)
                                   / 64.0f));
    setPlainValue(wave::parameters::panMode,
                  static_cast<float>(juce::jlimit(
                      0, 2, static_cast<int>(performance[instrumentOffset + 6]))));
    const auto performanceGain = juce::jmax(
        0.001f, static_cast<float>(sevenBit(performance[0])) / 127.0f);
    decodedPerformance->outputDb = juce::jlimit(
        -30.0f, 6.0f, juce::Decibels::gainToDecibels(performanceGain) - 3.0f);
    decodedPerformance->circuitAgeAmount
        = wave::parameters::readSnapshot(parameters).circuitAgeAmount;
    setPlainValue(wave::parameters::output,
                  decodedPerformance->outputDb);
    if (instrument >= 0 && instrument < 8
        && decodedPerformance->layers[static_cast<size_t>(instrument)].enabled)
    {
        for (size_t oscillator = 0; oscillator < panelOscillatorOctaves.size();
             ++oscillator)
            panelOscillatorOctaves[oscillator].store(
                decodedPerformance->layers[static_cast<size_t>(instrument)]
                    .sound.oscillatorOctaves[oscillator],
                std::memory_order_release);
    }
    std::atomic_store_explicit(
        &factoryPerformance,
        std::static_pointer_cast<const wave::dsp::WaldorfEngine::PerformanceSnapshot>(
            decodedPerformance),
        std::memory_order_release);
    std::atomic_store_explicit(
        &instrumentSoundSeed,
        std::static_pointer_cast<const PerformanceInstrumentSoundSeed>(soundSeed),
        std::memory_order_release);

    // Keep the physical potentiometer voltages exactly where the user left
    // them. Program recall changes the stored Sound, not the hardware knobs.
    // Once OS 1.700 reaches this Performance/Instrument, the routing handshake
    // copies the recalled selector state into that live edit record.
    modulationRoutingResetPending.store(true, std::memory_order_release);

    if (notifyFirmware && masterFirmware.isLoaded())
        pendingFirmwareProgram.store(index, std::memory_order_release);
}

bool WaveEmulationAudioProcessor::loadWavetableRom(const juce::File& image)
{
    juce::MemoryBlock data;
    if (!image.existsAsFile() || !image.loadFileAsData(data)
        || !engine.loadWavetableRom(data))
        return false;

    if (const auto set = currentPerformanceSet())
        engine.loadWaveSetUserTables(set->sourceImage());

    parameters.state.setProperty("wavetableRomFile", image.getFullPathName(), nullptr);
    return true;
}

void WaveEmulationAudioProcessor::discoverWavetableRom(const juce::File& directory)
{
    const auto savedPath = parameters.state.getProperty("wavetableRomFile").toString();
    if (savedPath.isNotEmpty() && loadWavetableRom(juce::File(savedPath)))
        return;

    juce::Array<juce::File> candidates;
    directory.findChildFiles(candidates, juce::File::findFiles, true, "*.bin;*.rom");
    std::sort(candidates.begin(), candidates.end(), [](const auto& left, const auto& right) {
        const auto priority = [](const juce::File& file) {
            const auto name = file.getFileNameWithoutExtension().toLowerCase();
            return (name.contains("ppg") || name.contains("wavetable")) ? 0 : 1;
        };
        const auto leftPriority = priority(left);
        const auto rightPriority = priority(right);
        return leftPriority != rightPriority
                   ? leftPriority < rightPriority
                   : left.getFullPathName() < right.getFullPathName();
    });

    for (const auto& candidate : candidates)
        if (candidate.getFileName() != "w2sys.bin" && candidate.getFileName() != "wdv.sys"
            && loadWavetableRom(candidate))
            return;
}

const wave::firmware::Bundle::Report& WaveEmulationAudioProcessor::getFirmwareReport() const noexcept
{
    return firmware.getReport();
}

juce::File WaveEmulationAudioProcessor::getFirmwareDirectory() const noexcept
{
    return firmware.getDirectory();
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new WaveEmulationAudioProcessor();
}

// UI artwork is a per-user preference, independent of firmware and host audio state.
juce::File WaveEmulationAudioProcessor::getRememberedPanelSkin() const
{
    const auto preference = firmwarePreferenceFile.getSiblingFile(
        firmwarePreferenceFile.getFileNameWithoutExtension() + "-panel-skin.txt");
    const auto path = preference.loadFileAsString().trim();
    return juce::File::isAbsolutePath(path) ? juce::File(path) : juce::File{};
}

juce::Result WaveEmulationAudioProcessor::rememberPanelSkin(const juce::File& file)
{
    const auto preference = firmwarePreferenceFile.getSiblingFile(
        firmwarePreferenceFile.getFileNameWithoutExtension() + "-panel-skin.txt");
    const auto created = preference.getParentDirectory().createDirectory();
    if (created.failed())
        return created;
    if (!preference.replaceWithText(file == juce::File{} ? juce::String{} : file.getFullPathName()))
        return juce::Result::fail("Could not save the panel skin preference.");
    return juce::Result::ok();
}

bool WaveEmulationAudioProcessor::getRememberedTabbedLayout() const
{
    return firmwarePreferenceFile.getSiblingFile(
               firmwarePreferenceFile.getFileNameWithoutExtension() + "-layout.txt")
               .loadFileAsString().trim() == "tabbed";
}

void WaveEmulationAudioProcessor::rememberTabbedLayout(bool tabbed)
{
    const auto preference = firmwarePreferenceFile.getSiblingFile(
        firmwarePreferenceFile.getFileNameWithoutExtension() + "-layout.txt");
    if (preference.getParentDirectory().createDirectory().wasOk())
        preference.replaceWithText(tabbed ? "tabbed" : "classic");
}
