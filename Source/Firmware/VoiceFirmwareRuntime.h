#pragma once

#include "M68000.h"
#include "SharedFirmwareMemory.h"

#include <juce_core/juce_core.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace wave::firmware
{
class VoiceFirmwareRuntime final : private M68000Bus
{
public:
    struct HardwareWrite
    {
        uint64_t cycle = 0;
        uint32_t address = 0;
        uint8_t value = 0;
    };

    void attachSharedMemory(SharedFirmwareMemory& memory) noexcept { sharedMemory = &memory; }
    bool setBoardIndex(int index) noexcept;
    bool loadAndReset(const juce::MemoryBlock& voiceImage);
    int runCycles(int cycles);
    int runForAudioSamples(int samples, double sampleRate);
    [[nodiscard]] std::vector<HardwareWrite> consumeHardwareWrites();
    [[nodiscard]] const std::vector<HardwareWrite>& pendingHardwareWrites() const noexcept
    {
        return hardwareWrites;
    }
    void clearHardwareWrites() noexcept { hardwareWrites.clear(); }

    [[nodiscard]] bool isLoaded() const noexcept { return loaded; }
    [[nodiscard]] bool reachedServiceLoop() const noexcept;
    [[nodiscard]] bool waitingForMasterAcknowledgement() const noexcept;
    [[nodiscard]] uint32_t programCounter() const noexcept { return cpu.programCounter(); }
    [[nodiscard]] uint32_t unmappedReadCount() const noexcept { return unmappedReads; }
    [[nodiscard]] uint32_t lastUnmappedReadAddress() const noexcept { return lastUnmappedRead; }
    [[nodiscard]] uint8_t controlLatchValue() const noexcept { return controlLatch; }
    [[nodiscard]] uint16_t asicWord(uint32_t offset) const noexcept;
    [[nodiscard]] uint16_t cvWord(int bank, uint32_t offset) const noexcept;
    [[nodiscard]] uint8_t sharedByte(uint32_t offset) const noexcept;
    [[nodiscard]] uint8_t waveRamByte(int ram, uint32_t offset) const noexcept
    {
        return (ram == 0 || ram == 1) && offset < waveRamSize
                   ? waveformRam[static_cast<size_t>(ram)][offset]
                   : 0xffu;
    }
    [[nodiscard]] uint64_t emulatedCycleCount() const noexcept { return emulatedCycles; }
    [[nodiscard]] uint64_t controlTickCount() const noexcept { return controlTicks; }
    [[nodiscard]] int getBoardIndex() const noexcept { return boardIndex; }

    static constexpr size_t loaderHeaderSize = 0x20;
    static constexpr uint32_t localRamSize = 0x10000;
    static constexpr uint32_t sharedProgramBase = 0x100000;
    static constexpr uint32_t sharedProgramSize = SharedFirmwareMemory::programSize;
    static constexpr uint32_t sharedWorkBase = 0x140000;
    // Two 64 KB wave RAMs (A, B) with a write-both alias. WDV.SYS stores 8-bit
    // samples on odd bytes at 0x060001 + (bank << 13); the alias is read back
    // as RAM A (alias read behaviour is not established by the firmware).
    static constexpr uint32_t waveRamABase = 0x040000;
    static constexpr uint32_t waveRamBBase = 0x050000;
    static constexpr uint32_t waveRamBothBase = 0x060000;
    static constexpr uint32_t waveRamSize = 0x10000;
    static constexpr uint32_t cvBankABase = 0x880000;
    static constexpr uint32_t cvBankBBase = 0x8a0000;
    static constexpr uint32_t cvWindowSize = 0x10000;
    static constexpr uint32_t boardControlAddress = 0x800001;
    // Firmware-visible black-box register window. The name describes the
    // physical target only; register storage does not model chip internals.
    static constexpr uint32_t asicBase = 0x980000;
    static constexpr uint32_t asicWindowSize = 0x10000;
    static constexpr int maximumBoardCount = 3;

private:
    [[nodiscard]] uint8_t read8(uint32_t address) noexcept override;
    void write8(uint32_t address, uint8_t value) noexcept override;
    [[nodiscard]] static uint32_t bigEndian32(const uint8_t* bytes) noexcept;

    M68000 cpu;
    std::array<uint8_t, localRamSize> localRam{};
    SharedFirmwareMemory ownedSharedMemory;
    SharedFirmwareMemory* sharedMemory = &ownedSharedMemory;
    std::array<uint8_t, asicWindowSize> asicRegisters{};
    std::array<std::array<uint8_t, waveRamSize>, 2> waveformRam{};
    std::array<std::array<uint8_t, cvWindowSize>, 2> cvRegisters{};
    uint8_t controlLatch = 0;
    int boardIndex = 0;
    uint8_t boardStatus = 0x06;
    uint32_t unmappedReads = 0;
    uint32_t lastUnmappedRead = 0;
    uint64_t emulatedCycles = 0;
    double audioCycleRemainder = 0.0;
    uint32_t cyclesUntilControlTick = 0;
    uint64_t controlTicks = 0;
    std::vector<HardwareWrite> hardwareWrites;
    bool loaded = false;

    static constexpr uint32_t cpuClockHz = 16000000;
    static constexpr uint32_t controlTickRateHz = 1000;
};
} // namespace wave::firmware
