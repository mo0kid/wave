#pragma once

#include "M68000.h"
#include "Dp8473.h"
#include "SharedFirmwareMemory.h"
#include "Via6522.h"

#include <juce_core/juce_core.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>

namespace wave::firmware
{
class MasterFirmwareRuntime final : private M68000Bus
{
public:
    void attachSharedMemory(SharedFirmwareMemory& memory) noexcept { sharedMemory = &memory; }
    bool loadAndStart(const juce::MemoryBlock& masterImage);
    int runCycles(int cycles);
    int runForAudioSamples(int samples, double sampleRate);
    bool runOs1700InitialisationFileLoad();
    bool runOs1700VoiceBoardLoaderHandoff(int voiceBoardCount);
    bool completeOs1700VoiceBoardServiceHandoff();
    bool setInitialisationRecords(std::span<const uint8_t> sound,
                                  std::span<const uint8_t> performance) noexcept;
    void useFirmwareInitialisationRecords() noexcept
    {
        customInitialisationRecords = false;
    }
    bool installEditRecords(std::span<const uint8_t> sound,
                            std::span<const uint8_t> performance) noexcept;
    bool installPerformanceBank(std::span<const uint8_t> performances) noexcept;
    bool installSoundBank(std::span<const uint8_t> sounds) noexcept;
    bool requestPerformanceSelection(int programIndex) noexcept;
    bool refreshCurrentScreenFromFirmware()
    {
        return loaded && redrawCurrentScreenWithFirmware();
    }
    juce::Result mountDiskImage(const juce::File& imageFile);
    juce::Result flushDiskImage();
    juce::Result ejectDiskImage();
    [[nodiscard]] bool hasMountedDiskImage() const noexcept { return floppy.isMounted(); }
    [[nodiscard]] bool mountedDiskImageIsWritable() const noexcept
    {
        return floppy.isWritable();
    }
    [[nodiscard]] bool mountedDiskImageIsDirty() const noexcept { return floppy.isDirty(); }
    [[nodiscard]] uint64_t mountedDiskBytesRead() const noexcept
    {
        return floppy.mediaBytesRead();
    }
    [[nodiscard]] juce::File mountedDiskImageFile() const { return floppy.mountedFile(); }
    [[nodiscard]] juce::String mountedDiskDescription() const
    {
        return floppy.mountedDescription();
    }
    [[nodiscard]] juce::MemoryBlock mountedDiskImageSnapshot() const
    {
        return floppy.mountedImageSnapshot();
    }

