#pragma once

#include "Cem3387.h"
#include "WaldorfAsic.h"
#include "WaveOutputStage.h"
#include "WaveEnvelope.h"
#include "WdvEnvelope.h"
#include "WaveLfo.h"
#include "../WaveParameters.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace wave::dsp
{
class WaldorfEngine
{
public:
    static constexpr int voicesPerBoard = 16;
    static constexpr int voiceBoardCount = 3;
    static constexpr int voiceCount = voicesPerBoard * voiceBoardCount;
    inline static constexpr std::array<uint16_t, voiceCount>
        installedFilterCalibrationCodes {
            // Voice card 1
            2046, 2047, 2048, 2049, 2050, 2051, 2052, 2052,
            2053, 2054, 2055, 2056, 2040, 2041, 2042, 2043,
            // Voice card 2
            2044, 2044, 2045, 2046, 2047, 2048, 2049, 2050,
            2051, 2052, 2052, 2053, 2054, 2055, 2056, 2040,
            // Voice card 3
            2041, 2042, 2043, 2044, 2044, 2045, 2046, 2047,
            2048, 2049, 2050, 2051, 2052, 2052, 2053, 2054
        };

    struct PerformanceLayer
    {
        parameters::Snapshot sound;
        bool enabled = false;
        bool muted = false;
        bool soloed = false;
        int source = 0;
        int midiChannel = 0;
        int transposeSemitones = 0;
        float detuneCents = 0.0f;
        float gain = 1.0f;
        float auxGain = 0.0f;
        int audioOutput = 0;
        int keyLow = 0;
        int keyHigh = 127;
        int velocityLow = 1;
        int velocityHigh = 127;
        int velocityTable = 0;
        // Native Instrument-record byte 22. OS 1.700 exposes thirteen
        // temperaments: global, linear+, HMT, linear-, random 1..4,
        // user 1..4 and the MIDI Tuning Standard table.
        int tuningTable = 0;
        float performanceModWheel = -1.0f;
        float performanceChannelPressure = -1.0f;
        float performancePitchBend = -100.0f;
        float performanceControlX = -1.0f;
        float performanceControlY = -1.0f;
    };

    struct PerformanceSnapshot
    {
        std::array<PerformanceLayer, 8> layers{};
        int editableLayer = 0;
        int controlXController = 1;
        int controlYController = 2;
        float outputDb = -7.0f;
        float circuitAgeAmount = 0.18f;
    };

    struct VoiceProbe
    {
        float minimumCutoffHz = 0.0f;
        float maximumCutoffHz = 0.0f;
        float finalCutoffHz = 0.0f;
        double meanCutoffHz = 0.0;
        double rmsOutput = 0.0;
    };

    struct VoiceState
    {
        int voice = -1;
        int triggerNote = -1;
        int layer = -1;
        float amplifierEnvelope = 0.0f;
        float amplifierModulation = 0.0f;
        float vcaControl = 0.0f;
        float filterEnvelope = 0.0f;
        float cutoffHz = 0.0f;
        float cutoffControl = 0.0f;
        uint16_t cutoffTrim = 0;
        float glidePitch = 0.0f;
        bool active = false;
        bool keyDown = false;
        float oscillator1FrequencyHz = 0.0f;
    };

    WaldorfEngine();
    ~WaldorfEngine();

    void prepare(double sampleRate, int maximumBlockSize);
    // Called by the host between render callbacks, while all card jobs are idle.
    void setAudioWorkgroup(const juce::AudioWorkgroup& workgroup) { audioWorkgroup = workgroup; }
    void reset();
    void render(juce::AudioBuffer<float>& output, const juce::MidiBuffer& midi,
                const parameters::Snapshot& parameters);
    void render(juce::AudioBuffer<float>& output, const juce::MidiBuffer& midi,
                const juce::MidiBuffer& localKeyboardMidi,
                const parameters::Snapshot& parameters);
    void render(juce::AudioBuffer<float>& output, const juce::MidiBuffer& midi,
                const PerformanceSnapshot& performance);
    void render(juce::AudioBuffer<float>& output, const juce::MidiBuffer& midi,
                const juce::MidiBuffer& localKeyboardMidi,
                const PerformanceSnapshot& performance);

    [[nodiscard]] const WavetableBank& getWavetableBank() const noexcept { return wavetableBank; }
    bool loadWavetableRom(const juce::MemoryBlock& data) noexcept;
    bool loadWaveSetUserTables(const juce::MemoryBlock& data) noexcept;
    void applyFirmwareHardwareWrite(int board, uint32_t address, uint8_t value) noexcept;
    void applyFirmwareHardwareWrite(uint32_t address, uint8_t value) noexcept
    {
        applyFirmwareHardwareWrite(0, address, value);
    }
    [[nodiscard]] uint64_t firmwareHardwareWriteCount() const noexcept
    {
        return appliedFirmwareWrites;
    }
    [[nodiscard]] int activeVoiceCount() const noexcept;
    [[nodiscard]] int firstActiveMidiNote() const noexcept;
    [[nodiscard]] int heldVoiceCount() const noexcept;
    [[nodiscard]] std::array<VoiceState, voiceCount> voiceStates() const noexcept;
    [[nodiscard]] bool isSustainPedalDown() const noexcept { return sustainPedal; }
    [[nodiscard]] float getPitchBendSemitones() const noexcept { return pitchBendSemitones; }
    [[nodiscard]] float firstActiveWavePosition() const noexcept;
    [[nodiscard]] float firstActiveLfoValue(int lfo) const noexcept;
    [[nodiscard]] float firstActivePitchModulation(int oscillator = 0) const noexcept;
    [[nodiscard]] float firstActiveAmplifierEnvelopeValue() const noexcept;
    [[nodiscard]] float firstActiveFilterEnvelopeValue() const noexcept;
    [[nodiscard]] float firstActiveVcaControlValue() const noexcept;
    [[nodiscard]] uint16_t filterCalibrationCode(int voice) const noexcept;
    void setFilterCalibrationCode(int voice, uint16_t code) noexcept;
    // Not synchronised: call with the callback lock held (or before audio starts).
    void setQuantiseWaveLevels(bool enabled) noexcept
    {
        for (auto& voice : voices)
            voice.quantiseWaveLevels = enabled;
    }
    void setVoiceCardThreadingEnabled(bool enabled) noexcept
    {
        voiceCardThreadingEnabled.store(enabled, std::memory_order_relaxed);
    }
    [[nodiscard]] bool isVoiceCardThreadingEnabled() const noexcept
    {
        return voiceCardThreadingEnabled.load(std::memory_order_relaxed);
    }
    [[nodiscard]] uint64_t parallelVoiceCardRenderCount() const noexcept
    {
        return parallelCardRenders;
    }
    [[nodiscard]] VoiceProbe probeVoice(int voice, const PerformanceLayer& layer,
                                        int midiNote, float velocity, int samples,
                                        int layerIndex = 0, uint64_t order = 1);

private:
    struct Voice
    {
        void prepare(double sampleRate, int index);
        void reset();
        void start(int midiNote, int midiChannel, float noteVelocity, uint64_t order,
                   uint64_t triggerId, int newLayerIndex, const PerformanceLayer& layer,
                   float tunedTargetNote, float glideFromNote, float glideRate,
                   bool glideDistanceMode,
                   bool glideQuantised, float inheritedGlideStepPerSample,
                   int inheritedGlideSamplesRemaining, float modWheel,
                   float channelPressure, float pitchBend);
        void release(bool allowSustain = true);
        void updatePitch(const parameters::Snapshot& parameters, float pitchBend);
        [[nodiscard]] float baseFrequencyHz() const noexcept;
        [[nodiscard]] Cem3387::StereoSample process(const WavetableBank& bank,
                                                    const parameters::Snapshot& parameters,
                                                    float modWheel, float channelPressure,
                                                    float pitchBend);
        void advanceIdleLfos(const parameters::Snapshot& parameters,
                             int samples = 1) noexcept;
        [[nodiscard]] uint16_t filterCalibrationCode() const noexcept
        {
            return circuit.cutoffCalibrationCode();
        }
        void setFilterCalibrationCode(uint16_t code) noexcept
        {
            circuit.setCutoffCalibrationCode(code);
        }
        // Off by default; see AsicOutputMixer::levelFromRegisterBits.
        bool quantiseWaveLevels = false;

        [[nodiscard]] float sourceValue(int source, float ampEnvelope,
                                        float waveEnvelopeValue,
                                        const std::array<float, 2>& lfoValues,
                                        float modWheel, float channelPressure,
                                        float pitchBend) const noexcept;
        [[nodiscard]] float routeValue(parameters::ModulationRouteIndex route,
                                       const parameters::Snapshot& parameters,
                                       float ampEnvelope, float waveEnvelopeValue,
                                       const std::array<float, 2>& lfoValues,
                                       float modWheel, float channelPressure,
                                       float pitchBend) const noexcept;
        void updateControlSampleAndHold(const parameters::Snapshot& parameters,
                                        float ampEnvelope, float waveEnvelopeValue,
                                        const std::array<float, 2>& lfoValues,
                                        float modWheel, float channelPressure,
                                        float pitchBend) noexcept;
        void updateControlComparator(const parameters::Snapshot& parameters,
                                     float ampEnvelope, float waveEnvelopeValue,
                                     const std::array<float, 2>& lfoValues,
                                     float modWheel, float channelPressure,
                                     float pitchBend) noexcept;

        [[nodiscard]] float noise() noexcept;

        OscillatorChipProxy oscillator1;
        OscillatorChipProxy oscillator2;
        AsicHighpassFilter highpassFilter;
        ReconstructionStage reconstruction;
        AsicResampler asicResampler;
        Cem3387 circuit;
        WdvEnvelope envelope;
        WdvEnvelope filterEnvelope;
        WaveEnvelope waveEnvelope;
        FreeEnvelope freeEnvelope;
        std::array<WaveLfo, 2> lfos;
        WdvEnvelope::Parameters envelopeParameters;
        WdvEnvelope::Parameters filterEnvelopeParameters;
        double sampleRate = 44100.0;
        double asicClockPhase = 1.0;
        uint64_t startOrder = 0;
        uint64_t triggerId = 0;
        uint32_t noiseState = 0x12345678u;
        int note = -1;
        int triggerNote = -1;
        int triggerChannel = 1;
        int voiceIndex = 0;
        int layerIndex = 0;
        float velocity = 0.0f;
        float layerGain = 1.0f;
        float performanceDetuneCents = 0.0f;
        int tuningTableSelection = 0;
        float tolerance = 0.0f;
        bool keyDown = false;
        bool active = false;
        float currentWavePosition = 0.0f;
        std::array<float, 2> currentLfoValues{};
        std::array<float, 2> waveStartOffsets{};
        std::array<float, 2> currentPitchModulations{};
        float currentGlideNote = 0.0f;
        float targetGlideNote = 0.0f;
        float glideStepPerSample = 0.0f;
        int glideSamplesRemaining = 0;
        bool glideQuantised = false;
        float currentAmplifierEnvelope = 0.0f;
        float currentFilterEnvelope = 0.0f;
        float currentFreeEnvelope = 0.0f;
        float currentControlSampleAndHold = 0.0f;
        float currentComparatorPositive = 0.0f;
        float currentComparatorNegative = 1.0f;
        float currentControlX = 0.0f;
        float currentControlY = 0.0f;
        float currentFreeWheel = 0.0f;
        float currentButton1 = 0.0f;
        float currentButton2 = 0.0f;
        float currentVolumeController = 0.0f;
        float currentPanController = 0.0f;
        float currentBreathController = 0.0f;
        std::array<float, 2> controlLfoRates{};
        std::array<float, 2> controlLfoLevels { 1.0f, 1.0f };
        std::array<float, 2> controlWavePositions{};
        std::array<float, 2> controlWaveLevels { 0.5f, 0.5f };
        float controlNoiseLevel = 0.0f;
        float smoothedBaseCutoffSemitones = 0.0f;
        float baseCutoffSlew = 1.0f;
        float controlAnalogueCutoff = 8200.0f;
        float controlHighpassCutoff = 20.0f;
        float controlResonance = 0.18f;
        float controlPan = 0.0f;
        float controlAmplifierMod = 0.0f;
        double samplesUntilControlUpdate = 0.0;
        double controlSampleAndHoldPhase = 0.0;
        bool controlSampleAndHoldInitialised = false;
        bool baseCutoffInitialised = false;
        int filterEnvelopeDelaySamples = 0;
        int vcaDrainSamplesRemaining = 0;
        int controlFilterMode = 0;
        bool waveStartOffsetsPending = false;
        bool filterEnvelopePending = false;
        bool filterEnvelopeTriggered = false;
    };

    struct LayerGlideTrajectory
    {
        float currentNote = 0.0f;
        float targetNote = 0.0f;
        float stepPerSample = 0.0f;
        int samplesRemaining = 0;
        uint64_t triggerId = 0;
        bool initialised = false;
    };

    struct RenderingVoice
    {
        Voice* voice = nullptr;
        const parameters::Snapshot* parameters = nullptr;
        int voiceIndex = -1;
        float gain = 0.0f;
        float modWheel = 0.0f;
        float pressure = 0.0f;
        float pitchBend = 0.0f;
        float controlX = 0.0f;
        float controlY = 0.0f;
    };

    class VoiceCardWorker;

    using UserTuningBank = std::array<std::array<float, 128>, 4>;

    [[nodiscard]] float tunedNoteForLayer(const PerformanceLayer& layer,
                                          int translatedNote, uint64_t seed,
                                          int layerIndex) const noexcept;
    void synchroniseActiveVoiceTuning(const PerformanceSnapshot& performance) noexcept;

    void handleMidi(const juce::MidiMessage& message, const PerformanceSnapshot& performance,
                    bool localKeyboard);
    void renderRange(juce::AudioBuffer<float>& output, int startSample, int endSample,
                     const PerformanceSnapshot& performance);
    void renderRangeChunk(juce::AudioBuffer<float>& output, int startSample, int endSample,
                          const PerformanceSnapshot& performance);
    void renderVoiceCard(int board, int sampleCount) noexcept;
    void startVoiceCardWorkers(double sampleRate, int maximumBlockSize);
    void stopVoiceCardWorkers() noexcept;
    Voice& chooseVoice();

    WavetableBank wavetableBank;
    std::shared_ptr<const UserTuningBank> userTuningBank;
    std::array<Voice, voiceCount> voices;
    std::array<RenderingVoice, voiceCount> renderingVoices{};
    int renderingVoiceCount = 0;
    int maximumRenderBlockSize = 1;
    std::vector<float> voiceLeftScratch;
    std::vector<float> voiceRightScratch;
    std::array<std::unique_ptr<VoiceCardWorker>, voiceBoardCount - 1> cardWorkers;
    juce::AudioWorkgroup audioWorkgroup;
    std::atomic<bool> voiceCardThreadingEnabled { true };
    uint64_t parallelCardRenders = 0;
    WaveOutputStage outputStage;
    std::array<std::array<uint8_t, 0x200>, voiceBoardCount> firmwareAsicRegisters{};
    std::array<std::array<std::array<uint8_t, 0x100>, 2>, voiceBoardCount>
        firmwareCvRegisters{};
    uint64_t appliedFirmwareWrites = 0;
    uint64_t noteOrder = 0;
    uint64_t triggerOrder = 0;
    float pitchBendSemitones = 0.0f;
    float modWheelAmount = 0.0f;
    float channelPressureAmount = 0.0f;
    std::array<float, 128> midiControllerAmounts{};
    std::array<int, 8> lastPlayedNotesByLayer {};
    std::array<LayerGlideTrajectory, 8> layerGlideTrajectories {};
    bool sustainPedal = false;
};
} // namespace wave::dsp
