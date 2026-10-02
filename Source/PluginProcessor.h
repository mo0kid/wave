#pragma once

#include "Dsp/WaldorfEngine.h"
#include "Firmware/FirmwareBundle.h"
#include "Firmware/MasterFirmwareRuntime.h"
#include "Firmware/SharedFirmwareMemory.h"
#include "Firmware/VoiceFirmwareRuntime.h"
#include "Presets/WaveFactorySet.h"
#include "PanelWiring.h"
#include "WaveParameters.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include <atomic>
#include <array>
#include <limits>
#include <memory>
#include <optional>

class WaveEmulationAudioProcessor final : public juce::AudioProcessor,
                                          private juce::AsyncUpdater
{
public:
    enum class InstrumentButtonMode
    {
        normal,
        mute,
        solo
    };

    explicit WaveEmulationAudioProcessor(const juce::File& firmwarePreferenceFile = {});
    ~WaveEmulationAudioProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void audioWorkgroupContextChanged(const juce::AudioWorkgroup& workgroup) override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 600.0; }

    int getNumPrograms() override;
    int getCurrentProgram() override
    {
        return currentProgram.load(std::memory_order_acquire);
    }
    void setCurrentProgram(int) override;
    bool selectFactoryPerformance(int bank, int performanceNumber);
    void selectFactoryBank(int bank);
    void stepFactoryPerformance(int delta);
    bool selectPerformanceInstrument(int instrument);
    bool pressPerformanceInstrumentButton(int instrument);
    void togglePerformanceMuteMode();
    void togglePerformanceSoloMode();
    [[nodiscard]] int getSelectedPerformanceInstrument() const noexcept;
    [[nodiscard]] wave::dsp::WaldorfEngine::PerformanceSnapshot
        getCurrentPerformanceSnapshot() const noexcept
    {
        const auto performance = std::atomic_load_explicit(
            &factoryPerformance, std::memory_order_acquire);
        return performance != nullptr
                   ? *performance
                   : wave::dsp::WaldorfEngine::PerformanceSnapshot {};
    }
    [[nodiscard]] bool isPerformanceInstrumentActive(int instrument) const noexcept;
    [[nodiscard]] bool isPerformanceInstrumentMuted(int instrument) const noexcept;
    [[nodiscard]] bool isPerformanceInstrumentAudible(int instrument) const noexcept;
    [[nodiscard]] InstrumentButtonMode getInstrumentButtonMode() const noexcept
    {
        return static_cast<InstrumentButtonMode>(
            instrumentButtonMode.load(std::memory_order_acquire));
    }
    const juce::String getProgramName(int) override;
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destinationData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    wave::firmware::Bundle::Report loadFirmware(const juce::File& directoryOrImage,
                                               bool rememberForNewInstances = true);
    bool loadWavetableRom(const juce::File& image);
    juce::Result mountDiskImage(const juce::File& image);
    juce::Result createBlankDiskImage(const juce::File& destination);
    juce::Result createDiskImageFromWaveSetup(const juce::File& setup,
                                              const juce::File& destination);
    juce::Result flushMountedDiskImage();
    juce::Result saveMountedDiskImageAs(const juce::File& destination);
    juce::Result ejectDiskImage();
    // OS 1.700 voice-allocation fix (MasterFirmwareRuntime::setVoiceAllocationFix); off by default, saved with the plugin state.
    void setVoiceAllocationFix(bool enabled);
    [[nodiscard]] bool getVoiceAllocationFix() const;
    void resetToColdStart();
    [[nodiscard]] bool hasMountedDiskImage() const noexcept
    {
        return masterFirmware.hasMountedDiskImage();
    }
    [[nodiscard]] bool mountedDiskImageIsWritable() const noexcept
    {
        return masterFirmware.mountedDiskImageIsWritable();
    }
    [[nodiscard]] bool mountedDiskImageIsDirty() const noexcept
    {
        return masterFirmware.mountedDiskImageIsDirty();
    }
    [[nodiscard]] juce::File getMountedDiskImageFile() const
    {
        return masterFirmware.mountedDiskImageFile();
    }
    [[nodiscard]] juce::String getMountedDiskDescription() const
    {
        return masterFirmware.mountedDiskDescription();
    }
    [[nodiscard]] juce::File getDiskImageChooserDirectory() const;
    [[nodiscard]] const wave::firmware::Bundle::Report& getFirmwareReport() const noexcept;
    [[nodiscard]] wave::presets::WaveFactorySet::Report getFactorySetReport() const noexcept
    {
        const auto set = currentPerformanceSet();
        return set != nullptr ? set->getReport()
                              : wave::presets::WaveFactorySet::Report{};
    }
    [[nodiscard]] juce::File getFirmwareDirectory() const noexcept;
    [[nodiscard]] const wave::firmware::VoiceFirmwareRuntime& getVoiceFirmwareRuntime(
        int board = 0) const noexcept
    {
        return voiceFirmwares[static_cast<size_t>(juce::jlimit(
            0, wave::dsp::WaldorfEngine::voiceBoardCount - 1, board))];
    }
    [[nodiscard]] static constexpr int getVoiceBoardCount() noexcept
    {
        return wave::dsp::WaldorfEngine::voiceBoardCount;
    }
    [[nodiscard]] const wave::firmware::MasterFirmwareRuntime& getMasterFirmwareRuntime() const noexcept
    {
        return masterFirmware;
    }
    [[nodiscard]] float getOutputPeak() const noexcept { return outputPeak.load(); }
    [[nodiscard]] int getActiveVoiceCount() const noexcept { return engine.activeVoiceCount(); }
    [[nodiscard]] int getHeldVoiceCount() const noexcept { return engine.heldVoiceCount(); }
    [[nodiscard]] int getFirstActiveMidiNote() const noexcept
    {
        return engine.firstActiveMidiNote();
    }
    [[nodiscard]] auto getVoiceStates() const noexcept { return engine.voiceStates(); }
    [[nodiscard]] bool isMidiNoteActive(int midiNote) const noexcept
    {
        return midiNote >= 0 && midiNote < 128
               && activeMidiNoteChannels[static_cast<size_t>(midiNote)].load(
                      std::memory_order_acquire) != 0;
    }
    [[nodiscard]] float getFirstActiveWavePosition() const noexcept
    {
        return engine.firstActiveWavePosition();
    }
    [[nodiscard]] float getFirstActiveLfoValue(int lfo) const noexcept
    {
        return engine.firstActiveLfoValue(lfo);
    }
    [[nodiscard]] float getFirstActivePitchModulation(int oscillator = 0) const noexcept
    {
        return engine.firstActivePitchModulation(oscillator);
    }
    [[nodiscard]] wave::dsp::WaldorfEngine::VoiceProbe probeCurrentVoice(
        int voice, int midiNote = 60, float velocity = 0.8f, int samples = 48000);
    [[nodiscard]] wave::dsp::WaldorfEngine::VoiceProbe probeCurrentLayerVoice(
        int voice, int layer, int midiNote = 60, float velocity = 0.8f,
        int samples = 48000, uint64_t order = 1);
    void noteOnFromUi(int midiNote, float velocity = 0.8f);
    void noteOffFromUi(int midiNote);
    void allSoundOffFromUi();
    void setPitchWheelFromUi(float normalised);
    void setModWheelFromUi(float normalised);
    void setFreeWheelFromUi(float normalised);
    [[nodiscard]] float getPitchWheelForUi() const noexcept
    {
        return static_cast<float>(keyboardPitchWheel.load(std::memory_order_acquire))
               / 16383.0f;
    }
    [[nodiscard]] float getModWheelForUi() const noexcept
    {
        return static_cast<float>(keyboardModWheel.load(std::memory_order_acquire))
               / 127.0f;
    }
    [[nodiscard]] float getFreeWheelForUi() const noexcept
    {
        return static_cast<float>(keyboardFreeWheel.load(std::memory_order_acquire))
               / 127.0f;
    }
    void setMidiInputActsAsLocalKeyboard(bool shouldActAsLocalKeyboard) noexcept
    {
        midiInputActsAsLocalKeyboard.store(shouldActAsLocalKeyboard,
                                           std::memory_order_release);
    }
    [[nodiscard]] int getKeyboardOctaveShift() const noexcept
    {
        return keyboardOctaveShift.load(std::memory_order_acquire);
    }
    [[nodiscard]] juce::File getRememberedPanelSkin() const;
    juce::Result rememberPanelSkin(const juce::File& file);
    [[nodiscard]] bool getRememberedTabbedLayout() const;
    void rememberTabbedLayout(bool tabbed);
    bool setPanelButton(int buttonId, bool pressed) noexcept;
    void setKeyboardControllerButton(uint8_t asciiCode, bool pressed) noexcept;
    [[nodiscard]] int getFirmwareOscillatorOctave(int oscillator) const noexcept;
    void setPanelAnalog(int controlId, float normalised) noexcept;
    void setPanelPotValue(const juce::String& parameterId, float normalised) noexcept;
    [[nodiscard]] std::optional<float> getPanelPotValue(
        const juce::String& parameterId) const noexcept;
    void turnPanelEncoder(int encoderId, int steps) noexcept;
    void beginPanelFaderGesture(int faderIndex, bool performanceMode = true);
    void setPanelFader(int faderIndex, int controlId, float normalised,
                       bool performanceMode = true);
    void endPanelFaderGesture(int faderIndex, bool performanceMode = true);
    [[nodiscard]] float getPanelFaderValue(int faderIndex) const noexcept;
    [[nodiscard]] uint8_t getPerformanceFadersTouchedMask() const noexcept
    {
        return performanceFadersTouched.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool getPanelLed(int ledId) const noexcept;
    [[nodiscard]] int getPanelSelectedMode() const noexcept
    {
        return panelSelectedMode.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool isPanelModeDisplayTransitionActive() const noexcept
    {
        return panelModeDisplayTransitionActive.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool isFirmwareRequesterActive() const noexcept
    {
        return firmwareRequesterActive.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool isInstrumentZoningPageActive() const noexcept
    {
        return instrumentZoningPageActive.load(std::memory_order_acquire);
    }
    [[nodiscard]] int getInstrumentEditPage() const noexcept
    {
        return instrumentEditPage.load(std::memory_order_acquire);
    }
    [[nodiscard]] const wave::dsp::WavetableBank& getWavetableBank() const noexcept
    {
        return engine.getWavetableBank();
    }

    juce::AudioProcessorValueTreeState parameters;

private:
    void removeRestoredHostStateDisk() noexcept;

    struct FirmwareSelectedSoundState
    {
        int performance = -1;
        int instrument = -1;
        wave::parameters::Snapshot sound;
    };

    struct PerformanceInstrumentSoundSeed
    {
        int performance = -1;
        std::array<std::array<uint8_t, wave::presets::WaveFactorySet::soundSize>, 8>
            records {};
        std::array<bool, 8> valid {};
    };

    // Immutable audio-thread publication of the firmware-owned edit buffers.
    // Host state reads this instead of racing the emulated CPU's shared SRAM.
    struct HostMachineSnapshot
    {
        int performance = -1;
        int selectedInstrument = -1;
        std::array<uint8_t, wave::presets::WaveFactorySet::performanceSize>
            performanceRecord {};
        std::array<std::array<uint8_t, wave::presets::WaveFactorySet::soundSize>, 8>
            soundRecords {};
        std::array<bool, 8> soundRecordValid {};
    };

    const juce::File firmwarePreferenceFile;
    bool loadRememberedFirmware();
    wave::dsp::WaldorfEngine engine;
    wave::firmware::Bundle firmware;
    wave::firmware::SharedFirmwareMemory sharedFirmwareMemory;
    wave::firmware::MasterFirmwareRuntime masterFirmware;
    std::array<wave::firmware::VoiceFirmwareRuntime,
               wave::dsp::WaldorfEngine::voiceBoardCount> voiceFirmwares;
    std::shared_ptr<const wave::presets::WaveFactorySet> embeddedFactorySet;
    std::shared_ptr<const wave::presets::WaveFactorySet> activeFactorySet;
    std::shared_ptr<const wave::dsp::WaldorfEngine::PerformanceSnapshot> factoryPerformance;
    std::shared_ptr<const FirmwareSelectedSoundState> latestFirmwareSelectedSound;
    std::shared_ptr<const PerformanceInstrumentSoundSeed> instrumentSoundSeed;
    std::shared_ptr<const PerformanceInstrumentSoundSeed> appliedInstrumentSoundSeed;
    std::shared_ptr<const HostMachineSnapshot> latestHostMachineSnapshot;
    std::shared_ptr<const HostMachineSnapshot> pendingHostMachineRestore;
    std::array<std::array<uint8_t, wave::presets::WaveFactorySet::soundSize>, 8>
        activeInstrumentSoundRecords {};
    std::array<bool, 8> activeInstrumentSoundRecordValid {};
    std::array<bool, 8> activeInstrumentSoundRecordReconciled {};
    std::array<bool, 8> activeInstrumentSoundRecordUsesImportEncoding {};
    int activeInstrumentSoundPerformance = -1;
    uint64_t lastPublishedFirmwareSoundHash = 0;
    uint64_t lastPublishedHostMachineHash = 0;
    int lastPublishedFirmwareSoundPerformance = -1;
    int lastPublishedFirmwareSoundInstrument = -1;
    std::atomic<float> outputPeak { 0.0f };
    std::atomic<uint64_t> firmwareHardwareWrites { 0 };
    std::atomic<int> pendingFirmwareProgram { -1 };
    std::atomic<int> pendingFirmwareInstrument { -1 };
    int activeInstrumentPanelDiagnosticCode = -1;
    int activeInstrumentPanelTarget = -1;
    bool activeInstrumentPanelPressSent = false;
    int activeInstrumentPanelRetryBlocks = 0;
    int activeInstrumentPanelHoldBlocks = 0;
    std::atomic<int> pendingInstrumentPageSelection { -1 };
    std::atomic<bool> cancelInstrumentPanelEvents { false };
    std::atomic<bool> cancelInstrumentPageSelection { false };
    std::atomic<bool> cancelActiveInstrumentSelection { false };
    std::atomic<bool> instrumentZoningPageActive { false };
    std::atomic<int> instrumentEditPage { 0 };
    std::atomic<bool> firmwareRequesterActive { false };
    std::atomic<bool> firmwareDiskNameEditorActive { false };
    std::atomic<bool> instrumentPageReady { false };
    std::atomic<bool> startInstrumentPageSelectionDelay { false };
    int scheduledInstrumentPageSelection = -1;
    int scheduledInstrumentPageDelayBlocks = 0;
    std::atomic<int> pendingFirmwareSoftButton { -1 };
    std::atomic<bool> pendingPanelCancel { false };
    std::atomic<bool> cancelFirmwareSoftButtonEvents { false };
    int activeSoftButtonDiagnosticCode = -1;
    bool activeSoftButtonPressSent = false;
    bool activeSoftButtonAccepted = false;
    bool activeSoftButtonReleaseSent = false;
    int activeSoftButtonHoldBlocks = 0;
    int activeSoftButtonRetryCount = 0;
    wave::firmware::MasterFirmwareRuntime::LcdVideoSnapshot
        activeSoftButtonInitialLcd {};
    std::atomic<int> pendingFirmwarePageButton { -1 };
    std::atomic<int> pendingFirmwareModeButton { -1 };
    std::atomic<bool> panelModeDisplayTransitionActive { false };
    std::atomic<bool> panelModeDisplayAwaitingDispatch { false };
    int activeModeButtonDiagnosticCode = -1;
    bool activeModeButtonPressSent = false;
    bool activeModeButtonReleaseSent = false;
    int activeModeButtonHoldBlocks = 0;
    int activeModeButtonSettleBlocks = 0;
    int activeModeButtonRetryCount = 0;
    uint32_t activeModeButtonInitialScreenCallback = 0;
    wave::firmware::MasterFirmwareRuntime::LcdVideoSnapshot
        activeModeButtonInitialLcd {};
    uint64_t modeDisplayWriteStartCount = 0;
    uint64_t modeDisplayLastWriteCount = 0;
    int modeDisplayQuietSamples = 0;
    bool modeDisplayObservedWrites = false;
    bool modeDisplayObservedCallbackChange = false;
    std::atomic<int> pendingEngineProgram { -1 };
    std::atomic<int> pendingPanelPerformance { -1 };
    std::atomic<uint64_t> pendingDiskSetBytes { 0 };
    std::atomic<bool> pendingDiskSetActivation { false };
    // Remains armed until the user accepts or cancels the mounted SET. The
    // byte transfer can finish before OK is pressed, so it cannot double as
    // the front-panel requester's ownership flag.
    std::atomic<bool> diskSetImportConfirmationPending { false };
    std::atomic<bool> firmwareDiskCalibrationRequesterActive { false };
    std::atomic<int> pendingManagerExitProgram { -1 };
    std::atomic<bool> returnToPerformanceAfterDiskImport { false };
    std::atomic<bool> returnToPerformanceAfterStoreExit { false };
    std::atomic<int> storeSaveMode { 39 };
    std::atomic<int> completedStoreMode { -1 };
    std::atomic<bool> storeSaveCompletionPending { false };
    const juce::String hostStateInstanceId { juce::Uuid().toString() };
    juce::File restoredHostStateDirectory;
    std::array<uint16_t, wave::dsp::WaldorfEngine::voiceCount>
        protectedFilterCalibrationCodes
            = wave::dsp::WaldorfEngine::installedFilterCalibrationCodes;
    std::atomic<bool> storeMenuActive { false };
    std::atomic<bool> diskMenuActive { false };
    std::atomic<bool> filterCalibrationServiceActive { false };
    std::atomic<bool> filterCalibrationServiceExitPending { false };
    std::atomic<bool> pendingFilterCalibrationServiceSeed { false };
    std::atomic<uint8_t> performanceFadersTouched { 0 };
    std::atomic<bool> midiInputActsAsLocalKeyboard { false };
    std::atomic<bool> keyboardControllerShiftDown { false };
    std::atomic<int> keyboardOctaveShift { 0 };
    // 2 means no host-state restoration is pending; valid requests are
    // the firmware's three keyboard positions -1, 0 and +1.
    std::atomic<int> pendingKeyboardOctaveRestore { 2 };
    std::atomic<int> keyboardOctaveRestoreWaitBlocks { 0 };
    std::atomic<int> keyboardPitchWheel { 8192 };
    std::atomic<int> keyboardModWheel { 0 };
    std::atomic<int> keyboardFreeWheel { 64 };
    // Performance faders and MIDI CC1 can address the same logical Wave
    // controller.  Keep an ordering token for each physical source so a
    // stationary fader cannot overwrite a subsequently moved MIDI wheel on
    // every audio block.
    std::atomic<uint64_t> controllerInputSequence { 0 };
    std::atomic<uint64_t> modWheelInputSequence { 0 };
    std::array<std::atomic<uint64_t>, 8> performanceFaderInputSequences {};
    std::array<int, 2> lastKeyboardAssignableButtonStates { -1, -1 };
    std::array<int, 128> localKeyboardTransposedNotes {};
    std::atomic<int> instrumentButtonMode {
        static_cast<int>(InstrumentButtonMode::normal)
    };
    // Even values are stable; odd values delimit the APVTS update performed
    // while switching the editable Performance Instrument.
    std::atomic<uint64_t> performanceInstrumentSelectionVersion { 0 };
    std::array<std::atomic<int>, 8> pendingFirmwareFaderValues;
    std::array<std::atomic<int>, 8> pendingInstrumentFaderEdits;
    std::array<uint8_t, 32> instrumentFaderRecordBaseline {};
    int instrumentFaderBaselinePerformance = -1;
    int instrumentFaderBaselineInstrument = -1;
    bool instrumentFaderRecordBaselineValid = false;
    std::array<std::atomic<int>, 9> pendingPanelEncoderSteps;
    std::atomic<bool> panelWavetableDataDialActive { false };
    std::atomic<int> pendingPanelStepButtonEvents { 0 };
    std::atomic<bool> cancelPanelStepButtonEvents { false };
    int queuedPanelStepButtonEvents = 0;
    int activePanelStepButtonDiagnosticCode = -1;
    int activePanelStepButtonWaitBlocks = 0;
    std::atomic<int> pendingStoreDestinationStepEvents { 0 };
    int queuedStoreDestinationStepEvents = 0;
    std::array<std::atomic<int>, wave::parameters::modulationRouteCount>
        pendingFirmwareModulationAmounts;
    std::array<std::atomic<int>, wave::parameters::modulationRouteCount>
        pendingFirmwareModulationSources;
    std::array<std::atomic<int>, wave::parameters::modulationRouteCount>
        pendingFirmwareModulationControls;
    std::array<int, wave::parameters::modulationRouteCount>
        lastFirmwareModulationSources;
    std::array<int, wave::parameters::modulationRouteCount>
        lastHostModulationSources;
    std::array<int, wave::parameters::modulationRouteCount>
        effectiveModulationSources;
    std::array<int, wave::parameters::modulationRouteCount>
        lastFirmwareModulationControls;
    std::array<int, wave::parameters::modulationRouteCount>
        lastHostModulationControls;
    std::array<int, wave::parameters::modulationRouteCount>
        effectiveModulationControls;
    std::array<int, wave::parameters::modulationRouteCount>
        lastFirmwareModulationAmounts;
    std::array<float, wave::parameters::modulationRouteCount>
        lastHostModulationAmounts;
    std::array<float, wave::parameters::modulationRouteCount>
        effectiveModulationAmounts;
    std::array<std::atomic<float>, wave::panel::visiblePots.size()>
        pendingFirmwarePanelPotValues;
    std::array<int, wave::panel::visiblePots.size()> lastFirmwarePanelPotRecordBytes;
    std::array<float, wave::panel::visiblePots.size()> lastHostPanelPotValues;
    std::array<std::atomic<float>, wave::panel::visiblePots.size()>
        programRecallPanelPotEchoValues;
    std::atomic<int> programRecallPanelPotEchoGuardBlocks { 0 };
    std::atomic<bool> resetHostPanelPotTracking { true };
    std::atomic<bool> relativePanelPotRebasePending { false };
    std::atomic<bool> firmwareSoundFeedbackSuspended { true };
    std::atomic<bool> resetPanelPotSoundRecordOffset { true };
    std::optional<uint32_t> lastPanelPotSoundRecordOffset;
    std::array<std::atomic<int>, 6> pendingFirmwareGlideValues;
    std::array<int, 6> lastFirmwareGlideRecordBytes;
    std::atomic<int> pendingFirmwareOscillatorLink { -1 };
    int lastFirmwareOscillatorLink = -1;
    int lastFirmwareOscillatorLinkLed = -1;
    // The UI and audio callback must never execute the emulated 68000
    // concurrently. The lower keyboard controller publishes its latest Rate
    // position here; advanceFirmware() commits and redraws it on the sole
    // firmware-owning audio thread.
    std::atomic<int> pendingPanelGlideRate { -1 };
    std::atomic<int> pendingFirmwareGlideSwitchClicks { 0 };
    int queuedFirmwareGlideSwitchClicks = 0;
    bool firmwareGlideSwitchTransactionActive = false;
    bool firmwareGlideSwitchReleaseSent = false;
    int firmwareGlideSwitchBaseline = -1;
    int firmwareGlideSwitchWaitBlocks = 0;
    int firmwareGlideSwitchRetryCount = 0;
    std::atomic<bool> pendingPanelGlideSwitchFeedback { false };
    std::atomic<int> confirmedPanelGlideRate { -1 };
    std::atomic<int> confirmedPanelGlideEnabled { -1 };
    int panelGlideSwitchBaseline = -1;
    int panelGlideSwitchFeedbackBlocks = 0;
    std::atomic<bool> modulationRoutingResetPending { false };
    std::array<std::atomic<bool>, 128> panelButtonDown;
    std::array<std::atomic<uint16_t>, 128> activeMidiNoteChannels;
    std::array<std::atomic<int>, 2> panelOscillatorOctaves;
    std::atomic<int> panelSelectedLfo { 0 };
    std::atomic<int> panelWaveEnvelopePage { 0 };
    std::atomic<int> panelFilterSelection { 0 };
    std::atomic<int> panelSelectedMode { 39 };
    std::atomic<int> panelSelectedEdit { -1 };
    juce::MidiMessageCollector uiMidiCollector;
    double currentSampleRate = 44100.0;
    std::atomic<int> currentProgram { 0 };
    bool firmwareKeyboardShiftDown = false;

    void advanceFirmware(int samples);
    void handleAsyncUpdate() override;
    void runFirmwareTimeline(const juce::MidiBuffer& midi, int sampleCount);
    void sendPendingPerformanceFadersToFirmware();
    void sendPendingInstrumentFadersToFirmware();
    void synchronisePerformanceInstrumentsFromFirmware() noexcept;
    void sendPendingPanelEncodersToFirmware();
    void sendPendingPanelGlideSwitchesToFirmware();
    void sendPendingPanelStepButtonsToFirmware();
    void sendPendingStoreDestinationStepsToFirmware();
    void synchroniseInstrumentSoundSeed() noexcept;
    void publishHostMachineSnapshot(
        int performance, int selectedInstrument,
        std::span<const uint8_t, wave::presets::WaveFactorySet::soundSize>
            selectedSound) noexcept;
    void applyPendingHostMachineRestore() noexcept;
    void stageFirmwareInstrumentSound(int targetInstrument) noexcept;
    void captureFirmwarePanelPotValues();
    void suspendFirmwareSoundFeedback() noexcept;
    void armPanelPotProgramRecallGuard() noexcept;
    void synchronisePanelPotInputsToParameters() noexcept;
    [[nodiscard]] std::optional<wave::parameters::ModulationRouteIndex>
    contextualFaderRoute(int faderIndex) const noexcept;
    void seedFilterCalibrationTable() noexcept;
    void applyPerformanceFaders(wave::dsp::WaldorfEngine::PerformanceSnapshot&) const;
    [[nodiscard]] std::shared_ptr<const wave::presets::WaveFactorySet>
    currentPerformanceSet() const noexcept
    {
        return std::atomic_load_explicit(&activeFactorySet,
                                         std::memory_order_acquire);
    }
    void setActivePerformanceSet(
        std::shared_ptr<const wave::presets::WaveFactorySet> set) noexcept;
    void discoverWavetableRom(const juce::File& directory);
    wave::firmware::Bundle::Report startLoadedFirmware(
        const wave::firmware::Bundle::Report& report, bool rememberDirectory);
    void loadEmbeddedPrivateRoms();
    void loadEmbeddedFactorySet();
    bool installFactoryEditRecords(int programIndex) noexcept;
    void applyFactoryProgram(int index, bool notifyFirmware);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WaveEmulationAudioProcessor)
};