    void setPanelButton(int buttonId, bool pressed) noexcept;
    void setPanelAnalog(int controlId, float normalised) noexcept;
    void setPerformanceFaderValue(int faderIndex, uint8_t value) noexcept;
    void pushPanelEvent(uint8_t status, uint8_t data1, uint8_t data2) noexcept;
    int discardPendingPanelButtonEvents(int buttonId) noexcept;
    [[nodiscard]] bool panelEventPending(uint8_t status, uint8_t data1,
                                         uint8_t data2) const noexcept;
    bool releasePanelEventLatch(int buttonId) noexcept;
    bool navigatePageWithFirmware(bool forwards) noexcept;
    bool stepDiskMenuWithFirmware(bool forwards) noexcept;
    bool stepStoreDestinationWithFirmware(bool forwards) noexcept;
    [[nodiscard]] bool panelActionBitActive(int buttonId) const noexcept;
    bool releasePanelActionBit(int buttonId) noexcept;
    void pushMidiByte(int port, uint8_t value) noexcept;
    void pushKeyboardByte(uint8_t value) noexcept;
    [[nodiscard]] bool panelLed(int ledId) const noexcept;
    [[nodiscard]] uint64_t timerTickCount() const noexcept { return timerTicks; }
    [[nodiscard]] uint64_t fastForwardedCycleCount() const noexcept
    {
        return fastForwardedCycles;
    }
    [[nodiscard]] uint64_t emulatedCycleCount() const noexcept
    {
        return emulatedCycles.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool isLoaded() const noexcept { return loaded; }
    [[nodiscard]] bool completedColdHardwareSetup() const noexcept
    {
        return loaded && coldHardwareSetupComplete;
    }
    [[nodiscard]] bool completedVoiceBoardLoaderHandoff() const noexcept
    {
        return voiceBoardHandoffComplete;
    }
    [[nodiscard]] uint32_t programCounter() const noexcept { return cpu.programCounter(); }
    [[nodiscard]] uint32_t stackPointer() const noexcept { return cpu.stackPointer(); }
    [[nodiscard]] uint32_t dataRegister(int index) const noexcept
    {
        return cpu.dataRegister(index);
    }
    [[nodiscard]] uint32_t addressRegister(int index) const noexcept
    {
        return cpu.addressRegister(index);
    }
    [[nodiscard]] uint16_t statusRegister() const noexcept { return cpu.statusRegister(); }
    [[nodiscard]] uint32_t unmappedReadCount() const noexcept { return unmappedReads; }
    [[nodiscard]] uint32_t lastUnmappedReadAddress() const noexcept { return lastUnmappedRead; }
    [[nodiscard]] uint32_t lastUnmappedReadProgramCounter() const noexcept
    {
        return lastUnmappedReadPc;
    }
    [[nodiscard]] uint8_t ioByte(uint32_t address) const noexcept;
    [[nodiscard]] uint8_t localByte(uint32_t address) const noexcept;
    [[nodiscard]] uint8_t sharedProgramByte(uint32_t offset) const noexcept;
    [[nodiscard]] std::optional<int> currentPerformanceId() const noexcept;
    [[nodiscard]] std::optional<uint32_t> currentPerformanceRecordOffset() const noexcept;
    [[nodiscard]] std::optional<int> currentPerformanceInstrument() const noexcept;
    [[nodiscard]] std::optional<int> currentInstrumentEditTarget() const noexcept;
    [[nodiscard]] std::optional<int> currentInstrumentEditPage() const noexcept;
    [[nodiscard]] std::optional<uint32_t> currentSoundRecordOffset() const noexcept;
    [[nodiscard]] std::optional<uint32_t> performanceInstrumentSoundRecordOffset(
        int instrument) const noexcept;
    [[nodiscard]] uint8_t currentSoundRecordByte(uint32_t offset) const noexcept;
    bool writeCurrentSoundRecordByte(uint32_t offset, uint8_t value) noexcept;
    bool writePerformanceInstrumentByte(int instrument, uint32_t offset,
                                        uint8_t value) noexcept;
    bool installPerformanceInstrumentSoundRecord(
        int instrument, std::span<const uint8_t, 256> sound) noexcept;
    bool installCurrentPerformanceRecord(
        std::span<const uint8_t, 512> performance) noexcept;
    void setFilterCalibrationCode(int voice, uint16_t code) noexcept;
    [[nodiscard]] uint16_t filterCalibrationCode(int voice) const noexcept;
    [[nodiscard]] uint8_t sharedWorkByte(uint32_t offset) const noexcept;
    [[nodiscard]] uint8_t panelAnalogByte(int controlId) const noexcept;
    void rebaseRelativePanelPot(int diagnosticCode, int adcChannel) noexcept;
    [[nodiscard]] bool panelButtonPressed(int buttonId) const noexcept;
    [[nodiscard]] bool panelButtonRequestedDown(int buttonId) const noexcept;
    [[nodiscard]] uint8_t lcdVideoByte(uint32_t offset) const noexcept;
    [[nodiscard]] uint8_t lcdDisplayPage() const noexcept;
    [[nodiscard]] uint64_t lcdVideoWriteCount() const noexcept
    {
        return lcdWrites.load(std::memory_order_relaxed);
    }
    [[nodiscard]] bool installedSyntheticInitialisationRecords() const noexcept
    {
        return syntheticInitialisationInstalled;
    }
    [[nodiscard]] bool loadedSyntheticInitialisationFiles() const noexcept
    {
        return loadedInitialisationMask == 0x03u;
    }

    static constexpr uint32_t imageBase = 0x001000;
    static constexpr uint32_t coldEntry = imageBase + 0x00000c;
    static constexpr uint32_t initialStackPointer = 0x000ffffe;
    static constexpr uint32_t localRamSize = 0x100000;
    static constexpr uint32_t sharedProgramBase = 0x100000;
    static constexpr uint32_t sharedProgramSize = SharedFirmwareMemory::programSize;
    static constexpr uint32_t sharedWorkBase = 0x140000;
    static constexpr uint32_t sharedWorkSize = SharedFirmwareMemory::workSize;
    static constexpr uint32_t lcdVideoBase = 0xa00000;
    static constexpr uint32_t lcdVideoWindowSize = 0x4000;
    using LcdVideoSnapshot = std::array<uint8_t, lcdVideoWindowSize>;
    [[nodiscard]] LcdVideoSnapshot lcdVideoSnapshot() const noexcept;

private:
    bool runPanelCallbacksWithFirmware(uint32_t callbackTable) noexcept;
    struct VirtualInitialisationFile
    {
        bool open = false;
        size_t offset = 0;
    };

    struct Acia6850
    {
        static constexpr size_t fifoCapacity = 1024;
        uint8_t control = 0x03;
        uint8_t status = 0x02; // Transmit data register empty.
        std::array<uint8_t, fifoCapacity> receive{};
        std::array<uint8_t, fifoCapacity> transmit{};
        size_t receiveRead = 0;
        size_t receiveWrite = 0;
        size_t receiveCount = 0;
        size_t transmitRead = 0;
        size_t transmitWrite = 0;
        size_t transmitCount = 0;

        void reset() noexcept;
        [[nodiscard]] uint8_t readStatus() const noexcept;
        [[nodiscard]] uint8_t readData() noexcept;
        void writeControl(uint8_t value) noexcept;
        void writeData(uint8_t value) noexcept;
        void pushReceive(uint8_t value) noexcept;
    };

    [[nodiscard]] uint8_t read8(uint32_t address) noexcept override;
    void write8(uint32_t address, uint8_t value) noexcept override;
    [[nodiscard]] bool usesInstructionInterception() const noexcept override
    {
        return initialisationLoaderActive || startupContinuationActive
               || displayRefreshActive;
    }
    bool interceptInstruction(M68000& activeCpu, uint32_t programCounter) noexcept override;
    bool redrawCurrentScreenWithFirmware();
    bool redrawPerformanceFaderWithFirmware(int faderIndex);
    void releaseExpiredPanelButtons(uint64_t currentCycle) noexcept;
    [[nodiscard]] static uint32_t bigEndian32(const uint8_t* bytes) noexcept;
    [[nodiscard]] static bool isDecodedIo(uint32_t address) noexcept;

    M68000 cpu;
    SharedFirmwareMemory ownedSharedMemory;
    SharedFirmwareMemory* sharedMemory = &ownedSharedMemory;
    std::array<std::atomic<uint8_t>, lcdVideoWindowSize> lcdVideoRam{};
    std::array<std::atomic<uint16_t>, 8> panelSwitchWords{};
    std::array<std::atomic<uint64_t>, 128> panelReleaseCycles{};
    std::array<std::atomic<bool>, 128> panelReleasePending{};
    std::array<std::atomic<bool>, 128> panelButtonRequested{};
    std::array<std::atomic<uint16_t>, 8> panelLedWords{};
    std::array<std::atomic<uint8_t>, 128> panelAnalogValues{};
    std::array<Acia6850, 3> serialPorts;
    Via6522 via;
    Dp8473 floppy;
    std::unordered_map<uint32_t, uint8_t> ioRegisters;
    uint8_t selectedAnalogChannel = 0;
    std::atomic<uint64_t> emulatedCycles { 0 };
    uint64_t timerTicks = 0;
    uint64_t fastForwardedCycles = 0;
    double audioCycleRemainder = 0.0;
    uint32_t unmappedReads = 0;
    uint32_t lastUnmappedRead = 0;
    uint32_t lastUnmappedReadPc = 0;
    std::atomic<uint64_t> lcdWrites { 0 };
    std::atomic<uint8_t> lcdPage { 0 };
    std::array<uint8_t, 256> syntheticInitSound{};
    std::array<uint8_t, 512> syntheticInitPerformance{};
    VirtualInitialisationFile initSoundFile;
    VirtualInitialisationFile initPerformanceFile;
    uint8_t loadedInitialisationMask = 0;
    bool syntheticInitialisationInstalled = false;
    bool initialisationLoaderActive = false;
    bool startupContinuationActive = false;
    bool displayRefreshActive = false;
    bool displayRefreshComplete = false;
    int pendingPerformanceRefresh = -1;
    bool voiceLoaderReached = false;
    bool customInitialisationRecords = false;
    bool voiceBoardHandoffComplete = false;
    int activeVoiceBoardCount = 0;
    bool coldHardwareSetupComplete = false;
    bool loaded = false;

    static constexpr uint32_t masterClockHz = 16000000;
    static constexpr uint64_t minimumPanelPressCycles
        = static_cast<uint64_t>(masterClockHz) * 30u / 1000u;
};
} // namespace wave::firmware
