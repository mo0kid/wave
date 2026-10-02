#include "VoiceFirmwareRuntime.h"

#include <algorithm>

namespace wave::firmware
{
bool VoiceFirmwareRuntime::setBoardIndex(int index) noexcept
{
    // WDV.SYS scans the three active-low card-ID bits from bit 2 downwards.
    // These are therefore the physical strap values for card slots 0, 1 and 2.
    static constexpr std::array<uint8_t, maximumBoardCount> statusBySlot {
        0x06, 0x05, 0x03
    };
    if (index < 0 || index >= maximumBoardCount || loaded)
        return false;
    boardIndex = index;
    boardStatus = statusBySlot[static_cast<size_t>(index)];
    return true;
}

bool VoiceFirmwareRuntime::loadAndReset(const juce::MemoryBlock& voiceImage)
{
    loaded = false;
    localRam.fill(0);
    if (sharedMemory == &ownedSharedMemory)
        sharedMemory->clear();
    asicRegisters.fill(0);
    for (auto& ram : waveformRam)
        ram.fill(0);
    for (auto& bank : cvRegisters)
        bank.fill(0);
    controlLatch = 0;
    unmappedReads = 0;
    lastUnmappedRead = 0;
    emulatedCycles = 0;
    audioCycleRemainder = 0.0;
    cyclesUntilControlTick = cpuClockHz / controlTickRateHz;
    controlTicks = 0;
    hardwareWrites.clear();
    hardwareWrites.reserve(65536);

    if (voiceImage.getSize() <= loaderHeaderSize)
        return false;

    const auto* bytes = static_cast<const uint8_t*>(voiceImage.getData());
    const auto bodySize = voiceImage.getSize() - loaderHeaderSize;
    if (bodySize > localRam.size() || bodySize > sharedMemory->program.size()
        || bigEndian32(bytes + loaderHeaderSize) != 0x00008ffe
        || bigEndian32(bytes + loaderHeaderSize + 4) != 0x00000400)
        return false;

    std::copy_n(bytes + loaderHeaderSize, bodySize, localRam.begin());
    std::copy_n(bytes + loaderHeaderSize, bodySize, sharedMemory->program.begin());
    cpu.reset(*this);
    loaded = true;
    return true;
}

int VoiceFirmwareRuntime::runCycles(int cycles)
{
    if (!loaded || cycles <= 0)
        return 0;
    auto remaining = cycles;
    auto executedTotal = 0;
    while (remaining > 0)
    {
        const auto slice = juce::jmin(remaining, static_cast<int>(cyclesUntilControlTick));
        const auto executed = cpu.execute(*this, slice);
        if (executed <= 0)
            break;
        executedTotal += executed;
        remaining -= juce::jmin(remaining, executed);
        emulatedCycles += static_cast<uint64_t>(executed);

        if (static_cast<uint32_t>(executed) >= cyclesUntilControlTick)
        {
            ++controlTicks;
            cyclesUntilControlTick = cpuClockHz / controlTickRateHz;
            // All WDV interrupt vectors point at the externally visible ASIC
            // acknowledgement handler. The exact measured cadence remains a
            // calibration value; 1 kHz is the documented research default.
            cpu.setInterruptLevel(*this, 2);
            const auto interruptCycles = cpu.execute(*this, 48);
            emulatedCycles += static_cast<uint64_t>(juce::jmax(0, interruptCycles));
            cpu.setInterruptLevel(*this, 0);
        }
        else
        {
            cyclesUntilControlTick -= static_cast<uint32_t>(executed);
        }
    }
    return executedTotal;
}

int VoiceFirmwareRuntime::runForAudioSamples(int samples, double sampleRate)
{
    if (samples <= 0 || sampleRate <= 0.0)
        return 0;
    const auto exact = audioCycleRemainder
                       + static_cast<double>(samples) * static_cast<double>(cpuClockHz)
                             / sampleRate;
    const auto cycles = static_cast<int>(exact);
    audioCycleRemainder = exact - static_cast<double>(cycles);
    return runCycles(cycles);
}

std::vector<VoiceFirmwareRuntime::HardwareWrite> VoiceFirmwareRuntime::consumeHardwareWrites()
{
    std::vector<HardwareWrite> result;
    result.swap(hardwareWrites);
    hardwareWrites.reserve(65536);
    return result;
}

bool VoiceFirmwareRuntime::reachedServiceLoop() const noexcept
{
    if (!loaded)
        return false;
    const auto slot = static_cast<size_t>(boardIndex);
    return sharedMemory->program[0x5086 + slot] == 0xff
           && sharedMemory->program[0x508e + slot] == 0xff
           && asicWord(0x12) == 0x0004;
}

bool VoiceFirmwareRuntime::waitingForMasterAcknowledgement() const noexcept
{
    const auto slot = static_cast<size_t>(boardIndex);
    return loaded && sharedMemory->program[0x5086 + slot] == 0xff
           && sharedMemory->program[0x508a + slot] == 0x00
           && sharedMemory->program[0x508e + slot] != 0xff;
}

uint16_t VoiceFirmwareRuntime::asicWord(uint32_t offset) const noexcept
{
    if (offset + 1u >= asicRegisters.size())
        return 0xffffu;
    return static_cast<uint16_t>((static_cast<uint16_t>(asicRegisters[offset]) << 8u)
                                 | asicRegisters[offset + 1u]);
}

uint16_t VoiceFirmwareRuntime::cvWord(int bank, uint32_t offset) const noexcept
{
    if (bank < 0 || bank >= static_cast<int>(cvRegisters.size())
        || offset + 1u >= cvRegisters[static_cast<size_t>(bank)].size())
        return 0xffffu;
    const auto& registers = cvRegisters[static_cast<size_t>(bank)];
    return static_cast<uint16_t>((static_cast<uint16_t>(registers[offset]) << 8u)
                                 | registers[offset + 1u]);
}

uint8_t VoiceFirmwareRuntime::sharedByte(uint32_t offset) const noexcept
{
    return offset < sharedMemory->program.size() ? sharedMemory->program[offset] : 0xffu;
}

uint8_t VoiceFirmwareRuntime::read8(uint32_t address) noexcept
{
    if (address < localRam.size())
        return localRam[address];
    // The board has only its private 64 KB below the shared SRAM; master DRAM
    // is on the other bus.
    if (address >= sharedProgramBase && address < sharedProgramBase + sharedMemory->program.size())
        return sharedMemory->program[address - sharedProgramBase];
    if (address >= sharedWorkBase && address < sharedWorkBase + sharedMemory->work.size())
        return sharedMemory->work[address - sharedWorkBase];
    if (address >= waveRamABase && address < waveRamABase + waveRamSize)
        return waveformRam[0][address - waveRamABase];
    if (address >= waveRamBBase && address < waveRamBBase + waveRamSize)
        return waveformRam[1][address - waveRamBBase];
    if (address >= waveRamBothBase && address < waveRamBothBase + waveRamSize)
        return waveformRam[0][address - waveRamBothBase];
    if (address == boardControlAddress)
        return boardStatus;
    if (address >= asicBase && address < asicBase + asicRegisters.size())
        return asicRegisters[address - asicBase];
    if (address >= cvBankABase && address < cvBankABase + cvWindowSize)
        return cvRegisters[0][address - cvBankABase];
    if (address >= cvBankBBase && address < cvBankBBase + cvWindowSize)
        return cvRegisters[1][address - cvBankBBase];

    ++unmappedReads;
    lastUnmappedRead = address;
    return 0xffu;
}

void VoiceFirmwareRuntime::write8(uint32_t address, uint8_t value) noexcept
{
    if (address < localRam.size())
    {
        localRam[address] = value;
        return;
    }
    if (address >= sharedProgramBase && address < sharedProgramBase + sharedMemory->program.size())
    {
        sharedMemory->program[address - sharedProgramBase] = value;
        return;
    }
    if (address >= sharedWorkBase && address < sharedWorkBase + sharedMemory->work.size())
    {
        sharedMemory->work[address - sharedWorkBase] = value;
        return;
    }
    // Wave-RAM stores are not queued: a bank load is up to 32K bytes, which
    // would crowd the 64K ASIC/CV write queue, and the engine ignores them.
    if (address >= waveRamABase && address < waveRamABase + waveRamSize)
    {
        waveformRam[0][address - waveRamABase] = value;
        return;
    }
    if (address >= waveRamBBase && address < waveRamBBase + waveRamSize)
    {
        waveformRam[1][address - waveRamBBase] = value;
        return;
    }
    if (address >= waveRamBothBase && address < waveRamBothBase + waveRamSize)
    {
        waveformRam[0][address - waveRamBothBase] = value;
        waveformRam[1][address - waveRamBothBase] = value;
        return;
    }
    if (address == boardControlAddress)
    {
        controlLatch = value;
        return;
    }
    if (address >= asicBase && address < asicBase + asicRegisters.size())
    {
        asicRegisters[address - asicBase] = value;
        queueHardwareWrite(address, value);
    }
    if (address >= cvBankABase && address < cvBankABase + cvWindowSize)
    {
        cvRegisters[0][address - cvBankABase] = value;
        queueHardwareWrite(address, value);
    }
    if (address >= cvBankBBase && address < cvBankBBase + cvWindowSize)
    {
        cvRegisters[1][address - cvBankBBase] = value;
        queueHardwareWrite(address, value);
    }
    if (address >= routingLatchBase && address < routingLatchBase + routingLatchWindowSize)
        queueHardwareWrite(address, value);
}

void VoiceFirmwareRuntime::queueHardwareWrite(uint32_t address, uint8_t value) noexcept
{
    if (hardwareWrites.size() < 65536)
        hardwareWrites.push_back(
            { emulatedCycles
                  + static_cast<uint64_t>(juce::jmax(0, cpu.currentExecutionCycleOffset())),
              address, value });
}

uint32_t VoiceFirmwareRuntime::bigEndian32(const uint8_t* bytes) noexcept
{
    return (static_cast<uint32_t>(bytes[0]) << 24u)
           | (static_cast<uint32_t>(bytes[1]) << 16u)
           | (static_cast<uint32_t>(bytes[2]) << 8u)
           | static_cast<uint32_t>(bytes[3]);
}
} // namespace wave::firmware
