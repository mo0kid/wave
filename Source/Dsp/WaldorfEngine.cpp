#include "WaldorfEngine.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace wave::dsp
{
namespace
{
// WDV OS 1.700 stores modulation amounts in a 128-word table at 0x25e8.
// Apart from the saturated -64 endpoint and three typos, its relationship is
// sign(amount) * amount^2 / 4096 (8 * n^2 of 32768). The typos are index 6
// (amount -58: -29912 instead of -26912), index 126 (+62: 31000 instead of
// 30752) and index 127 (+63: 32767 instead of 31752). The pitch routine
// applies the result squared, then both oscillator callers at $0e14/$0eca
// arithmetic-shift it right by three before adding it to the pitch
// accumulator.
float wdvAmountDepth(float amount) noexcept
{
    const auto limited = juce::jlimit(-64.0f, 63.0f, amount);
    static constexpr std::array<std::pair<float, float>, 3> tableTypos {{
        { -58.0f, -3000.0f }, { 62.0f, 248.0f }, { 63.0f, 1015.0f }
    }};
    auto depth = std::copysign(limited * limited / 4096.0f, limited);
    for (const auto& [index, error] : tableTypos)
        depth += error / 32768.0f * juce::jmax(0.0f, 1.0f - std::abs(limited - index));
    return depth;
}

float wdvPitchDepthSemitones(float amount) noexcept
{
    const auto firstPass = wdvAmountDepth(amount);
    return 16.0f * firstPass * std::abs(firstPass);
}

float wdvVolumeDepth(float amount) noexcept
{
    // Mixer amounts are stored in steps of eight (-7...+7 in the UI).
    const auto limited = juce::jlimit(-7.0f, 7.0f, amount);
    return std::copysign(limited * limited / 64.0f, limited);
}

float wdvEnvelopeCutoffSteps(float amount, float velocityAmount,
                             float velocity, float envelope) noexcept
{
    // WDV.SYS 1.700 $1356-$1388 forms a signed Q7.8 depth directly from
    // Filter Env and Filter Velocity, clamps their sum, then $14B8-$14CE
    // multiplies it by the live envelope and doubles the result. Unlike the
    // general modulation routes, these two fields do not use the quadratic
    // amount table at $25E8.
    const auto depth = juce::jlimit(
        -64.0f, 32767.0f / 512.0f,
        juce::jlimit(-64.0f, 63.0f, amount)
            + juce::jlimit(-64.0f, 63.0f, velocityAmount)
                  * juce::jlimit(0.0f, 1.0f, velocity));
    return 2.0f * depth * envelope;
}

float wdvFilterKeytrackSteps(float amount, int noteDelta) noexcept
{
    // WDV.SYS $13CE-$13EA multiplies the note displacement in Q7.8 by the
    // signed Keytrack byte and shifts five places: maximum negative/positive
    // settings are approximately -200/+197 percent keyboard tracking.
    return juce::jlimit(-64.0f, 63.0f, amount)
           * static_cast<float>(noteDelta) / 32.0f;
}

float performanceVelocity(float velocity, int table) noexcept
{
    velocity = juce::jlimit(0.0f, 1.0f, velocity);
    switch (juce::jlimit(0, 11, table))
    {
        case 2: return 1.0f - velocity;                         // Linear -
        case 3: return velocity * velocity;                     // Exponential +
        case 4: return (1.0f - velocity) * (1.0f - velocity);   // Exponential -
        case 5: return std::sin(velocity * juce::MathConstants<float>::pi * 0.5f);
        case 6: return std::cos(velocity * juce::MathConstants<float>::pi * 0.5f);
        case 7: return 1.0f;                                    // Full
        default: return velocity;                               // Global/linear/user fallback
    }
}

float signedHash(uint64_t seed) noexcept
{
    // Stable note-on randomness: the hardware chooses a new error on each
    // key strike, while an audio-block redraw must not retune an existing
    // voice. SplitMix64 gives that behaviour without shared mutable state.
    seed += 0x9e3779b97f4a7c15ull;
    seed = (seed ^ (seed >> 30u)) * 0xbf58476d1ce4e5b9ull;
    seed = (seed ^ (seed >> 27u)) * 0x94d049bb133111ebull;
    seed ^= seed >> 31u;
    const auto unit = static_cast<float>(seed & 0x00ffffffu)
                      / static_cast<float>(0x00ffffffu);
    return unit * 2.0f - 1.0f;
}
} // namespace

class WaldorfEngine::VoiceCardWorker final : public juce::Thread
{
public:
    VoiceCardWorker(WaldorfEngine& engineToUse, int boardToRender)
        : juce::Thread("Wave voice card " + juce::String(boardToRender + 1)),
          engine(engineToUse), board(boardToRender)
    {
    }

    ~VoiceCardWorker() override
    {
        shutdown();
    }

    bool start(double sampleRate, int maximumBlockSize)
    {
        const auto periodMs = 1000.0 * static_cast<double>(maximumBlockSize)
                              / juce::jmax(1.0, sampleRate);
        const auto options = juce::Thread::RealtimeOptions{}
                                 .withPriority(9)
                                 .withPeriodMs(periodMs)
                                 .withProcessingTimeMs(periodMs * 0.42)
                                 .withMaximumProcessingTimeMs(periodMs * 0.9);
        if (startRealtimeThread(options))
            return true;
        return startThread(juce::Thread::Priority::high);
    }

    uint32_t dispatch(int samples) noexcept
    {
        jobSamples = samples;
        const auto generation
            = requestedGeneration.fetch_add(1, std::memory_order_release) + 1;
        requestedGeneration.notify_one();
        return generation;
    }

    void waitFor(uint32_t generation) const noexcept
    {
        auto completed = completedGeneration.load(std::memory_order_acquire);
        while (completed != generation)
        {
            completedGeneration.wait(completed, std::memory_order_acquire);
            completed = completedGeneration.load(std::memory_order_acquire);
        }
    }

    void shutdown() noexcept
    {
        if (!isThreadRunning())
            return;
        signalThreadShouldExit();
        requestedGeneration.fetch_add(1, std::memory_order_release);
        requestedGeneration.notify_one();
        stopThread(5000);
    }

    void run() override
    {
        juce::WorkgroupToken workgroupToken;
        uint32_t handledGeneration = 0;
        for (;;)
        {
            auto requested = requestedGeneration.load(std::memory_order_acquire);
            while (requested == handledGeneration && !threadShouldExit())
            {
                requestedGeneration.wait(requested, std::memory_order_acquire);
                requested = requestedGeneration.load(std::memory_order_acquire);
            }
            if (threadShouldExit())
                return;

            // The dispatch/completion barriers also protect the host workgroup.
            // Join and leave on this worker; rejoining the same group is a no-op.
            engine.audioWorkgroup.join(workgroupToken);
            engine.renderVoiceCard(board, jobSamples);
            handledGeneration = requested;
            completedGeneration.store(handledGeneration, std::memory_order_release);
            completedGeneration.notify_one();
        }
    }

private:
    WaldorfEngine& engine;
    const int board;
    int jobSamples = 0;
    std::atomic<uint32_t> requestedGeneration { 0 };
    mutable std::atomic<uint32_t> completedGeneration { 0 };
};

WaldorfEngine::WaldorfEngine()
{
    // The bipolar Free wheel starts at its centre marker on a cold boot.
    midiControllerAmounts[16] = 64.0f / 127.0f;
    userTuningBank = std::make_shared<const UserTuningBank>();
}

WaldorfEngine::~WaldorfEngine()
{
    stopVoiceCardWorkers();
}

void WaldorfEngine::prepare(double sampleRate, int maximumBlockSize)
{
    stopVoiceCardWorkers();
    maximumRenderBlockSize = juce::jmax(1, maximumBlockSize);
    const auto scratchSamples = static_cast<size_t>(voiceCount)
                                * static_cast<size_t>(maximumRenderBlockSize);
    voiceLeftScratch.assign(scratchSamples, 0.0f);
    voiceRightScratch.assign(scratchSamples, 0.0f);
    for (int i = 0; i < voiceCount; ++i)
        voices[static_cast<size_t>(i)].prepare(sampleRate, i);
    outputStage.prepare(sampleRate);
    noteOrder = 0;
    triggerOrder = 0;
    parallelCardRenders = 0;
    lastPlayedNotesByLayer.fill(-1);
    layerGlideTrajectories.fill({});
    startVoiceCardWorkers(sampleRate, maximumRenderBlockSize);
}

void WaldorfEngine::reset()
{
    for (auto& voice : voices)
        voice.reset();
    outputStage.reset();
    for (auto& card : firmwareAsicRegisters)
        card.fill(0);
    for (auto& card : firmwareCvRegisters)
        for (auto& bank : card)
            bank.fill(0);
    appliedFirmwareWrites = 0;
    noteOrder = 0;
    triggerOrder = 0;
    pitchBendSemitones = 0.0f;
    modWheelAmount = 0.0f;
    channelPressureAmount = 0.0f;
    midiControllerAmounts.fill(0.0f);
    midiControllerAmounts[16] = 64.0f / 127.0f;
    lastPlayedNotesByLayer.fill(-1);
    layerGlideTrajectories.fill({});
    sustainPedal = false;
}

float WaldorfEngine::tunedNoteForLayer(const PerformanceLayer& layer,
                                       int translatedNote, uint64_t seed,
                                       int layerIndex) const noexcept
{
    const auto key = juce::jlimit(0, 127, translatedNote);
    auto selection = juce::jlimit(0, 12, layer.tuningTable);

    // The Wave global parameter selects User table 1..4. A cold machine uses
    // table 1; all four are equal-tempered in the supplied factory SET.
    if (selection == 0)
        selection = 8;

    if (selection == 1 || selection == 12)
        return static_cast<float>(key); // Linear +, or an unchanged MIDI TT.

    if (selection == 2)
    {
        // The Wave's HMT is a real-time just-intonation system. Anchor the
        // current chord to its lowest held key and use the conventional just
        // ratios for the other pitch classes. Re-evaluation at every block
        // makes held voices follow a newly established chord root.
        auto root = key;
        for (const auto& voice : voices)
            if (voice.active && voice.keyDown && voice.layerIndex == layerIndex)
                root = juce::jmin(root, voice.note);

        static constexpr std::array<float, 12> justCents {
            0.0f, 111.731f, 203.910f, 315.641f, 386.314f, 498.045f,
            590.224f, 701.955f, 813.686f, 884.359f, 1017.596f, 1088.269f
        };
        const auto distance = key - root;
        const auto octave = distance >= 0 ? distance / 12 : (distance - 11) / 12;
        const auto pitchClass = distance - octave * 12;
        return static_cast<float>(root + octave * 12)
               + justCents[static_cast<size_t>(pitchClass)] / 100.0f;
    }

    if (selection == 3)
    {
        // OS 1.700 describes linear- as equal temperament mirrored around
        // MIDI note 64 (the E above middle C).
        return 128.0f - static_cast<float>(key);
    }

    if (selection >= 4 && selection <= 7)
    {
        // Random 1 is intentionally minute; Random 4 reaches the conspicuous
        // "drunken violinist" range described by the Wave manual.
        static constexpr std::array<float, 4> maximumCents {
            1.5f, 4.0f, 12.0f, 30.0f
        };
        const auto randomSeed = seed
                                ^ (static_cast<uint64_t>(key) << 24u)
                                ^ (static_cast<uint64_t>(layerIndex + 1) << 48u);
        return static_cast<float>(key)
               + signedHash(randomSeed)
                     * maximumCents[static_cast<size_t>(selection - 4)] / 100.0f;
    }

    if (selection >= 8 && selection <= 11)
    {
        const auto bank = std::atomic_load_explicit(
            &userTuningBank, std::memory_order_acquire);
        if (bank != nullptr)
            return static_cast<float>(key)
                   + (*bank)[static_cast<size_t>(selection - 8)]
                            [static_cast<size_t>(key)] / 100.0f;
    }

    return static_cast<float>(key);
}

void WaldorfEngine::synchroniseActiveVoiceTuning(
    const PerformanceSnapshot& performance) noexcept
{
    for (size_t layerIndex = 0; layerIndex < layerGlideTrajectories.size(); ++layerIndex)
    {
        auto& trajectory = layerGlideTrajectories[layerIndex];
        if (!trajectory.initialised || lastPlayedNotesByLayer[layerIndex] < 0)
            continue;
        const auto desiredTarget = tunedNoteForLayer(
            performance.layers[layerIndex], lastPlayedNotesByLayer[layerIndex],
            trajectory.triggerId, static_cast<int>(layerIndex));
        const auto tuningDelta = desiredTarget - trajectory.targetNote;
        if (std::abs(tuningDelta) > 1.0e-6f)
        {
            // Portamento is owned by the layer. Retuning only its active
            // voices leaves the next note gliding from the old temperament.
            trajectory.currentNote += tuningDelta;
            trajectory.targetNote = desiredTarget;
        }
    }

    for (auto& voice : voices)
    {
        if (!voice.active || voice.layerIndex < 0 || voice.layerIndex >= 8)
            continue;
        const auto& layer = performance.layers[static_cast<size_t>(voice.layerIndex)];
        const auto selection = juce::jlimit(0, 12, layer.tuningTable);
        if (selection == voice.tuningTableSelection && selection != 2)
        {
            voice.updatePitch(layer.sound, pitchBendSemitones);
            continue;
        }
        const auto desiredTarget = tunedNoteForLayer(
            layer, voice.note, voice.triggerId, voice.layerIndex);
        const auto tuningDelta = desiredTarget - voice.targetGlideNote;
        if (std::abs(tuningDelta) > 1.0e-6f)
        {
            // A table edit changes pitch, not the remaining portamento time.
            // Translate the whole live trajectory so continuous glide does
            // not restart or jump back to its source note.
            voice.currentGlideNote += tuningDelta;
            voice.targetGlideNote = desiredTarget;
        }
        voice.tuningTableSelection = selection;
        voice.updatePitch(layer.sound, pitchBendSemitones);
    }
}

void WaldorfEngine::render(juce::AudioBuffer<float>& output, const juce::MidiBuffer& midi,
                           const parameters::Snapshot& parameters)
{
    const juce::MidiBuffer noLocalKeyboardMidi;
    render(output, midi, noLocalKeyboardMidi, parameters);
}

void WaldorfEngine::render(juce::AudioBuffer<float>& output, const juce::MidiBuffer& midi,
                           const juce::MidiBuffer& localKeyboardMidi,
                           const parameters::Snapshot& parameters)
{
    PerformanceSnapshot performance;
    performance.layers[0].enabled = true;
    performance.layers[0].source = 3;
    performance.layers[0].sound = parameters;
    performance.outputDb = parameters.outputDb;
    performance.circuitAgeAmount = parameters.circuitAgeAmount;
    render(output, midi, localKeyboardMidi, performance);
}

void WaldorfEngine::render(juce::AudioBuffer<float>& output, const juce::MidiBuffer& midi,
                           const PerformanceSnapshot& performance)
{
    const juce::MidiBuffer noLocalKeyboardMidi;
    render(output, midi, noLocalKeyboardMidi, performance);
}

void WaldorfEngine::render(juce::AudioBuffer<float>& output, const juce::MidiBuffer& midi,
                           const juce::MidiBuffer& localKeyboardMidi,
                           const PerformanceSnapshot& performance)
{
    output.clear();
    synchroniseActiveVoiceTuning(performance);
    auto cursor = 0;
    const auto sampleCount = output.getNumSamples();

    auto midiIterator = midi.begin();
    auto keyboardIterator = localKeyboardMidi.begin();
    while (midiIterator != midi.end() || keyboardIterator != localKeyboardMidi.end())
    {
        const auto useLocalKeyboard
            = midiIterator == midi.end()
              || (keyboardIterator != localKeyboardMidi.end()
                  && (*keyboardIterator).samplePosition <= (*midiIterator).samplePosition);
        const auto metadata = useLocalKeyboard ? *keyboardIterator++ : *midiIterator++;
        const auto eventSample = juce::jlimit(0, sampleCount, metadata.samplePosition);
        renderRange(output, cursor, eventSample, performance);
        handleMidi(metadata.getMessage(), performance, useLocalKeyboard);
        cursor = eventSample;
    }

    renderRange(output, cursor, sampleCount, performance);
}

void WaldorfEngine::Voice::prepare(double newSampleRate, int index)
{
    sampleRate = juce::jmax(1.0, newSampleRate);
    // A firmware Cutoff edit advances in semitone-sized Sound-record codes.
    // Reconstruct the continuous base control voltage between those writes in
    // pitch space, while leaving envelopes and modulation on the WDV control
    // path untouched. This removes zippering without bypassing OS Knob Mode.
    constexpr auto baseCutoffGlideSeconds = 0.018f;
    baseCutoffSlew = 1.0f - std::exp(
        -1.0f / (static_cast<float>(sampleRate) * baseCutoffGlideSeconds));
    voiceIndex = index;
    // Component ageing remains disabled here: the installed Wave's real
    // per-voice correction comes from its 48-word VCF service calibration.
    tolerance = 0.0f;
    noiseState = 0x9e3779b9u ^ (static_cast<uint32_t>(index) * 0x45d9f3bu);
    oscillator1.prepare(sampleRate);
    oscillator2.prepare(sampleRate);
    // Only the digital ASIC/DAC/reconstruction boundary runs at the original
    // 250 kHz domain. The substantially more expensive CEM voice circuit
    // remains at its own existing 2x-host integration rate.
    highpassFilter.prepare(OscillatorChipProxy::modelClockRate());
    reconstruction.prepare(OscillatorChipProxy::modelClockRate(), tolerance);
    asicResampler.prepare(sampleRate);
    circuit.prepare(sampleRate, tolerance);
    // Reinstate the installed card trim before the first note. The firmware's
    // $15A680 service table remains authoritative once the CPU is running.
    circuit.setControls(1000.0f, 0.0f, 0.0f, 0.0f, 0.18f);
    circuit.setCutoffCalibrationCode(
        installedFilterCalibrationCodes[static_cast<size_t>(index)]);
    envelope.prepare(sampleRate);
    filterEnvelope.prepare(sampleRate);
    waveEnvelope.prepare(sampleRate);
    freeEnvelope.prepare(sampleRate);
    lfos[0].prepare(sampleRate, 0x6d2b79f5u ^ static_cast<uint32_t>(index * 0x9e37));
    lfos[1].prepare(sampleRate, 0xa511e9b3u ^ static_cast<uint32_t>(index * 0x45d9));
    reset();
}

void WaldorfEngine::Voice::reset()
{
    oscillator1.reset();
    oscillator2.reset();
    highpassFilter.reset();
    reconstruction.reset();
    asicResampler.reset();
    circuit.reset();
    envelope.reset();
    filterEnvelope.reset();
    waveEnvelope.reset();
    freeEnvelope.reset();
    for (auto& lfo : lfos)
        lfo.reset();
    note = -1;
    triggerNote = -1;
    triggerChannel = 1;
    triggerId = 0;
    layerIndex = 0;
    velocity = 0.0f;
    layerGain = 1.0f;
    performanceDetuneCents = 0.0f;
    tuningTableSelection = 0;
    keyDown = false;
    active = false;
    currentWavePosition = 0.0f;
    currentLfoValues.fill(0.0f);
    waveStartOffsets.fill(0.0f);
    currentPitchModulations.fill(0.0f);
    currentGlideNote = 0.0f;
    targetGlideNote = 0.0f;
    glideStepPerSample = 0.0f;
    glideSamplesRemaining = 0;
    glideQuantised = false;
    currentAmplifierEnvelope = 0.0f;
    currentFilterEnvelope = 0.0f;
    currentFreeEnvelope = 0.0f;
    currentControlSampleAndHold = 0.0f;
    currentComparatorPositive = 0.0f;
    currentComparatorNegative = 1.0f;
    currentControlX = 0.0f;
    currentControlY = 0.0f;
    controlLfoRates.fill(0.0f);
    controlLfoLevels.fill(1.0f);
    controlWavePositions.fill(0.0f);
    controlWaveLevels.fill(0.5f);
    controlNoiseLevel = 0.0f;
    smoothedBaseCutoffSemitones = 0.0f;
    controlAnalogueCutoff = 8200.0f;
    controlHighpassCutoff = 20.0f;
    controlResonance = 0.18f;
    controlPan = 0.0f;
    controlAmplifierMod = 0.0f;
    samplesUntilControlUpdate = 0.0;
    controlSampleAndHoldPhase = 0.0;
    controlSampleAndHoldInitialised = false;
    baseCutoffInitialised = false;
    filterEnvelopeDelaySamples = 0;
    vcaDrainSamplesRemaining = 0;
    controlFilterMode = 0;
    asicClockPhase = 1.0;
    waveStartOffsetsPending = false;
    filterEnvelopePending = false;
    filterEnvelopeTriggered = false;
}

void WaldorfEngine::Voice::start(int midiNote, int midiChannel, float noteVelocity,
                                 uint64_t order, uint64_t newTriggerId, int newLayerIndex,
                                 const PerformanceLayer& layer, float tunedTargetNote,
                                 float glideFromNote, float glideRate,
                                 bool glideDistanceMode,
                                 bool shouldQuantiseGlide,
                                 float inheritedGlideStepPerSample,
                                 int inheritedGlideSamplesRemaining, float modWheel,
                                 float channelPressure, float pitchBend)
{
    const auto isStealingActiveVoice = active;
    const auto& parameters = layer.sound;
    triggerNote = midiNote;
    triggerChannel = midiChannel;
    note = juce::jlimit(0, 127, midiNote + layer.transposeSemitones);
    layerIndex = newLayerIndex;
    velocity = juce::jlimit(0.0f, 1.0f, noteVelocity);
    layerGain = juce::jmax(0.0f, layer.gain);
    performanceDetuneCents = layer.detuneCents;
    tuningTableSelection = juce::jlimit(0, 12, layer.tuningTable);
    startOrder = order;
    triggerId = newTriggerId;
    active = true;
    keyDown = true;
    vcaDrainSamplesRemaining = 0;
    samplesUntilControlUpdate = 0.0;
    targetGlideNote = tunedTargetNote;
    currentGlideNote = glideFromNote;
    glideQuantised = shouldQuantiseGlide;
    if (inheritedGlideSamplesRemaining > 0
        && std::abs(targetGlideNote - currentGlideNote) > 1.0e-5f)
    {
        // A repeated destination note on the hardware does not restart its
        // portamento timer. The newly allocated voice picks up the live pitch
        // accumulator and the remainder of the existing trajectory.
        glideStepPerSample = inheritedGlideStepPerSample;
        glideSamplesRemaining = inheritedGlideSamplesRemaining;
    }
    else
    {
        const auto rateModulation = sourceValue(
            parameters.glideRateModulationSource, 0.0f, 0.0f,
            currentLfoValues, modWheel, channelPressure, pitchBend);
        const auto effectiveRate = juce::jlimit(
            0.0f, 127.0f,
            glideRate
                + 128.0f * wdvAmountDepth(parameters.glideRateModulationAmount)
                      * rateModulation);
        auto glideSeconds = static_cast<float>(
            WaveEnvelope::segmentTimeSeconds(effectiveRate));
        if (glideDistanceMode)
            glideSeconds *= std::abs(targetGlideNote - currentGlideNote) / 12.0f;
        glideSamplesRemaining = juce::roundToInt(
            glideSeconds * static_cast<float>(sampleRate));
        if (glideSamplesRemaining > 0
            && std::abs(targetGlideNote - currentGlideNote) > 1.0e-5f)
            glideStepPerSample
                = (targetGlideNote - currentGlideNote)
                  / static_cast<float>(glideSamplesRemaining);
        else
        {
            currentGlideNote = targetGlideNote;
            glideStepPerSample = 0.0f;
            glideSamplesRemaining = 0;
        }
    }
    updatePitch(parameters, pitchBend);

    const auto startPhase = [&](size_t oscillator) {
        const auto programmed = parameters.wavePhases[oscillator];
        if (programmed > 0.0f)
            return static_cast<double>(programmed) / 128.0;

        // Zero is the documented free-start setting. Each allocated hardware
        // voice has its own oscillator state, so retain a distinct,
        // deterministic phase for each voice allocation.
        const auto hash = static_cast<uint64_t>(voiceIndex * 29 + note * 7)
                          + order * 0x9e3779b97f4a7c15ull
                          + oscillator * 0x632be59bd9b4e019ull;
        return static_cast<double>(hash & 0xffffu) / 65536.0;
    };
    oscillator1.reset(startPhase(0));
    oscillator2.reset(startPhase(1));

    // A reassigned hardware voice does not discharge its analogue filter,
    // reconstruction network, sample-and-hold capacitors or VCA in one CPU
    // cycle. Keeping those states gives the new note the short, continuous
    // handover of the physical voice and avoids an impossible full-scale edge.
    if (!isStealingActiveVoice)
    {
        highpassFilter.reset();
        reconstruction.reset();
        asicResampler.reset();
        circuit.reset();
        baseCutoffInitialised = false;
    }

    envelopeParameters.attack = parameters.attackSeconds;
    envelopeParameters.decay = parameters.decaySeconds;
    envelopeParameters.sustain = parameters.sustainLevel;
    envelopeParameters.release = parameters.releaseSeconds;
    envelopeParameters.useMeasuredAmplifierAttackScaling = true;
    envelope.setParameters(envelopeParameters);
    // A stolen WDV voice starts a new digital envelope calculation. Its
    // analogue CEM/VCA state is deliberately retained above for a click-free
    // handover, but retaining the previous note's envelope level makes groups
    // of release-tail voices begin at different effective filter cutoffs.
    envelope.reset();
    envelope.noteOn();
    filterEnvelopeParameters.attack = parameters.filterAttackSeconds;
    filterEnvelopeParameters.decay = parameters.filterDecaySeconds;
    filterEnvelopeParameters.sustain = parameters.filterSustainLevel;
    filterEnvelopeParameters.release = parameters.filterReleaseSeconds;
    filterEnvelope.setParameters(filterEnvelopeParameters);
    filterEnvelope.reset();
    filterEnvelopeDelaySamples = juce::roundToInt(
        juce::jmax(0.0f, parameters.filterDelaySeconds) * static_cast<float>(sampleRate));
    filterEnvelopePending = filterEnvelopeDelaySamples > 0;
    filterEnvelopeTriggered = !filterEnvelopePending;
    if (filterEnvelopeTriggered)
        filterEnvelope.noteOn();
    waveEnvelope.noteOn();
    freeEnvelope.noteOn();
    for (size_t lfo = 0; lfo < lfos.size(); ++lfo)
        lfos[lfo].noteOn(parameters.lfos[lfo].sync,
                         parameters.lfos[lfo].phaseDegrees);
    waveStartOffsets.fill(0.0f);
    waveStartOffsetsPending = true;
}

void WaldorfEngine::Voice::release(bool allowSustain)
{
    keyDown = false;
    if (active && !allowSustain)
    {
        envelope.noteOff();
        if (filterEnvelopeTriggered)
            filterEnvelope.noteOff();
        else
            filterEnvelopePending = false;
        waveEnvelope.noteOff();
    }
}

float WaldorfEngine::Voice::baseFrequencyHz() const noexcept
{
    const auto pitchNote = glideQuantised
                               ? std::round(currentGlideNote)
                               : currentGlideNote;
    return 440.0f * std::exp2((pitchNote - 69.0f) / 12.0f);
}

void WaldorfEngine::Voice::updatePitch(const parameters::Snapshot& parameters, float pitchBend)
{
    if (note < 0)
        return;
    const auto baseFrequency = baseFrequencyHz();
    const auto bend1 = pitchBend * parameters.oscillatorBendRanges[0] / 2.0f;
    const auto bend2Range = parameters.oscillatorLinkEnabled
                                ? parameters.oscillatorBendRanges[0]
                                : parameters.oscillatorBendRanges[1];
    const auto bend2 = pitchBend * bend2Range / 2.0f;
    const auto performanceRatio = std::exp2(performanceDetuneCents / 1200.0f);
    auto detune = parameters.oscillatorDetuneCents;
    if (detune[0] == 0.0f && detune[1] == 0.0f && parameters.detuneCents != 0.0f)
        detune = { -0.5f * parameters.detuneCents, 0.5f * parameters.detuneCents };
    oscillator1.setFrequency(
        baseFrequency * std::exp2(bend1 / 12.0f) * performanceRatio
        * std::exp2((static_cast<float>(parameters.oscillatorOctaves[0] * 12)
                     + parameters.oscillatorSemitones[0] + detune[0] / 100.0f)
                    / 12.0f));
    oscillator2.setFrequency(
        baseFrequency * std::exp2(bend2 / 12.0f) * performanceRatio
        * std::exp2((static_cast<float>(parameters.oscillatorOctaves[1] * 12)
                     + parameters.oscillatorSemitones[1] + detune[1] / 100.0f)
                    / 12.0f));
}

Cem3387::StereoSample WaldorfEngine::Voice::process(
    const WavetableBank& bank, const parameters::Snapshot& parameters,
    float modWheel, float channelPressure, float pitchBend)
{
    if (!active)
        return {};

    if (glideSamplesRemaining > 0)
    {
        currentGlideNote += glideStepPerSample;
        if (--glideSamplesRemaining <= 0)
            currentGlideNote = targetGlideNote;
        updatePitch(parameters, pitchBend);
    }

    envelopeParameters.attack = parameters.attackSeconds;
    envelopeParameters.decay = parameters.decaySeconds;
    envelopeParameters.sustain = parameters.sustainLevel;
    envelopeParameters.release = parameters.releaseSeconds;
    envelope.setParameters(envelopeParameters);

    const auto env = envelope.getNextSample();
    currentAmplifierEnvelope = env;
    const auto amplifierEnvelopeActive = envelope.isActive();
    if (!amplifierEnvelopeActive && vcaDrainSamplesRemaining == 0)
        vcaDrainSamplesRemaining = juce::roundToInt(sampleRate * 0.012);
    const auto updateControlTargets = samplesUntilControlUpdate <= 0.0;
    if (updateControlTargets)
        samplesUntilControlUpdate += sampleRate / 1000.0;
    samplesUntilControlUpdate -= 1.0;

    const auto targetBaseCutoffSemitones
        = 12.0f * std::log2(juce::jlimit(20.0f, 32000.0f, parameters.cutoffHz)
                            / 20.0f);
    if (!baseCutoffInitialised)
    {
        smoothedBaseCutoffSemitones = targetBaseCutoffSemitones;
        baseCutoffInitialised = true;
    }
    else
    {
        smoothedBaseCutoffSemitones
            += (targetBaseCutoffSemitones - smoothedBaseCutoffSemitones)
               * baseCutoffSlew;
    }

    if (updateControlTargets)
    {
        const auto filterStageModulation = [&](size_t stage) {
            return wdvAmountDepth(parameters.filterEnvelopeModAmounts[stage])
                   * sourceValue(parameters.filterEnvelopeModSources[stage], env,
                                 waveEnvelope.currentValue(), currentLfoValues,
                                 modWheel, channelPressure, pitchBend);
        };
        const auto modulatedEnvelopeTime = [&](float base, size_t stage) {
            return juce::jlimit(0.001f, 20.0f,
                                base * std::exp2(4.0f * filterStageModulation(stage)));
        };
        filterEnvelopeParameters.attack = modulatedEnvelopeTime(
            parameters.filterAttackSeconds, 0);
        filterEnvelopeParameters.decay = modulatedEnvelopeTime(
            parameters.filterDecaySeconds, 1);
        filterEnvelopeParameters.sustain = juce::jlimit(
            0.0f, 1.0f, parameters.filterSustainLevel + filterStageModulation(2));
        filterEnvelopeParameters.release = modulatedEnvelopeTime(
            parameters.filterReleaseSeconds, 3);
        filterEnvelope.setParameters(filterEnvelopeParameters);
    }

    if (filterEnvelopePending)
    {
        if (--filterEnvelopeDelaySamples <= 0)
        {
            filterEnvelopePending = false;
            filterEnvelopeTriggered = true;
            filterEnvelope.noteOn();
        }
        currentFilterEnvelope = 0.0f;
    }
    else
    {
        currentFilterEnvelope = filterEnvelopeTriggered
                                    ? filterEnvelope.getNextSample()
                                    : 0.0f;
    }

    const auto waveEnvelopeValue = waveEnvelope.process(parameters);
    currentFreeEnvelope = freeEnvelope.process(parameters);
    if (updateControlTargets)
    {
        const auto lfo1RateMod = routeValue(
            parameters::lfo1RateMod, parameters, env, waveEnvelopeValue,
            currentLfoValues, modWheel, channelPressure, pitchBend);
        controlLfoRates[0]
            = parameters.lfos[0].rate
              + 128.0f
                    * wdvAmountDepth(
                        parameters.modulationRoutes[parameters::lfo1RateMod].amount)
                    * lfo1RateMod;
        const auto lfo2RateMod = routeValue(
            parameters::lfo2RateMod, parameters, env, waveEnvelopeValue,
            currentLfoValues, modWheel, channelPressure, pitchBend);
        controlLfoRates[1]
            = parameters.lfos[1].rate
              + 128.0f
                    * wdvAmountDepth(
                        parameters.modulationRoutes[parameters::lfo2RateMod].amount)
                    * lfo2RateMod;
    }
    const auto lfo1Raw = lfos[0].process(
        controlLfoRates[0],
        parameters.lfos[0].shape, parameters.lfos[0].symmetry,
        parameters.lfos[0].humanize);
    currentLfoValues[0] = lfo1Raw;

    const auto lfo2Raw = lfos[1].process(
        controlLfoRates[1],
        parameters.lfos[1].shape, parameters.lfos[1].symmetry,
        parameters.lfos[1].humanize);
    currentLfoValues[1] = lfo2Raw;

    if (updateControlTargets)
    {
        for (size_t lfo = 0; lfo < lfos.size(); ++lfo)
        {
            const auto route = lfo == 0 ? parameters::lfo1LevelMod
                                        : parameters::lfo2LevelMod;
            const auto amount = parameters.modulationRoutes[route].amount;
            controlLfoLevels[lfo] = 1.0f;
            if (std::abs(amount) >= 0.5f)
            {
                const auto levelMod = routeValue(
                    route, parameters, env, waveEnvelopeValue, currentLfoValues,
                    modWheel, channelPressure, pitchBend);
                controlLfoLevels[lfo] = juce::jlimit(
                    -1.0f, 1.0f, wdvAmountDepth(amount) * levelMod);
            }
        }
    }
    currentLfoValues[0] = lfo1Raw * controlLfoLevels[0];
    currentLfoValues[1] = lfo2Raw * controlLfoLevels[1];

    updateControlSampleAndHold(parameters, env, waveEnvelopeValue,
                               currentLfoValues, modWheel, channelPressure,
                               pitchBend);
    updateControlComparator(parameters, env, waveEnvelopeValue,
                            currentLfoValues, modWheel, channelPressure,
                            pitchBend);

    if (updateControlTargets)
    {
        // Start modifiers are sampled at note-on; all other WDV targets are
        // refreshed by the voice-board control interrupt rather than by the
        // host audio clock.
        if (waveStartOffsetsPending)
        {
            waveStartOffsets[0]
                = 64.0f
                  * wdvAmountDepth(
                      parameters.modulationRoutes[parameters::wave1StartMod].amount)
                  * routeValue(parameters::wave1StartMod, parameters, env,
                               waveEnvelopeValue, currentLfoValues, modWheel,
                               channelPressure, pitchBend);
            waveStartOffsets[1]
                = 64.0f
                  * wdvAmountDepth(
                      parameters.modulationRoutes[parameters::wave2StartMod].amount)
                  * routeValue(parameters::wave2StartMod, parameters, env,
                               waveEnvelopeValue, currentLfoValues, modWheel,
                               channelPressure, pitchBend);
            waveStartOffsetsPending = false;
        }

        const auto bend1 = pitchBend * parameters.oscillatorBendRanges[0] / 2.0f;
        const auto bend2Range = parameters.oscillatorLinkEnabled
                                    ? parameters.oscillatorBendRanges[0]
                                    : parameters.oscillatorBendRanges[1];
        const auto bend2 = pitchBend * bend2Range / 2.0f;
        const auto pitch1 = bend1
            + static_cast<float>(parameters.oscillatorOctaves[0] * 12)
            + parameters.oscillatorSemitones[0]
            + wdvPitchDepthSemitones(
                parameters.modulationRoutes[parameters::osc1PitchMod1].amount)
                  * routeValue(parameters::osc1PitchMod1, parameters, env,
                               waveEnvelopeValue, currentLfoValues, modWheel,
                               channelPressure, pitchBend)
            + wdvPitchDepthSemitones(
                parameters.modulationRoutes[parameters::osc1PitchMod2].amount)
                  * routeValue(parameters::osc1PitchMod2, parameters, env,
                               waveEnvelopeValue, currentLfoValues, modWheel,
                               channelPressure, pitchBend);
        const auto oscillator2Mod1 = parameters.oscillatorLinkEnabled
                                         ? parameters::osc1PitchMod1
                                         : parameters::osc2PitchMod1;
        const auto oscillator2Mod2 = parameters.oscillatorLinkEnabled
                                         ? parameters::osc1PitchMod2
                                         : parameters::osc2PitchMod2;
        const auto pitch2 = bend2
            + static_cast<float>(parameters.oscillatorOctaves[1] * 12)
            + parameters.oscillatorSemitones[1]
            + wdvPitchDepthSemitones(
                parameters.modulationRoutes[oscillator2Mod1].amount)
                  * routeValue(oscillator2Mod1, parameters, env,
                               waveEnvelopeValue, currentLfoValues, modWheel,
                               channelPressure, pitchBend)
            + wdvPitchDepthSemitones(
                parameters.modulationRoutes[oscillator2Mod2].amount)
                  * routeValue(oscillator2Mod2, parameters, env,
                               waveEnvelopeValue, currentLfoValues, modWheel,
                               channelPressure, pitchBend);
        currentPitchModulations = { pitch1 - bend1, pitch2 - bend2 };
        // The control interrupt must use the same live tuned/gliding pitch as
        // Voice::updatePitch. Using the original MIDI key here alternated HMT
        // (or a User tuning) with equal temperament about once per millisecond.
        const auto baseFrequency = baseFrequencyHz();
        const auto performanceRatio = std::exp2(performanceDetuneCents / 1200.0f);
        auto detune = parameters.oscillatorDetuneCents;
        if (detune[0] == 0.0f && detune[1] == 0.0f && parameters.detuneCents != 0.0f)
            detune = { -0.5f * parameters.detuneCents, 0.5f * parameters.detuneCents };
        oscillator1.setFrequency(
            baseFrequency * performanceRatio
            * std::exp2((pitch1 + detune[0] / 100.0f) / 12.0f));
        oscillator2.setFrequency(
            baseFrequency * performanceRatio
            * std::exp2((pitch2 + detune[1] / 100.0f) / 12.0f));

        const auto wave1Mod = waveStartOffsets[0]
            + 64.0f * wdvAmountDepth(
                          parameters.modulationRoutes[parameters::wave1Mod1].amount)
                  * routeValue(parameters::wave1Mod1, parameters, env,
                               waveEnvelopeValue, currentLfoValues, modWheel,
                               channelPressure, pitchBend)
            + 64.0f * wdvAmountDepth(
                          parameters.modulationRoutes[parameters::wave1Mod2].amount)
                  * routeValue(parameters::wave1Mod2, parameters, env,
                               waveEnvelopeValue, currentLfoValues, modWheel,
                               channelPressure, pitchBend);
        const auto keyPosition = juce::jlimit(
            -1.0f, 1.0f, static_cast<float>(note - 60) / 60.0f);
        const auto wave1EnvelopeDepth = wdvAmountDepth(parameters.waveScan)
            + wdvAmountDepth(parameters.waveEnvelopeVelocityAmounts[0]) * velocity;
        controlWavePositions[0] = juce::jlimit(
            0.0f, 63.0f,
            parameters.wavePosition + 64.0f * wave1EnvelopeDepth * waveEnvelopeValue
                + 64.0f * wdvAmountDepth(parameters.waveKeytrackAmounts[0]) * keyPosition
                + wave1Mod);
        currentWavePosition = controlWavePositions[0];
        const auto wave2Mod = waveStartOffsets[1]
            + 64.0f * wdvAmountDepth(
                          parameters.modulationRoutes[parameters::wave2Mod1].amount)
                  * routeValue(parameters::wave2Mod1, parameters, env,
                               waveEnvelopeValue, currentLfoValues, modWheel,
                               channelPressure, pitchBend)
            + 64.0f * wdvAmountDepth(
                          parameters.modulationRoutes[parameters::wave2Mod2].amount)
                  * routeValue(parameters::wave2Mod2, parameters, env,
                               waveEnvelopeValue, currentLfoValues, modWheel,
                               channelPressure, pitchBend);
        const auto wave2EnvelopeDepth = wdvAmountDepth(parameters.waveScan2)
            + wdvAmountDepth(parameters.waveEnvelopeVelocityAmounts[1]) * velocity;
        controlWavePositions[1] = juce::jlimit(
            0.0f, 63.0f,
            parameters.wavePosition2 + 64.0f * wave2EnvelopeDepth * waveEnvelopeValue
                + 64.0f * wdvAmountDepth(parameters.waveKeytrackAmounts[1]) * keyPosition
                + wave2Mod);
        auto baseWaveLevels = parameters.waveLevels;
        if (baseWaveLevels[0] == 0.5f && baseWaveLevels[1] == 0.5f
            && parameters.oscillatorBalance != 0.5f)
            baseWaveLevels = { 1.0f - parameters.oscillatorBalance,
                               parameters.oscillatorBalance };
        constexpr auto maximumNativeWaveLevel = 127.0f / 112.0f;
        controlWaveLevels[0] = juce::jlimit(
            0.0f, maximumNativeWaveLevel, baseWaveLevels[0]
                            + wdvVolumeDepth(parameters.modulationRoutes[
                                                  parameters::wave1VolumeMod].amount)
                                  * routeValue(parameters::wave1VolumeMod, parameters,
                                               env, waveEnvelopeValue, currentLfoValues,
                                               modWheel, channelPressure, pitchBend));
        controlWaveLevels[1] = juce::jlimit(
            0.0f, maximumNativeWaveLevel, baseWaveLevels[1]
                            + wdvVolumeDepth(parameters.modulationRoutes[
                                                  parameters::wave2VolumeMod].amount)
                                  * routeValue(parameters::wave2VolumeMod, parameters,
                                               env, waveEnvelopeValue, currentLfoValues,
                                               modWheel, channelPressure, pitchBend));
        controlNoiseLevel = juce::jlimit(
            0.0f, 1.0f, parameters.noiseLevel
                            + wdvVolumeDepth(parameters.modulationRoutes[
                                                  parameters::noiseVolumeMod].amount)
                                  * routeValue(parameters::noiseVolumeMod, parameters,
                                               env, waveEnvelopeValue, currentLfoValues,
                                               modWheel, channelPressure, pitchBend));

        const auto filterPitch
            = wdvEnvelopeCutoffSteps(parameters.filterEnvelopeSemitones,
                                     parameters.filterVelocitySemitones,
                                     velocity, currentFilterEnvelope)
              + wdvFilterKeytrackSteps(
                    parameters.filterKeytrackAmount,
                    note - parameters.filterKeyCenterNote)
              + 128.0f * wdvAmountDepth(
                              parameters.modulationRoutes[parameters::filterMod1].amount)
                    * routeValue(parameters::filterMod1, parameters, env,
                                 waveEnvelopeValue, currentLfoValues, modWheel,
                                 channelPressure, pitchBend)
              + 128.0f * wdvAmountDepth(
                              parameters.modulationRoutes[parameters::filterMod2].amount)
                    * routeValue(parameters::filterMod2, parameters, env,
                                 waveEnvelopeValue, currentLfoValues, modWheel,
                                 channelPressure, pitchBend);
        const auto cutoff = 20.0f * std::exp2(
            (smoothedBaseCutoffSemitones + filterPitch) / 12.0f);
        controlAnalogueCutoff = cutoff;
        controlHighpassCutoff = cutoff;
        controlFilterMode = juce::jlimit(0, 3, parameters.filterMode);
        if (controlFilterMode == 1)
            controlAnalogueCutoff = 20000.0f;
        else if (controlFilterMode == 2)
        {
            const auto halfBandwidth = 0.5f * parameters.bandpassBandwidthSemitones;
            controlHighpassCutoff = cutoff * std::exp2(-halfBandwidth / 12.0f);
            controlAnalogueCutoff = cutoff * std::exp2(halfBandwidth / 12.0f);
        }
        else if (controlFilterMode == 3)
        {
            const auto selectedEnvelope
                = parameters.highpassEnvelopeSelector == 0
                      ? env
                      : (parameters.highpassEnvelopeSelector == 1
                             ? currentFilterEnvelope
                             : (parameters.highpassEnvelopeSelector == 2
                                    ? waveEnvelopeValue : currentFreeEnvelope));
            const auto highpassPitch
                = wdvEnvelopeCutoffSteps(parameters.highpassEnvelopeSemitones,
                                         parameters.highpassVelocitySemitones,
                                         velocity, selectedEnvelope)
                  + wdvFilterKeytrackSteps(
                        parameters.highpassKeytrackAmount,
                        note - parameters.highpassKeyCenterNote)
                  + 128.0f * wdvAmountDepth(parameters.modulationRoutes[
                                                  parameters::highpassMod1].amount)
                        * routeValue(parameters::highpassMod1, parameters, env,
                                     waveEnvelopeValue, currentLfoValues, modWheel,
                                     channelPressure, pitchBend)
                  + 128.0f * wdvAmountDepth(parameters.modulationRoutes[
                                                  parameters::highpassMod2].amount)
                        * routeValue(parameters::highpassMod2, parameters, env,
                                     waveEnvelopeValue, currentLfoValues, modWheel,
                                     channelPressure, pitchBend);
            controlHighpassCutoff = parameters.highpassCutoffHz
                                    * std::exp2(highpassPitch / 12.0f);
        }

        const auto panPolarity = parameters.panModulationMode == 0
                                     ? 0.0f
                                     : (parameters.panModulationMode == 2 ? -1.0f : 1.0f);
        controlPan = juce::jlimit(
            -1.0f, 1.0f, parameters.panAmount
                + panPolarity * wdvAmountDepth(parameters.modulationRoutes[
                                                     parameters::panMod1].amount)
                      * routeValue(parameters::panMod1, parameters, env,
                                   waveEnvelopeValue, currentLfoValues, modWheel,
                                   channelPressure, pitchBend)
                + panPolarity * wdvAmountDepth(parameters.modulationRoutes[
                                                     parameters::panMod2].amount)
                      * routeValue(parameters::panMod2, parameters, env,
                                   waveEnvelopeValue, currentLfoValues, modWheel,
                                   channelPressure, pitchBend));
        controlResonance = juce::jlimit(
            0.0f, 1.0f, parameters.resonanceAmount
                + wdvAmountDepth(parameters.modulationRoutes[
                                       parameters::resonanceMod].amount)
                      * routeValue(parameters::resonanceMod, parameters, env,
                                   waveEnvelopeValue, currentLfoValues, modWheel,
                                   channelPressure, pitchBend));
        controlAmplifierMod
            = wdvAmountDepth(parameters.modulationRoutes[
                                  parameters::amplifierMod1].amount)
                  * routeValue(parameters::amplifierMod1, parameters, env,
                               waveEnvelopeValue, currentLfoValues, modWheel,
                               channelPressure, pitchBend)
              + wdvAmountDepth(parameters.modulationRoutes[
                                    parameters::amplifierMod2].amount)
                    * routeValue(parameters::amplifierMod2, parameters, env,
                                 waveEnvelopeValue, currentLfoValues, modWheel,
                                 channelPressure, pitchBend);
    }

    reconstruction.setAge(parameters.circuitAgeAmount);
    auto levelCode1 = static_cast<uint8_t>(juce::jlimit(
        0, 127, juce::roundToInt(controlWaveLevels[0] * 112.0f)));
    auto levelCode2 = static_cast<uint8_t>(juce::jlimit(
        0, 127, juce::roundToInt(controlWaveLevels[1] * 112.0f)));
    if (quantiseWaveLevels)
    {
        levelCode1 = AsicOutputMixer::levelFromRegisterBits(levelCode1);
        levelCode2 = AsicOutputMixer::levelFromRegisterBits(levelCode2);
    }
    asicClockPhase += OscillatorChipProxy::modelClockRate() / sampleRate;
    while (asicClockPhase >= 1.0)
    {
        asicClockPhase -= 1.0;
        const auto oscillatorCode1 = oscillator1.tickCode(
            bank, parameters.wavetableIndex, controlWavePositions[0], true);
        const auto oscillatorCode2 = oscillator2.tickCode(
            bank, parameters.wavetableIndex, controlWavePositions[1], true);
        const auto oscillatorMix = AsicOutputMixer::normaliseOutput(
            AsicOutputMixer::mixOscillatorCodes(
                oscillatorCode1, oscillatorCode2, levelCode1, levelCode2));
        // The documented ES2 overflow concerns the two oscillator level
        // accumulator. Noise joins at the DAC boundary and may drive the
        // following analogue input without changing that wrap decision.
        const auto dacInput = oscillatorMix
                              + noise() * controlNoiseLevel * 0.22f;
        const auto digitalInput
            = controlFilterMode == 0
                  ? dacInput
                  : highpassFilter.process(dacInput, controlHighpassCutoff);
        asicResampler.push(reconstruction.process(digitalInput));
    }
    const auto reconstructed = asicResampler.read(asicClockPhase);
    circuit.setControls(controlAnalogueCutoff, controlResonance,
                        parameters.driveDb, controlPan,
                        parameters.circuitAgeAmount);

    const auto velocityResponse = 0.25f + 0.75f * velocity;
    const auto vcaTarget = amplifierEnvelopeActive
                               ? juce::jlimit(0.0f, 1.0f, env + controlAmplifierMod)
                                     * velocityResponse
                               : 0.0f;
    const auto output = circuit.process(reconstructed, vcaTarget);
    // The WDV envelope can reach zero in one 53 Hz control tick at its
    // shortest release settings. The physical AD7545/PD508/hold-capacitor
    // path still has to discharge, so keep the voice circuit alive until its
    // modelled VCA voltage is effectively closed. Removing the voice at the
    // digital zero bypasses that analogue transition and produces a loud
    // full-band click.
    if (!amplifierEnvelopeActive)
    {
        --vcaDrainSamplesRemaining;
        if (circuit.currentVcaCv() <= 1.0f / 4095.0f
            || vcaDrainSamplesRemaining <= 0)
        {
            active = false;
            note = -1;
            vcaDrainSamplesRemaining = 0;
        }
    }
    return output;
}

float WaldorfEngine::Voice::sourceValue(
    int source, float ampEnvelope, float waveEnvelopeValue,
    const std::array<float, 2>& lfoValues, float modWheel, float channelPressure,
    float pitchBend) const noexcept
{
    switch (juce::jlimit(0, 39, source))
    {
        case 0: return lfoValues[0];
        case 1: return lfoValues[1];
        case 2: return ampEnvelope;
        case 3: return currentFilterEnvelope;
        case 4: return waveEnvelopeValue;
        case 5: return currentFreeEnvelope;
        case 10: return currentControlSampleAndHold;
        case 11: return currentComparatorPositive;
        case 12: return currentComparatorNegative;
        case 13: return juce::jlimit(-1.0f, 1.0f, static_cast<float>(note - 60) / 60.0f);
        case 14: return velocity;
        case 16: return channelPressure;
        case 21: return juce::jlimit(-1.0f, 1.0f, pitchBend / 2.0f);
        case 22: return modWheel;
        case 23: return juce::jmax(0.0f, currentFreeWheel);
        case 24: return juce::jmax(0.0f, -currentFreeWheel);
        case 25: return currentFreeWheel;
        case 26: return keyDown ? 1.0f : 0.0f;
        case 29: return currentButton1;
        case 30: return currentButton2;
        case 31: return currentVolumeController;
        case 32: return currentPanController;
        case 33: return currentBreathController;
        case 34: return currentControlX;
        case 35: return currentControlY;
        case 37: return -1.0f;
        case 38: return 1.0f;
        default: return 0.0f;
    }
}

void WaldorfEngine::Voice::updateControlSampleAndHold(
    const parameters::Snapshot& parameters, float ampEnvelope,
    float waveEnvelopeValue, const std::array<float, 2>& lfoValues,
    float modWheel, float channelPressure, float pitchBend) noexcept
{
    const auto rateMod = sourceValue(parameters.controlSampleAndHoldRateModSource,
                                     ampEnvelope, waveEnvelopeValue, lfoValues,
                                     modWheel, channelPressure, pitchBend);
    const auto rawRate = juce::jlimit(
        0.0f, 127.0f,
        parameters.controlSampleAndHoldRate
            + 128.0f * wdvAmountDepth(parameters.controlSampleAndHoldRateModAmount)
                  * rateMod);

    // The WAVE control is an interval setting: factory use shows zero is its
    // fastest end. Until the undocumented ASIC divider is measured, use the
    // inverse of the firmware-derived LFO span for its control clock. This
    // preserves the documented stepped behaviour without inventing audio-rate
    // switching.
    const auto rateHz = WaveLfo::rateHz(127.0f - rawRate);
    controlSampleAndHoldPhase += rateHz / sampleRate;
    if (!controlSampleAndHoldInitialised || controlSampleAndHoldPhase >= 1.0)
    {
        controlSampleAndHoldPhase -= std::floor(controlSampleAndHoldPhase);
        currentControlSampleAndHold = sourceValue(
            parameters.controlSampleAndHoldSource, ampEnvelope,
            waveEnvelopeValue, lfoValues, modWheel, channelPressure, pitchBend);
        controlSampleAndHoldInitialised = true;
    }
}

void WaldorfEngine::Voice::updateControlComparator(
    const parameters::Snapshot& parameters, float ampEnvelope,
    float waveEnvelopeValue, const std::array<float, 2>& lfoValues,
    float modWheel, float channelPressure, float pitchBend) noexcept
{
    const auto input = sourceValue(parameters.controlComparatorSource,
                                   ampEnvelope, waveEnvelopeValue, lfoValues,
                                   modWheel, channelPressure, pitchBend);
    const auto threshold = juce::jlimit(
        -1.0f, 63.0f / 64.0f,
        parameters.controlComparatorThreshold / 64.0f);
    const auto reached = input >= threshold;
    currentComparatorPositive = reached ? 1.0f : 0.0f;
    currentComparatorNegative = reached ? 0.0f : 1.0f;
}

void WaldorfEngine::Voice::advanceIdleLfos(
    const parameters::Snapshot& parameters, int samples) noexcept
{
    for (size_t lfo = 0; lfo < lfos.size(); ++lfo)
        currentLfoValues[lfo]
            = lfos[lfo].processSamples(samples, parameters.lfos[lfo].rate,
                                       parameters.lfos[lfo].shape,
                                       parameters.lfos[lfo].symmetry,
                                       parameters.lfos[lfo].humanize);
}

float WaldorfEngine::Voice::routeValue(
    parameters::ModulationRouteIndex route, const parameters::Snapshot& parameters,
    float ampEnvelope, float waveEnvelopeValue, const std::array<float, 2>& lfoValues,
    float modWheel, float channelPressure, float pitchBend) const noexcept
{
    const auto& assignment = parameters.modulationRoutes[static_cast<size_t>(route)];
    return sourceValue(assignment.source, ampEnvelope, waveEnvelopeValue, lfoValues,
                       modWheel, channelPressure, pitchBend)
           * sourceValue(assignment.control, ampEnvelope, waveEnvelopeValue, lfoValues,
                         modWheel, channelPressure, pitchBend);
}

float WaldorfEngine::Voice::noise() noexcept
{
    noiseState ^= noiseState << 13u;
    noiseState ^= noiseState >> 17u;
    noiseState ^= noiseState << 5u;
    return static_cast<float>(static_cast<int32_t>(noiseState))
           / static_cast<float>(std::numeric_limits<int32_t>::max());
}

void WaldorfEngine::handleMidi(const juce::MidiMessage& message,
                               const PerformanceSnapshot& performance,
                               bool localKeyboard)
{
    if (message.isNoteOn())
    {
        const auto midiNote = message.getNoteNumber();
        const auto midiVelocity = juce::jlimit(
            1, 127, juce::roundToInt(message.getFloatVelocity() * 127.0f));
        const auto triggerId = ++triggerOrder;
        for (size_t layerIndex = 0; layerIndex < performance.layers.size(); ++layerIndex)
        {
            const auto& layer = performance.layers[layerIndex];
            const auto acceptsNoteSource = localKeyboard
                                               ? layer.source == 1 || layer.source == 3
                                               : layer.source == 2 || layer.source == 3;
            if (!layer.enabled || !acceptsNoteSource
                || midiNote < layer.keyLow || midiNote > layer.keyHigh
                || midiVelocity < layer.velocityLow || midiVelocity > layer.velocityHigh
                || (!localKeyboard && layer.midiChannel != 0
                    && message.getChannel() != layer.midiChannel))
                continue;

            const auto translatedNote = juce::jlimit(
                0, 127, midiNote + layer.transposeSemitones);
            const auto tunedTargetNote = tunedNoteForLayer(
                layer, translatedNote, triggerId, static_cast<int>(layerIndex));
            const auto previousNote
                = lastPlayedNotesByLayer[layerIndex];
            const auto hasHeldLayerVoice = std::any_of(
                voices.begin(), voices.end(), [layerIndex](const auto& candidate) {
                    return candidate.active && candidate.keyDown
                           && candidate.layerIndex == static_cast<int>(layerIndex);
                });
            const auto glideMode = juce::jlimit(1, 6, layer.sound.glideTypeMode);
            const auto midiControlled = glideMode == 3 || glideMode == 4;
            const auto fingered = glideMode == 5 || glideMode == 6;
            const auto midiPortamentoEnabled
                = midiControllerAmounts[65] >= 0.5f;
            const auto shouldGlide
                = layer.sound.glideEnabled && previousNote >= 0
                  && (!fingered || hasHeldLayerVoice)
                  && (!midiControlled || midiPortamentoEnabled);
            const auto rate = midiControlled
                                  ? midiControllerAmounts[5] * 127.0f
                                  : layer.sound.glideRateValue;

            auto glideFromNote = tunedTargetNote;
            if (shouldGlide)
            {
                glideFromNote = layerGlideTrajectories[layerIndex].initialised
                                    ? layerGlideTrajectories[layerIndex].targetNote
                                    : tunedNoteForLayer(
                                          layer, previousNote, triggerId - 1u,
                                          static_cast<int>(layerIndex));
            }
            float inheritedGlideStepPerSample = 0.0f;
            int inheritedGlideSamplesRemaining = 0;
            auto& glideTrajectory = layerGlideTrajectories[layerIndex];
            if (shouldGlide && glideTrajectory.initialised
                && glideTrajectory.samplesRemaining > 0)
            {
                // Portamento belongs to the instrument layer, not to the
                // particular hardware voice that happened to start it. A new
                // key therefore begins at the live glide accumulator even if
                // the preceding voice has already completed its VCA release.
                glideFromNote = glideTrajectory.currentNote;

                // Repeating the same destination must not restart either the
                // curve or its timer. When the destination changes, Voice::start
                // computes a fresh leg from this live position instead.
                if (std::abs(glideTrajectory.targetNote
                             - tunedTargetNote) <= 1.0e-5f)
                {
                    inheritedGlideStepPerSample = glideTrajectory.stepPerSample;
                    inheritedGlideSamplesRemaining = glideTrajectory.samplesRemaining;
                }
            }

            auto& voice = chooseVoice();
            voice.start(midiNote, localKeyboard ? 0 : message.getChannel(),
                        performanceVelocity(message.getFloatVelocity(), layer.velocityTable),
                        ++noteOrder, triggerId, static_cast<int>(layerIndex), layer,
                        tunedTargetNote, glideFromNote,
                        rate, layer.sound.glideTimeModeValue != 0,
                        glideMode == 2 || glideMode == 4 || glideMode == 6,
                        inheritedGlideStepPerSample, inheritedGlideSamplesRemaining,
                        modWheelAmount, channelPressureAmount, pitchBendSemitones);

            const auto destinationChanged
                = shouldGlide && glideTrajectory.initialised
                  && glideTrajectory.samplesRemaining > 0
                  && std::abs(glideTrajectory.targetNote
                              - tunedTargetNote) > 1.0e-5f;
            if (destinationChanged)
            {
                // Released voices can remain audible for a substantial VCA
                // tail. Leaving those voices on the abandoned trajectory makes
                // the instrument audibly keep rising after a lower key has
                // redirected the layer glide. The hardware layer clock changes
                // direction as one unit, so attach every release tail to the
                // newly calculated leg while leaving held polyphonic notes alone.
                for (auto& candidate : voices)
                {
                    if (&candidate == &voice || !candidate.active
                        || candidate.keyDown
                        || candidate.layerIndex != static_cast<int>(layerIndex))
                        continue;

                    candidate.currentGlideNote = voice.currentGlideNote;
                    candidate.targetGlideNote = voice.targetGlideNote;
                    candidate.glideStepPerSample = voice.glideStepPerSample;
                    candidate.glideSamplesRemaining = voice.glideSamplesRemaining;
                    candidate.glideQuantised = voice.glideQuantised;
                    candidate.updatePitch(layer.sound, pitchBendSemitones);
                }
            }
            glideTrajectory.currentNote = voice.currentGlideNote;
            glideTrajectory.targetNote = voice.targetGlideNote;
            glideTrajectory.stepPerSample = voice.glideStepPerSample;
            glideTrajectory.samplesRemaining = voice.glideSamplesRemaining;
            glideTrajectory.triggerId = triggerId;
            glideTrajectory.initialised = true;
            lastPlayedNotesByLayer[layerIndex] = translatedNote;
            voice.updatePitch(layer.sound, pitchBendSemitones);
        }
        return;
    }

    if (message.isNoteOff())
    {
        const auto channel = localKeyboard ? 0 : message.getChannel();
        auto oldestTrigger = std::numeric_limits<uint64_t>::max();
        for (const auto& voice : voices)
            if (voice.active && voice.keyDown
                && voice.triggerNote == message.getNoteNumber()
                && voice.triggerChannel == channel)
                oldestTrigger = std::min(oldestTrigger, voice.triggerId);

        // A MIDI stream may retrigger a pitch before the earlier Note Off is
        // delivered. Release only the oldest matching keystroke and all of
        // its Performance layers. Releasing every voice with this pitch also
        // kills the freshly allocated note and can collapse a three-note
        // chord to one audible note.
        if (oldestTrigger != std::numeric_limits<uint64_t>::max())
            for (auto& voice : voices)
                if (voice.active && voice.keyDown
                    && voice.triggerId == oldestTrigger)
                    voice.release(sustainPedal);
        return;
    }

    if (message.isPitchWheel())
    {
        pitchBendSemitones = (static_cast<float>(message.getPitchWheelValue()) - 8192.0f)
                             * (2.0f / 8192.0f);
        for (auto& voice : voices)
            if (voice.active)
                voice.updatePitch(
                    performance.layers[static_cast<size_t>(voice.layerIndex)].sound,
                    pitchBendSemitones);
        return;
    }

    if (message.isAllSoundOff())
    {
        for (auto& voice : voices)
            voice.reset();
        lastPlayedNotesByLayer.fill(-1);
        layerGlideTrajectories.fill({});
        sustainPedal = false;
        return;
    }

    if (message.isAllNotesOff())
    {
        for (auto& voice : voices)
            voice.release(sustainPedal);
        return;
    }

    if (message.isController())
    {
        const auto controller = message.getControllerNumber();
        const auto value = message.getControllerValue();
        midiControllerAmounts[static_cast<size_t>(juce::jlimit(0, 127, controller))]
            = static_cast<float>(value) / 127.0f;
        if (controller == 1)
            modWheelAmount = static_cast<float>(value) / 127.0f;
        else if (controller == 64)
        {
            const auto wasDown = sustainPedal;
            sustainPedal = value >= 64;
            if (wasDown && !sustainPedal)
                for (auto& voice : voices)
                    if (voice.active && !voice.keyDown)
                        voice.release(false);
        }
        return;
    }

    if (message.isChannelPressure())
    {
        channelPressureAmount = static_cast<float>(message.getChannelPressureValue()) / 127.0f;
        return;
    }

}

void WaldorfEngine::renderRange(juce::AudioBuffer<float>& output, int startSample, int endSample,
                                const PerformanceSnapshot& performance)
{
    while (startSample < endSample)
    {
        const auto chunkEnd = juce::jmin(endSample,
                                         startSample + maximumRenderBlockSize);
        renderRangeChunk(output, startSample, chunkEnd, performance);
        startSample = chunkEnd;
    }
}

void WaldorfEngine::renderRangeChunk(juce::AudioBuffer<float>& output,
                                     int startSample, int endSample,
                                     const PerformanceSnapshot& performance)
{
    const auto rangeSamples = endSample - startSample;
    if (rangeSamples <= 0)
        return;

    renderingVoiceCount = 0;
    const auto hasSoloedInstrument = std::any_of(
        performance.layers.begin(), performance.layers.end(), [](const auto& layer) {
            return layer.enabled && layer.soloed;
        });
    for (size_t voiceIndex = 0; voiceIndex < voices.size(); ++voiceIndex)
    {
        auto& voice = voices[voiceIndex];
        if (!voice.active)
        {
            const auto& idleLayer
                = performance.layers[static_cast<size_t>(voice.layerIndex)];
            voice.advanceIdleLfos(idleLayer.sound, rangeSamples);
            continue;
        }
        const auto& layer = performance.layers[static_cast<size_t>(voice.layerIndex)];
        const auto audible = !layer.muted && layer.audioOutput == 0
                             && (!hasSoloedInstrument || layer.soloed);
        voice.currentFreeWheel
            = juce::jlimit(-1.0f, 1.0f,
                           midiControllerAmounts[16] * 2.0f - 1.0f);
        voice.currentButton1 = midiControllerAmounts[80];
        voice.currentButton2 = midiControllerAmounts[81];
        voice.currentVolumeController = midiControllerAmounts[7];
        voice.currentPanController
            = juce::jlimit(-1.0f, 1.0f,
                           midiControllerAmounts[10] * 2.0f - 1.0f);
        voice.currentBreathController = midiControllerAmounts[2];
        voice.performanceDetuneCents = layer.detuneCents;
        voice.updatePitch(layer.sound,
                          layer.performancePitchBend > -99.0f
                              ? layer.performancePitchBend
                              : pitchBendSemitones);
        renderingVoices[static_cast<size_t>(renderingVoiceCount++)] = {
            &voice, &layer.sound, static_cast<int>(voiceIndex),
            audible ? layer.gain : 0.0f,
            layer.performanceModWheel >= 0.0f
                ? layer.performanceModWheel : modWheelAmount,
            layer.performanceChannelPressure >= 0.0f
                ? layer.performanceChannelPressure : channelPressureAmount,
            layer.performancePitchBend > -99.0f
                ? layer.performancePitchBend : pitchBendSemitones,
            layer.performanceControlX >= 0.0f
                ? layer.performanceControlX
                : midiControllerAmounts[static_cast<size_t>(juce::jlimit(
                      0, 127, performance.controlXController))],
            layer.performanceControlY >= 0.0f
                ? layer.performanceControlY
                : midiControllerAmounts[static_cast<size_t>(juce::jlimit(
                      0, 127, performance.controlYController))]
        };
    }

    std::array<bool, voiceBoardCount> boardHasVoices {};
    for (int index = 0; index < renderingVoiceCount; ++index)
    {
        const auto board = renderingVoices[static_cast<size_t>(index)].voiceIndex
                           / voicesPerBoard;
        boardHasVoices[static_cast<size_t>(board)] = true;
    }

    constexpr auto minimumParallelSamples = 16;
    constexpr auto minimumParallelVoices = 24;
    const auto workersAvailable = cardWorkers[0] != nullptr && cardWorkers[1] != nullptr;
    const auto useWorkers = workersAvailable
                            && voiceCardThreadingEnabled.load(std::memory_order_relaxed)
                            && rangeSamples >= minimumParallelSamples
                            && renderingVoiceCount >= minimumParallelVoices;
    std::array<uint32_t, voiceBoardCount - 1> generations {};
    if (useWorkers)
    {
        for (int board = 1; board < voiceBoardCount; ++board)
            if (boardHasVoices[static_cast<size_t>(board)])
                generations[static_cast<size_t>(board - 1)]
                    = cardWorkers[static_cast<size_t>(board - 1)]->dispatch(rangeSamples);
        renderVoiceCard(0, rangeSamples);
        for (int board = 1; board < voiceBoardCount; ++board)
            if (generations[static_cast<size_t>(board - 1)] != 0)
                cardWorkers[static_cast<size_t>(board - 1)]->waitFor(
                    generations[static_cast<size_t>(board - 1)]);
        ++parallelCardRenders;
    }
    else
    {
        for (int board = 0; board < voiceBoardCount; ++board)
            if (boardHasVoices[static_cast<size_t>(board)])
                renderVoiceCard(board, rangeSamples);
    }

    auto* left = output.getWritePointer(0);
    auto* right = output.getNumChannels() > 1 ? output.getWritePointer(1) : nullptr;
    const auto outputGain = juce::Decibels::decibelsToGain(performance.outputDb);

    using FloatVector = juce::dsp::SIMDRegister<float>;
    constexpr auto vectorWidth = static_cast<int>(FloatVector::size());
    for (int rangeSample = 0; rangeSample < rangeSamples; ++rangeSample)
    {
        float leftSum = 0.0f;
        float rightSum = 0.0f;
        auto index = 0;
        for (; index + vectorWidth <= renderingVoiceCount; index += vectorWidth)
        {
            alignas(FloatVector::SIMDRegisterSize) std::array<float, FloatVector::size()>
                leftSamples {};
            alignas(FloatVector::SIMDRegisterSize) std::array<float, FloatVector::size()>
                rightSamples {};
            alignas(FloatVector::SIMDRegisterSize) std::array<float, FloatVector::size()>
                gains {};
            for (auto lane = 0; lane < vectorWidth; ++lane)
            {
                const auto& rendering
                    = renderingVoices[static_cast<size_t>(index + lane)];
                const auto scratchIndex
                    = static_cast<size_t>(rendering.voiceIndex)
                          * static_cast<size_t>(maximumRenderBlockSize)
                      + static_cast<size_t>(rangeSample);
                const auto laneIndex = static_cast<size_t>(lane);
                leftSamples[laneIndex] = voiceLeftScratch[scratchIndex];
                rightSamples[laneIndex] = voiceRightScratch[scratchIndex];
                gains[laneIndex] = rendering.gain;
            }
            const auto gainVector = FloatVector::fromRawArray(gains.data());
            leftSum += (FloatVector::fromRawArray(leftSamples.data()) * gainVector).sum();
            rightSum += (FloatVector::fromRawArray(rightSamples.data()) * gainVector).sum();
        }
        for (; index < renderingVoiceCount; ++index)
        {
            const auto& rendering = renderingVoices[static_cast<size_t>(index)];
            const auto scratchIndex
                = static_cast<size_t>(rendering.voiceIndex)
                      * static_cast<size_t>(maximumRenderBlockSize)
                  + static_cast<size_t>(rangeSample);
            leftSum += voiceLeftScratch[scratchIndex] * rendering.gain;
            rightSum += voiceRightScratch[scratchIndex] * rendering.gain;
        }

        const auto staged = outputStage.process(leftSum, rightSum,
                                                performance.circuitAgeAmount);
        const auto outputSample = startSample + rangeSample;
        left[outputSample] += staged[0] * outputGain;
        if (right != nullptr)
            right[outputSample] += staged[1] * outputGain;
        else
            left[outputSample] += staged[1] * outputGain;

        // Advance each layer's portamento clock independently of voice
        // lifetime. This is what lets any number of later notes inherit the
        // exact in-flight pitch rather than restarting from a MIDI key value.
        for (auto& trajectory : layerGlideTrajectories)
        {
            if (trajectory.samplesRemaining <= 0)
                continue;

            trajectory.currentNote += trajectory.stepPerSample;
            if (--trajectory.samplesRemaining <= 0)
            {
                trajectory.currentNote = trajectory.targetNote;
                trajectory.stepPerSample = 0.0f;
                trajectory.samplesRemaining = 0;
            }
        }
    }
}

void WaldorfEngine::renderVoiceCard(int board, int sampleCount) noexcept
{
    const auto firstVoice = board * voicesPerBoard;
    const auto endVoice = firstVoice + voicesPerBoard;
    for (int index = 0; index < renderingVoiceCount; ++index)
    {
        auto& rendering = renderingVoices[static_cast<size_t>(index)];
        if (rendering.voiceIndex < firstVoice || rendering.voiceIndex >= endVoice)
            continue;

        auto& voice = *rendering.voice;
        voice.currentControlX = rendering.controlX;
        voice.currentControlY = rendering.controlY;
        const auto scratchBase = static_cast<size_t>(rendering.voiceIndex)
                                 * static_cast<size_t>(maximumRenderBlockSize);
        for (int sample = 0; sample < sampleCount; ++sample)
        {
            Cem3387::StereoSample voiceSample;
            if (voice.active)
                voiceSample = voice.process(
                    wavetableBank, *rendering.parameters, rendering.modWheel,
                    rendering.pressure, rendering.pitchBend);
            else
                voice.advanceIdleLfos(*rendering.parameters);
            voiceLeftScratch[scratchBase + static_cast<size_t>(sample)]
                = voiceSample.left;
            voiceRightScratch[scratchBase + static_cast<size_t>(sample)]
                = voiceSample.right;
        }
    }
}

void WaldorfEngine::startVoiceCardWorkers(double sampleRate, int maximumBlockSize)
{
    if (juce::SystemStats::getNumCpus() < voiceBoardCount)
        return;

    for (int board = 1; board < voiceBoardCount; ++board)
    {
        auto worker = std::make_unique<VoiceCardWorker>(*this, board);
        if (!worker->start(sampleRate, maximumBlockSize))
        {
            stopVoiceCardWorkers();
            return;
        }
        cardWorkers[static_cast<size_t>(board - 1)] = std::move(worker);
    }
}

void WaldorfEngine::stopVoiceCardWorkers() noexcept
{
    for (auto& worker : cardWorkers)
        if (worker != nullptr)
            worker->shutdown();
    for (auto& worker : cardWorkers)
        worker.reset();
}

int WaldorfEngine::activeVoiceCount() const noexcept
{
    return static_cast<int>(std::count_if(voices.begin(), voices.end(), [](const auto& voice) {
        return voice.active;
    }));
}

int WaldorfEngine::firstActiveMidiNote() const noexcept
{
    for (const auto& voice : voices)
        if (voice.active)
            return voice.triggerNote;
    return -1;
}

int WaldorfEngine::heldVoiceCount() const noexcept
{
    return static_cast<int>(std::count_if(voices.begin(), voices.end(), [](const auto& voice) {
        return voice.active && voice.keyDown;
    }));
}

std::array<WaldorfEngine::VoiceState, WaldorfEngine::voiceCount>
WaldorfEngine::voiceStates() const noexcept
{
    std::array<VoiceState, voiceCount> states {};
    for (size_t index = 0; index < voices.size(); ++index)
    {
        const auto& voice = voices[index];
        states[index] = {
            static_cast<int>(index), voice.triggerNote, voice.layerIndex,
            voice.currentAmplifierEnvelope, voice.controlAmplifierMod,
            voice.circuit.currentVcaCv(),
            voice.currentFilterEnvelope, voice.controlAnalogueCutoff,
            voice.circuit.currentCutoffCv(), voice.circuit.cutoffCalibrationCode(),
            voice.glideQuantised ? std::round(voice.currentGlideNote)
                                  : voice.currentGlideNote,
            voice.active, voice.keyDown,
            voice.oscillator1.frequencyHz()
        };
    }
    return states;
}

float WaldorfEngine::firstActiveWavePosition() const noexcept
{
    const auto voice = std::find_if(voices.begin(), voices.end(), [](const auto& candidate) {
        return candidate.active;
    });
    return voice != voices.end() ? voice->currentWavePosition : 0.0f;
}

float WaldorfEngine::firstActiveLfoValue(int lfo) const noexcept
{
    const auto voice = std::find_if(voices.begin(), voices.end(), [](const auto& candidate) {
        return candidate.active;
    });
    return voice != voices.end()
               ? voice->currentLfoValues[static_cast<size_t>(juce::jlimit(0, 1, lfo))]
               : 0.0f;
}

float WaldorfEngine::firstActivePitchModulation(int oscillator) const noexcept
{
    const auto voice = std::find_if(voices.begin(), voices.end(), [](const auto& candidate) {
        return candidate.active;
    });
    return voice != voices.end()
               ? voice->currentPitchModulations[static_cast<size_t>(
                     juce::jlimit(0, 1, oscillator))]
               : 0.0f;
}

float WaldorfEngine::firstActiveAmplifierEnvelopeValue() const noexcept
{
    const auto voice = std::find_if(voices.begin(), voices.end(), [](const auto& candidate) {
        return candidate.active;
    });
    return voice != voices.end() ? voice->currentAmplifierEnvelope : 0.0f;
}

float WaldorfEngine::firstActiveFilterEnvelopeValue() const noexcept
{
    const auto voice = std::find_if(voices.begin(), voices.end(), [](const auto& candidate) {
        return candidate.active;
    });
    return voice != voices.end() ? voice->currentFilterEnvelope : 0.0f;
}

float WaldorfEngine::firstActiveVcaControlValue() const noexcept
{
    const auto voice = std::find_if(voices.begin(), voices.end(), [](const auto& candidate) {
        return candidate.active;
    });
    return voice != voices.end() ? voice->circuit.currentVcaCv() : 0.0f;
}

uint16_t WaldorfEngine::filterCalibrationCode(int voice) const noexcept
{
    if (voice < 0 || voice >= voiceCount)
        return 0x0800u;
    return voices[static_cast<size_t>(voice)].filterCalibrationCode();
}

void WaldorfEngine::setFilterCalibrationCode(int voice, uint16_t code) noexcept
{
    if (voice < 0 || voice >= voiceCount)
        return;
    voices[static_cast<size_t>(voice)].setFilterCalibrationCode(code);
}

WaldorfEngine::VoiceProbe WaldorfEngine::probeVoice(
    int voiceIndex, const PerformanceLayer& layer, int midiNote,
    float velocity, int samples, int layerIndex, uint64_t order)
{
    VoiceProbe result;
    if (voiceIndex < 0 || voiceIndex >= voiceCount || samples <= 0)
        return result;

    auto& voice = voices[static_cast<size_t>(voiceIndex)];
    voice.reset();
    const auto translatedNote = juce::jlimit(
        0, 127, midiNote + layer.transposeSemitones);
    const auto tunedNote = tunedNoteForLayer(layer, translatedNote, order, layerIndex);
    voice.start(midiNote, 1, velocity, order, order, layerIndex, layer,
                tunedNote, tunedNote, 0.0f,
                false, false, 0.0f, 0, 0.0f, 0.0f, 0.0f);
    const auto performanceGain = layer.audioOutput == 0 ? layer.gain : 0.0f;
    result.minimumCutoffHz = std::numeric_limits<float>::max();
    double energy = 0.0;
    for (int sample = 0; sample < samples; ++sample)
    {
        const auto output = voice.process(wavetableBank, layer.sound,
                                          0.0f, 0.0f, 0.0f);
        result.minimumCutoffHz = juce::jmin(result.minimumCutoffHz,
                                            voice.controlAnalogueCutoff);
        result.maximumCutoffHz = juce::jmax(result.maximumCutoffHz,
                                            voice.controlAnalogueCutoff);
        result.finalCutoffHz = voice.controlAnalogueCutoff;
        result.meanCutoffHz += voice.controlAnalogueCutoff;
        const auto scaledLeft = output.left * performanceGain;
        const auto scaledRight = output.right * performanceGain;
        energy += 0.5 * (static_cast<double>(scaledLeft) * scaledLeft
                         + static_cast<double>(scaledRight) * scaledRight);
    }
    result.meanCutoffHz /= static_cast<double>(samples);
    result.rmsOutput = std::sqrt(energy / static_cast<double>(samples));
    voice.reset();
    return result;
}

bool WaldorfEngine::loadWavetableRom(const juce::MemoryBlock& data) noexcept
{
    return wavetableBank.loadRomImage(data.getData(), data.getSize());
}

bool WaldorfEngine::loadWaveSetUserTables(const juce::MemoryBlock& data) noexcept
{
    // A native Wave SET places its four 256-byte STT-format tables directly
    // after the two Performance banks. Each MIDI key has a destination-note
    // byte followed by a detune byte (14..114 means -50..+50 cents).
    static constexpr size_t tuningBankOffset = 0x42e7cu;
    static constexpr size_t tableSize = 256u;
    static constexpr size_t tableCount = 4u;
    if (data.getData() != nullptr
        && data.getSize() >= tuningBankOffset + tableCount * tableSize)
    {
        const auto* bytes = static_cast<const uint8_t*>(data.getData());
        auto decoded = std::make_shared<UserTuningBank>();
        for (size_t table = 0; table < tableCount; ++table)
        {
            const auto* source = bytes + tuningBankOffset + table * tableSize;
            for (size_t key = 0; key < 128u; ++key)
            {
                const auto destinationNote = static_cast<int>(source[key * 2u] & 0x7fu);
                const auto detuneCents
                    = juce::jlimit(14, 114,
                                   static_cast<int>(source[key * 2u + 1u] & 0x7fu))
                      - 64;
                (*decoded)[table][key]
                    = static_cast<float>((destinationNote - static_cast<int>(key)) * 100
                                         + detuneCents);
            }
        }
        std::atomic_store_explicit(
            &userTuningBank,
            std::static_pointer_cast<const UserTuningBank>(decoded),
            std::memory_order_release);
    }
    return wavetableBank.loadWaveSetUserTables(data.getData(), data.getSize());
}

void WaldorfEngine::applyFirmwareHardwareWrite(int board, uint32_t address,
                                               uint8_t value) noexcept
{
    if (board < 0 || board >= voiceBoardCount)
        return;
    const auto card = static_cast<size_t>(board);
    if (address >= 0x980000u && address < 0x980200u)
        firmwareAsicRegisters[card][address - 0x980000u] = value;
    else if (address >= 0x880000u && address < 0x880100u)
        firmwareCvRegisters[card][0][address - 0x880000u] = value;
    else if (address >= 0x8a0000u && address < 0x8a0100u)
        firmwareCvRegisters[card][1][address - 0x8a0000u] = value;
    else
        return;
    ++appliedFirmwareWrites;
}

WaldorfEngine::Voice& WaldorfEngine::chooseVoice()
{
    if (const auto inactive = std::find_if(voices.begin(), voices.end(),
                                           [](const auto& voice) { return !voice.active; });
        inactive != voices.end())
        return *inactive;

    // Dynamic allocation protects notes whose keys are still held. Released
    // and sustain-held tails are the hardware-like first candidates for reuse.
    if (const auto released = std::min_element(
            voices.begin(), voices.end(), [](const auto& a, const auto& b) {
                if (a.keyDown != b.keyDown)
                    return !a.keyDown;
                return a.startOrder < b.startOrder;
            });
        released != voices.end() && !released->keyDown)
        return *released;

    return *std::min_element(voices.begin(), voices.end(), [](const auto& a, const auto& b) {
        return a.startOrder < b.startOrder;
    });
}
} // namespace wave::dsp
