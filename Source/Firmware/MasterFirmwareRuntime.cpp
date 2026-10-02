#include "MasterFirmwareRuntime.h"

#include <algorithm>
#include <limits>
#include <string>

namespace wave::firmware
{
namespace
{
int sparseBankIndex(uint32_t offset) noexcept
{
    for (int index = 0; index < 8; ++index)
        if (offset == (2u << static_cast<uint32_t>(index)))
            return index;
    return -1;
}

constexpr size_t initPerformanceImageOffset = 0x0a22e;
constexpr size_t initSoundImageOffset = 0x0a42e;
constexpr size_t initSoundDestination = 0x5300;
constexpr size_t initPerformanceDestination = 0x5400;
constexpr uint32_t osFileOpen = 0x02d22c;
constexpr uint32_t osFileClose = 0x02d234;
constexpr uint32_t osFileRead = 0x02d23a;
constexpr uint16_t initSoundHandle = 0x7001;
constexpr uint16_t initPerformanceHandle = 0x7002;

class ScopedDisplayTextBuffer
{
public:
    explicit ScopedDisplayTextBuffer(SharedFirmwareMemory& memory) noexcept
        : text(memory.mainRam.data() + 0x56000u)
    {
        std::copy_n(text, saved.size(), saved.begin());
    }

    ~ScopedDisplayTextBuffer()
    {
        std::copy(saved.begin(), saved.end(), text);
    }

private:
    // OS 1.700 shares this formatting workspace with Store's name editor.
    // The panel dispatch table begins immediately after it at $56070.
    uint8_t* text;
    std::array<uint8_t, 0x70> saved{};
};
} // namespace

void MasterFirmwareRuntime::Acia6850::reset() noexcept
{
    control = 0x03;
    status = 0x02;
    receiveRead = receiveWrite = receiveCount = 0;
    transmitRead = transmitWrite = transmitCount = 0;
}

uint8_t MasterFirmwareRuntime::Acia6850::readStatus() const noexcept
{
    return static_cast<uint8_t>(status | (receiveCount == 0 ? 0x00u : 0x01u));
}

uint8_t MasterFirmwareRuntime::Acia6850::readData() noexcept
{
    if (receiveCount == 0)
        return 0x00;
    const auto result = receive[receiveRead];
    receiveRead = (receiveRead + 1u) % fifoCapacity;
    --receiveCount;
    return result;
}

void MasterFirmwareRuntime::Acia6850::writeControl(uint8_t value) noexcept
{
    control = value;
    if ((value & 0x03u) == 0x03u)
        reset();
}

void MasterFirmwareRuntime::Acia6850::writeData(uint8_t value) noexcept
{
    if (transmitCount < fifoCapacity)
    {
        transmit[transmitWrite] = value;
        transmitWrite = (transmitWrite + 1u) % fifoCapacity;
        ++transmitCount;
    }
    status |= 0x02u;
}

void MasterFirmwareRuntime::Acia6850::pushReceive(uint8_t value) noexcept
{
    if (receiveCount < fifoCapacity)
    {
        receive[receiveWrite] = value;
        receiveWrite = (receiveWrite + 1u) % fifoCapacity;
        ++receiveCount;
    }
}

bool MasterFirmwareRuntime::loadAndStart(const juce::MemoryBlock& masterImage)
{
    loaded = false;
    if (sharedMemory == &ownedSharedMemory)
        sharedMemory->clear();
    else
        sharedMemory->mainRam.fill(0);
    for (auto& byte : lcdVideoRam)
        byte.store(0, std::memory_order_relaxed);
    for (auto& word : panelSwitchWords)
        word.store(0xffffu, std::memory_order_relaxed);
    for (auto& cycle : panelReleaseCycles)
        cycle.store(0, std::memory_order_relaxed);
    for (auto& pending : panelReleasePending)
        pending.store(false, std::memory_order_relaxed);
    for (auto& requested : panelButtonRequested)
        requested.store(false, std::memory_order_relaxed);
    for (auto& word : panelLedWords)
        word.store(0x0000u, std::memory_order_relaxed);
    for (auto& value : panelAnalogValues)
        value.store(0x7fu, std::memory_order_relaxed);
    for (auto& serial : serialPorts)
        serial.reset();
    via.reset();
    ioRegisters.clear();
    selectedAnalogChannel = 0;
    emulatedCycles.store(0, std::memory_order_relaxed);
    timerTicks = 0;
    fastForwardedCycles = 0;
    audioCycleRemainder = 0.0;
    unmappedReads = 0;
    lastUnmappedRead = 0;
    lastUnmappedReadPc = 0;
    lcdWrites.store(0, std::memory_order_relaxed);
    lcdPage.store(0, std::memory_order_relaxed);
    initSoundFile = {};
    initPerformanceFile = {};
    loadedInitialisationMask = 0;
    syntheticInitialisationInstalled = false;
    // Keep the narrow file-call bridge active during the genuine cold boot;
    // OS 1.700 requests INIT.SND and INIT.PFM before the later WDV handoff.
    initialisationLoaderActive = true;
    startupContinuationActive = false;
    displayRefreshActive = false;
    displayRefreshComplete = false;
    pendingPerformanceRefresh = -1;
    voiceLoaderReached = false;
    voiceBoardHandoffComplete = false;
    activeVoiceBoardCount = 0;
    coldHardwareSetupComplete = false;

    if (masterImage.getSize() <= 0x0cu
        || masterImage.getSize() > sharedMemory->mainRam.size() - imageBase)
        return false;

    const auto* bytes = static_cast<const uint8_t*>(masterImage.getData());
    if (bytes[0] != 0x4e || bytes[1] != 0xf9 || bigEndian32(bytes + 2) != 0x0000100c
        || bytes[6] != 0x4e || bytes[7] != 0xf9 || bigEndian32(bytes + 8) != 0x00001050)
        return false;

    std::copy_n(bytes, masterImage.getSize(), sharedMemory->mainRam.begin() + imageBase);

    // INIT.PFM and INIT.SND are normally separate files on the Wave system
    // disk. OS 1.700 also contains genuine safe fallback records, used after
    // printing the missing-file warnings. The current bridge resumes after
    // the firmware's floppy-load stage, so install byte-for-byte copies of
    // those records in the exact destinations that stage would populate.
    // This preserves Waldorf's field values and checksums while avoiding
    // invented parameter layouts.
    if (masterImage.getSize() < initSoundImageOffset + syntheticInitSound.size())
        return false;
    if (!customInitialisationRecords)
    {
        std::copy_n(bytes + initPerformanceImageOffset, syntheticInitPerformance.size(),
                    syntheticInitPerformance.begin());
        std::copy_n(bytes + initSoundImageOffset, syntheticInitSound.size(),
                    syntheticInitSound.begin());
    }
    std::copy(syntheticInitSound.begin(), syntheticInitSound.end(),
              sharedMemory->program.begin() + initSoundDestination);
    std::copy(syntheticInitPerformance.begin(), syntheticInitPerformance.end(),
              sharedMemory->program.begin() + initPerformanceDestination);
    syntheticInitialisationInstalled = true;

    // The host allocations above are deterministic and bounds-checked, so the
    // destructive multi-megabyte hardware RAM test has already been satisfied.
    // OS 1.700 uses this warm-start marker to skip that test; without it the
    // direct WDV loader handoff would interrupt the test and leave its progress
    // text frozen on the LCD.
    sharedMemory->mainRam[0x400] = 0xba;
    sharedMemory->mainRam[0x401] = 0xbe;
    sharedMemory->mainRam[0x402] = 0xfa;
    sharedMemory->mainRam[0x403] = 0xce;

    // CPU-board revision/status word sampled twice by the cold-start code.
    // A zero value selects the ordinary installed-board path; $C0 is the
    // firmware's exceptional configuration branch.
    ioRegisters[0x980020] = 0x00;
    ioRegisters[0x980021] = 0x00;
    ioRegisters[0xbe0001] = 0x00;
    ioRegisters[0xbe0005] = 0x00;

    // w2sys.bin is a program loaded by the Wave's boot ROM, not a reset-vector
    // ROM. The cold-entry prologue at $00000C installs its own stack at
    // $000FFFFE, matching the local DRAM decode in the CPU-board schematic.
    cpu.start(*this, initialStackPointer, coldEntry);
    loaded = true;
    return true;
}

bool MasterFirmwareRuntime::setInitialisationRecords(
    std::span<const uint8_t> sound, std::span<const uint8_t> performance) noexcept
{
    if (sound.size() != syntheticInitSound.size()
        || performance.size() != syntheticInitPerformance.size())
        return false;

    std::copy(sound.begin(), sound.end(), syntheticInitSound.begin());
    std::copy(performance.begin(), performance.end(), syntheticInitPerformance.begin());
    customInitialisationRecords = true;
    return true;
}

bool MasterFirmwareRuntime::installEditRecords(
    std::span<const uint8_t> sound, std::span<const uint8_t> performance) noexcept
{
    if (sound.size() != syntheticInitSound.size()
        || performance.size() != syntheticInitPerformance.size()
        || initPerformanceDestination + performance.size() > sharedMemory->program.size())
        return false;

    std::copy(sound.begin(), sound.end(),
              sharedMemory->program.begin() + initSoundDestination);
    std::copy(performance.begin(), performance.end(),
              sharedMemory->program.begin() + initPerformanceDestination);
    return true;
}

bool MasterFirmwareRuntime::installSoundBank(
    std::span<const uint8_t> sounds) noexcept
{
    constexpr size_t editableBank = 0x8000u;
    constexpr size_t storedBank = 0x18000u;
    constexpr size_t bankBytes = 256u * 256u;
    if (!loaded || sounds.size() != bankBytes
        || storedBank + bankBytes > sharedMemory->program.size())
        return false;

    // The OS can resolve a Sound through either bank according to its edit
    // registry. Populate both from the loaded SET so the native browser and
    // subsequent selection see the same 256 distinct programs as the engine.
    std::copy(sounds.begin(), sounds.end(),
              sharedMemory->program.begin() + editableBank);
    std::copy(sounds.begin(), sounds.end(),
              sharedMemory->program.begin() + storedBank);
    return true;
}

bool MasterFirmwareRuntime::installPerformanceBank(
    std::span<const uint8_t> performances) noexcept
{
    constexpr uint32_t bankOffset = 0x28000u;
    constexpr size_t bankBytes = 256u * 512u;
    if (!loaded || sharedMemory == nullptr || performances.size() != bankBytes)
        return false;

    // The native bank at $128000 crosses into work SRAM at $140000.
    for (size_t index = 0; index < bankBytes; ++index)
        write8(sharedProgramBase + bankOffset + static_cast<uint32_t>(index),
               performances[index]);
    return true;
}

bool MasterFirmwareRuntime::requestPerformanceSelection(int programIndex) noexcept
{
    if (!loaded || programIndex < 0 || programIndex >= 256)
        return false;

    // OS 1.700's MIDI program-change handler combines the 7-bit program
    // number with bit 0 of this genuine system-work byte (see $008B3C-$008B52)
    // before calling the firmware's performance-selection routine.  Feeding
    // the request through that handler is important: merely replacing the
    // edit records leaves the OS's selected-performance state and its LCD
    // callbacks pointing at the previous program.
    constexpr uint32_t performanceBankOffset = 0x8820u;
    auto& bank = sharedMemory->work[performanceBankOffset];
    bank = static_cast<uint8_t>((bank & 0xfeu) | ((programIndex / 128) & 0x01));

    // A request may deliberately reselect the same program number after its
    // backing bank has changed (for example, cold-start A001 after a disk
    // Total Recall). Do not let runCycles mistake the old matching number for
    // completion before OS 1.700 has consumed the MIDI event and installed
    // the new bank's display callbacks/data.
    constexpr uint32_t selectedPerformanceAddress = 0x54b40u;
    sharedMemory->mainRam[selectedPerformanceAddress] = 0xffu;
    sharedMemory->mainRam[selectedPerformanceAddress + 1u] = 0xffu;

    pushMidiByte(0, 0xc0u);
    pushMidiByte(0, static_cast<uint8_t>(programIndex & 0x7f));
    pendingPerformanceRefresh = programIndex;
    return true;
}

bool MasterFirmwareRuntime::redrawCurrentScreenWithFirmware()
{
    constexpr uint32_t currentScreenCallbackAddress = 0x56bb0u;
    constexpr uint32_t temporaryStackBottom = 0x0f0000u;
    constexpr uint32_t temporaryStackPointer = 0x0ffff0u;
    constexpr uint32_t returnSentinel = temporaryStackBottom;
    auto& ram = sharedMemory->mainRam;
    const auto readLong = [&ram](uint32_t address) {
        return (static_cast<uint32_t>(ram[address]) << 24u)
               | (static_cast<uint32_t>(ram[address + 1u]) << 16u)
               | (static_cast<uint32_t>(ram[address + 2u]) << 8u)
               | static_cast<uint32_t>(ram[address + 3u]);
    };
    const auto entry = readLong(currentScreenCallbackAddress);
    if (entry < imageBase || entry >= 0x00050000u)
        return false;

    // Run the installed current-screen callback on an isolated 68000 context.
    // Its temporary stack occupies the Wave's reserved upper stack area and is
    // restored afterwards, leaving the continuously running OS CPU untouched.
    // All display writes and all drawing decisions still come from w2sys.bin.
    // The main CPU may be suspended between selecting a Store destination
    // and copying its edited name. Drawing fader labels uses the same text
    // buffer, so preserve it as well as the stack during this extra redraw.
    const ScopedDisplayTextBuffer preserveText(*sharedMemory);
    std::array<uint8_t, localRamSize - temporaryStackBottom> savedStack{};
    std::copy(ram.begin() + temporaryStackBottom, ram.end(), savedStack.begin());
    ram[temporaryStackPointer] = static_cast<uint8_t>(returnSentinel >> 24u);
    ram[temporaryStackPointer + 1u] = static_cast<uint8_t>(returnSentinel >> 16u);
    ram[temporaryStackPointer + 2u] = static_cast<uint8_t>(returnSentinel >> 8u);
    ram[temporaryStackPointer + 3u] = static_cast<uint8_t>(returnSentinel);

    M68000 displayCpu;
    displayRefreshComplete = false;
    displayRefreshActive = true;
    displayCpu.start(*this, temporaryStackPointer, entry);
    for (int slice = 0; slice < 200 && !displayRefreshComplete; ++slice)
        displayCpu.execute(*this, 50000);
    displayRefreshActive = false;
    std::copy(savedStack.begin(), savedStack.end(), ram.begin() + temporaryStackBottom);
    return displayRefreshComplete;
}

bool MasterFirmwareRuntime::navigatePageWithFirmware(bool forwards) noexcept
{
    if (!loaded || sharedMemory == nullptr)
        return false;

    // The genuine Page handlers at $28BA2/$28BFE call these two callbacks on
    // release. The normal controller path first waits in the OS auto-repeat
    // scheduler, which makes a host mouse click liable either to be missed or
    // to repeat several times inside one audio block. Run the callbacks that
    // the current screen installed, in their exact firmware order, once.
    const auto completed = runPanelCallbacksWithFirmware(forwards ? 0x566a8u : 0x56698u);
    sharedMemory->mainRam[0x59890u] = 0u;
    releasePanelEventLatch(21);
    releasePanelEventLatch(23);
    return completed;
}

bool MasterFirmwareRuntime::stepDiskMenuWithFirmware(bool forwards) noexcept
{
    if (!loaded || sharedMemory == nullptr)
        return false;
    const auto code = forwards ? 72u : 69u;
    const auto action = sharedMemory->mainRam[0x2850eu + code];
    const auto callbacks = 0x56618u + static_cast<uint32_t>(action) * 16u;
    const auto& ram = sharedMemory->mainRam;
    const auto callback = (static_cast<uint32_t>(ram[callbacks]) << 24u)
                          | (static_cast<uint32_t>(ram[callbacks + 1u]) << 16u)
                          | (static_cast<uint32_t>(ram[callbacks + 2u]) << 8u)
                          | ram[callbacks + 3u];
    // OS menu selectors do not arm the +/- repeat latch. Calling their
    // installed selector once avoids mistaking successful input for a retry.
    if (callback != (forwards ? 0x2dcaau : 0x2dce6u))
        return false;
    discardPendingPanelButtonEvents(static_cast<int>(code));
    releasePanelEventLatch(static_cast<int>(code));
    return runPanelCallbacksWithFirmware(callbacks);
}

bool MasterFirmwareRuntime::runPanelCallbacksWithFirmware(uint32_t callbackTable) noexcept
{
    constexpr uint32_t displayService = 0x0010b6u;
    constexpr uint32_t temporaryStackBottom = 0x0f0000u;
    constexpr uint32_t trampolineAddress = temporaryStackBottom + 0x10u;
    constexpr uint32_t temporaryStackPointer = 0x0ffff0u;
    constexpr uint32_t returnSentinel = temporaryStackBottom;
    auto& ram = sharedMemory->mainRam;
    const auto readLong = [&ram](uint32_t address) {
        return (static_cast<uint32_t>(ram[address]) << 24u)
               | (static_cast<uint32_t>(ram[address + 1u]) << 16u)
               | (static_cast<uint32_t>(ram[address + 2u]) << 8u)
               | static_cast<uint32_t>(ram[address + 3u]);
    };
    const std::array entries { readLong(callbackTable), displayService,
                               readLong(callbackTable + 4u), displayService };
    for (const auto entry : entries)
        if (entry < imageBase || entry >= 0x00050000u)
            return false;

    std::array<uint8_t, localRamSize - temporaryStackBottom> savedStack{};
    std::copy(ram.begin() + temporaryStackBottom, ram.end(), savedStack.begin());
    auto code = trampolineAddress;
    for (const auto entry : entries)
    {
        ram[code++] = 0x4eu; // JSR absolute long
        ram[code++] = 0xb9u;
        ram[code++] = static_cast<uint8_t>(entry >> 24u);
        ram[code++] = static_cast<uint8_t>(entry >> 16u);
        ram[code++] = static_cast<uint8_t>(entry >> 8u);
        ram[code++] = static_cast<uint8_t>(entry);
    }
    ram[code++] = 0x4eu; // RTS
    ram[code] = 0x75u;
    ram[temporaryStackPointer] = static_cast<uint8_t>(returnSentinel >> 24u);
    ram[temporaryStackPointer + 1u]
        = static_cast<uint8_t>(returnSentinel >> 16u);
    ram[temporaryStackPointer + 2u]
        = static_cast<uint8_t>(returnSentinel >> 8u);
    ram[temporaryStackPointer + 3u] = static_cast<uint8_t>(returnSentinel);

    M68000 pageCpu;
    displayRefreshComplete = false;
    displayRefreshActive = true;
    pageCpu.start(*this, temporaryStackPointer, trampolineAddress);
    for (int slice = 0; slice < 200 && !displayRefreshComplete; ++slice)
        pageCpu.execute(*this, 50000);
    displayRefreshActive = false;
    std::copy(savedStack.begin(), savedStack.end(), ram.begin() + temporaryStackBottom);

    return displayRefreshComplete;
}

bool MasterFirmwareRuntime::stepStoreDestinationWithFirmware(
    bool forwards) noexcept
{
    if (!loaded || sharedMemory == nullptr)
        return false;

    // The Manager controller resolves keypad and -/+ input to a program
    // number, then calls the selector installed by the active OS page at
    // $57030. That controller ASIC is unavailable; retain its exact boundary
    // by supplying only D0 and executing the callback installed by OS 1.700.
    // The callback owns the Store destination word and all LCD drawing.
    constexpr uint32_t managerSelectionCallbackAddress = 0x57030u;
    constexpr uint32_t storeDestinationAddress = 0x4b1b2u;
    constexpr uint32_t temporaryStackBottom = 0x0f0000u;
    constexpr uint32_t temporaryStackPointer = 0x0ffff0u;
    constexpr uint32_t returnSentinel = temporaryStackBottom;
    auto& ram = sharedMemory->mainRam;
    const auto readLong = [&ram](uint32_t address) {
        return (static_cast<uint32_t>(ram[address]) << 24u)
               | (static_cast<uint32_t>(ram[address + 1u]) << 16u)
               | (static_cast<uint32_t>(ram[address + 2u]) << 8u)
               | static_cast<uint32_t>(ram[address + 3u]);
    };
    const auto callback = readLong(managerSelectionCallbackAddress);
    if (callback < imageBase || callback >= 0x00050000u)
        return false;

    const auto current = static_cast<int>(
        (static_cast<uint16_t>(ram[storeDestinationAddress]) << 8u)
        | static_cast<uint16_t>(ram[storeDestinationAddress + 1u]));
    if (current < 0 || current >= 256)
        return false;
    const auto destination = juce::jlimit(
        0, 255, current + (forwards ? 1 : -1));
    if (destination == current)
        return true;

    std::array<uint8_t, localRamSize - temporaryStackBottom> savedStack {};
    std::copy(ram.begin() + temporaryStackBottom, ram.end(), savedStack.begin());
    ram[temporaryStackPointer] = static_cast<uint8_t>(returnSentinel >> 24u);
    ram[temporaryStackPointer + 1u]
        = static_cast<uint8_t>(returnSentinel >> 16u);
    ram[temporaryStackPointer + 2u]
        = static_cast<uint8_t>(returnSentinel >> 8u);
    ram[temporaryStackPointer + 3u] = static_cast<uint8_t>(returnSentinel);

    M68000 managerCpu;
    displayRefreshComplete = false;
    displayRefreshActive = true;
    managerCpu.start(*this, temporaryStackPointer, callback);
    managerCpu.setDataRegister(*this, 0, static_cast<uint32_t>(destination));
    for (int slice = 0; slice < 80 && !displayRefreshComplete; ++slice)
        managerCpu.execute(*this, 50000);
    displayRefreshActive = false;
    std::copy(savedStack.begin(), savedStack.end(), ram.begin() + temporaryStackBottom);

    const auto selected = static_cast<int>(
        (static_cast<uint16_t>(ram[storeDestinationAddress]) << 8u)
        | static_cast<uint16_t>(ram[storeDestinationAddress + 1u]));
    return displayRefreshComplete && selected == destination;
}

bool MasterFirmwareRuntime::redrawPerformanceFaderWithFirmware(int faderIndex)
{
    constexpr uint32_t currentScreenCallbackAddress = 0x56bb0u;
    constexpr uint32_t performanceScreenCallback = 0x0189c4u;
    constexpr uint32_t performanceFaderRenderer = 0x018d6cu;
    constexpr uint32_t temporaryStackBottom = 0x0f0000u;
    constexpr uint32_t temporaryStackPointer = 0x0ffff0u;
    constexpr uint32_t returnSentinel = temporaryStackBottom;
    if (faderIndex < 0 || faderIndex >= 8)
        return false;

    auto& ram = sharedMemory->mainRam;
    const auto readLong = [&ram](uint32_t address) {
        return (static_cast<uint32_t>(ram[address]) << 24u)
               | (static_cast<uint32_t>(ram[address + 1u]) << 16u)
               | (static_cast<uint32_t>(ram[address + 2u]) << 8u)
               | static_cast<uint32_t>(ram[address + 3u]);
    };
    if (readLong(currentScreenCallbackAddress) != performanceScreenCallback)
        return false;

    // $18D6C is the handler that OS 1.700 installs for each of the eight
    // Performance faders. Invoke that genuine routine with D0 equal to the
    // fader index; it chooses and rasterises the displayed value itself.
    const ScopedDisplayTextBuffer preserveText(*sharedMemory);
    std::array<uint8_t, localRamSize - temporaryStackBottom> savedStack{};
    std::copy(ram.begin() + temporaryStackBottom, ram.end(), savedStack.begin());
    ram[temporaryStackPointer] = static_cast<uint8_t>(returnSentinel >> 24u);
    ram[temporaryStackPointer + 1u] = static_cast<uint8_t>(returnSentinel >> 16u);
    ram[temporaryStackPointer + 2u] = static_cast<uint8_t>(returnSentinel >> 8u);
    ram[temporaryStackPointer + 3u] = static_cast<uint8_t>(returnSentinel);

    M68000 displayCpu;
    displayRefreshComplete = false;
    displayRefreshActive = true;
    displayCpu.start(*this, temporaryStackPointer, performanceFaderRenderer);
    displayCpu.setDataRegister(*this, 0, static_cast<uint32_t>(faderIndex));
    for (int slice = 0; slice < 40 && !displayRefreshComplete; ++slice)
        displayCpu.execute(*this, 50000);
    displayRefreshActive = false;
    std::copy(savedStack.begin(), savedStack.end(), ram.begin() + temporaryStackBottom);
    return displayRefreshComplete;
}

bool MasterFirmwareRuntime::runOs1700InitialisationFileLoad()
{
    if (!loaded || !completedColdHardwareSetup())
        return false;
    if (loadedSyntheticInitialisationFiles() && voiceLoaderReached)
        return true;

    constexpr uint32_t postFloppyInitialisation = imageBase + 0x0198u;
    constexpr uint32_t mainLoopContinuation = imageBase + 0x0050u;
    constexpr uint32_t startupStack = initialStackPointer - 4u;
    auto& mainRam = sharedMemory->mainRam;

    // The cold path is waiting inside the DP8473 setup called by the JSR at
    // $1192. Continue immediately after that one unavailable hardware call;
    // every later Wave OS initialiser, including its genuine INIT.SND/PFM
    // loader and callback registration, then executes in linked order.
    if (mainRam[postFloppyInitialisation] != 0x4e
        || mainRam[postFloppyInitialisation + 1u] != 0xb9
        || mainRam[postFloppyInitialisation + 2u] != 0x00
        || mainRam[postFloppyInitialisation + 3u] != 0x02)
        return false;

    mainRam[startupStack] = static_cast<uint8_t>(mainLoopContinuation >> 24u);
    mainRam[startupStack + 1u] = static_cast<uint8_t>(mainLoopContinuation >> 16u);
    mainRam[startupStack + 2u] = static_cast<uint8_t>(mainLoopContinuation >> 8u);
    mainRam[startupStack + 3u] = static_cast<uint8_t>(mainLoopContinuation);

    initSoundFile = {};
    initPerformanceFile = {};
    loadedInitialisationMask = 0;
    initialisationLoaderActive = true;
    startupContinuationActive = true;
    voiceLoaderReached = false;
    cpu.start(*this, startupStack, postFloppyInitialisation);
    for (int slice = 0; slice < 2000 && !voiceLoaderReached; ++slice)
        cpu.execute(*this, 50000);
    startupContinuationActive = false;
    initialisationLoaderActive = false;
    return voiceLoaderReached && loadedSyntheticInitialisationFiles();
}

bool MasterFirmwareRuntime::interceptInstruction(M68000& activeCpu,
                                                 uint32_t programCounter) noexcept
{
    constexpr uint32_t displayRefreshReturnSentinel = 0x0f0000u;
    if (displayRefreshActive && programCounter == displayRefreshReturnSentinel)
    {
        displayRefreshComplete = true;
        activeCpu.endTimeslice();
        return true;
    }

    constexpr uint32_t voiceLoaderEntry = imageBase + 0x00a738u;
    if (startupContinuationActive && programCounter == voiceLoaderEntry)
    {
        voiceLoaderReached = true;
        startupContinuationActive = false;
        activeCpu.endTimeslice();
        return true;
    }

    const auto readString = [this](uint32_t address) {
        std::string result;
        for (size_t index = 0; index < 32; ++index)
        {
            const auto character = read8(address + static_cast<uint32_t>(index));
            if (character == 0)
                break;
            result.push_back(static_cast<char>(character >= 'A' && character <= 'Z'
                                                   ? character + ('a' - 'A')
                                                   : character));
        }
        return result;
    };

    if (programCounter == osFileOpen)
    {
        const auto name = readString(activeCpu.addressRegister(0));
        if (name == "init.snd")
        {
            initSoundFile = { true, 0 };
            return activeCpu.returnFromSubroutine(initSoundHandle);
        }
        if (name == "init.pfm")
        {
            initPerformanceFile = { true, 0 };
            return activeCpu.returnFromSubroutine(initPerformanceHandle);
        }
        return false;
    }

    if (programCounter == osFileRead)
    {
        const auto handle = static_cast<uint16_t>(activeCpu.dataRegister(0));
        const auto requested = static_cast<size_t>(activeCpu.dataRegister(1));
        const auto destination = activeCpu.addressRegister(0);
        const auto serve = [this, &activeCpu, destination, requested](
                               auto& state, const auto& bytes, uint8_t loadedBit) {
            if (!state.open)
                return false;
            const auto available = bytes.size() - std::min(state.offset, bytes.size());
            const auto count = std::min(requested, available);
            for (size_t index = 0; index < count; ++index)
                write8(destination + static_cast<uint32_t>(index), bytes[state.offset + index]);
            state.offset += count;
            if (state.offset == bytes.size())
                loadedInitialisationMask |= loadedBit;
            return activeCpu.returnFromSubroutine(static_cast<uint32_t>(count));
        };

        if (handle == initSoundHandle)
            return serve(initSoundFile, syntheticInitSound, 0x01u);
        if (handle == initPerformanceHandle)
            return serve(initPerformanceFile, syntheticInitPerformance, 0x02u);
        return false;
    }

    if (programCounter == osFileClose)
    {
        const auto handle = static_cast<uint16_t>(activeCpu.dataRegister(0));
        if (handle == initSoundHandle && initSoundFile.open)
        {
            initSoundFile.open = false;
            if (loadedSyntheticInitialisationFiles() && !initPerformanceFile.open)
                initialisationLoaderActive = false;
            return activeCpu.returnFromSubroutine(0);
        }
        if (handle == initPerformanceHandle && initPerformanceFile.open)
        {
            initPerformanceFile.open = false;
            if (loadedSyntheticInitialisationFiles() && !initSoundFile.open)
                initialisationLoaderActive = false;
            return activeCpu.returnFromSubroutine(0);
        }
    }
    return false;
}

int MasterFirmwareRuntime::runCycles(int cycles)
{
    if (!loaded || cycles <= 0)
        return 0;

    auto remaining = cycles;
    auto executedTotal = 0;
    while (remaining > 0)
    {
        auto slice = remaining;
        const auto viaCycles = via.cpuCyclesUntilTimerEvent();
        if (viaCycles <= static_cast<uint32_t>(std::numeric_limits<int>::max()))
            slice = std::min(slice, static_cast<int>(viaCycles));
        cpu.setInterruptLevel(*this, via.interruptAsserted() ? 2 : 0);
        auto fastForwarded = uint32_t{};
        if (!via.interruptAsserted() && coldHardwareSetupComplete
            && voiceBoardHandoffComplete)
        {
            const auto programCounter = cpu.programCounter();
            if (programCounter == 0x0055c0u || programCounter == 0x00bdccu)
            {
                const auto dataRegister = programCounter == 0x0055c0u ? 0 : 4;
                fastForwarded = cpu.fastForwardDbraLoop(
                    *this, dataRegister, static_cast<uint32_t>(slice));
                fastForwardedCycles += fastForwarded;
            }
        }
        const auto executed = fastForwarded > 0u
                                  ? static_cast<int>(fastForwarded)
                                  : cpu.execute(*this, slice);
        if (executed <= 0)
            break;
        executedTotal += executed;
        remaining -= std::min(remaining, executed);
        const auto currentCycle
            = emulatedCycles.fetch_add(static_cast<uint64_t>(executed),
                                       std::memory_order_relaxed)
              + static_cast<uint64_t>(executed);
        releaseExpiredPanelButtons(currentCycle);
        timerTicks += static_cast<uint64_t>(via.advanceCpuCycles(executed));
        cpu.setInterruptLevel(*this, via.interruptAsserted() ? 2 : 0);

        if (pendingPerformanceRefresh >= 0)
        {
            constexpr uint32_t selectedPerformanceAddress = 0x54b40u;
            const auto selected = (static_cast<int>(sharedMemory->mainRam[
                                       selectedPerformanceAddress])
                                   << 8)
                                  | static_cast<int>(sharedMemory->mainRam[
                                      selectedPerformanceAddress + 1u]);
            if (selected == pendingPerformanceRefresh)
            {
                redrawCurrentScreenWithFirmware();
                pendingPerformanceRefresh = -1;
            }
        }

    }
    return executedTotal;
}

int MasterFirmwareRuntime::runForAudioSamples(int samples, double sampleRate)
{
    if (samples <= 0 || sampleRate <= 0.0)
        return 0;
    const auto exact = audioCycleRemainder
                       + static_cast<double>(samples) * static_cast<double>(masterClockHz)
                             / sampleRate;
    const auto cycles = static_cast<int>(exact);
    audioCycleRemainder = exact - static_cast<double>(cycles);
    return runCycles(cycles);
}

bool MasterFirmwareRuntime::runOs1700VoiceBoardLoaderHandoff(int voiceBoardCount)
{
    voiceBoardHandoffComplete = false;
    activeVoiceBoardCount = 0;
    if (!loaded || !completedColdHardwareSetup() || !voiceLoaderReached
        || voiceBoardCount < 1 || voiceBoardCount > 3)
        return false;

    constexpr uint32_t oscillatorInitialisationEntry = imageBase + 0x0091cc;
    constexpr uint32_t boardCountOffset = 0x50e4;
    constexpr uint32_t acknowledgementOffset = 0x508a;
    constexpr uint32_t serviceOffset = 0x508e;

    // Enter the complete authenticated oscillator-initialisation subroutine,
    // not its internal wait-loop tail. This lets the firmware create and
    // restore both MOVEM register frames itself.
    auto& mainRam = sharedMemory->mainRam;
    if (mainRam[oscillatorInitialisationEntry] != 0x48
        || mainRam[oscillatorInitialisationEntry + 1u] != 0xe7
        || mainRam[oscillatorInitialisationEntry + 2u] != 0xff
        || mainRam[oscillatorInitialisationEntry + 3u] != 0xfe)
        return false;

    sharedMemory->program[boardCountOffset] = 0x00;
    sharedMemory->program[boardCountOffset + 1u]
        = static_cast<uint8_t>(voiceBoardCount);
    for (auto board = 0; board < voiceBoardCount; ++board)
    {
        sharedMemory->program[acknowledgementOffset + static_cast<uint32_t>(board)] = 0x00;
        sharedMemory->program[serviceOffset + static_cast<uint32_t>(board)] = 0x00;
    }

    // WDV.SYS has already been installed in shared SRAM. The missing floppy
    // load would return at $11DC, whose next JSR enters this routine and
    // returns at $11E2. Recreate only those two genuine caller return words;
    // all register frames are then pushed by the Wave OS itself.
    constexpr uint32_t callerReturnBytes = 2u * 4u;
    constexpr uint32_t handoffStack = initialStackPointer - callerReturnBytes;
    constexpr uint32_t postOscillatorInitialisation = imageBase + 0x01e2u;
    constexpr uint32_t mainLoopContinuation = imageBase + 0x0050u;
    mainRam[handoffStack] = static_cast<uint8_t>(postOscillatorInitialisation >> 24u);
    mainRam[handoffStack + 1u]
        = static_cast<uint8_t>(postOscillatorInitialisation >> 16u);
    mainRam[handoffStack + 2u]
        = static_cast<uint8_t>(postOscillatorInitialisation >> 8u);
    mainRam[handoffStack + 3u] = static_cast<uint8_t>(postOscillatorInitialisation);
    mainRam[handoffStack + 4u]
        = static_cast<uint8_t>(mainLoopContinuation >> 24u);
    mainRam[handoffStack + 5u]
        = static_cast<uint8_t>(mainLoopContinuation >> 16u);
    mainRam[handoffStack + 6u]
        = static_cast<uint8_t>(mainLoopContinuation >> 8u);
    mainRam[handoffStack + 7u] = static_cast<uint8_t>(mainLoopContinuation);
    cpu.start(*this, handoffStack, oscillatorInitialisationEntry);

    for (int slice = 0; slice < 100 && sharedMemory->program[acknowledgementOffset] != 0x01u;
         ++slice)
        cpu.execute(*this, 50000);

    voiceBoardHandoffComplete = true;
    for (auto board = 0; board < voiceBoardCount; ++board)
        voiceBoardHandoffComplete = voiceBoardHandoffComplete
                                    && sharedMemory->program[acknowledgementOffset
                                                             + static_cast<uint32_t>(board)] == 0x01;
    if (voiceBoardHandoffComplete)
        activeVoiceBoardCount = voiceBoardCount;
    return voiceBoardHandoffComplete;
}

bool MasterFirmwareRuntime::completeOs1700VoiceBoardServiceHandoff()
{
    constexpr uint32_t serviceOffset = 0x508e;

    if (!loaded || !voiceBoardHandoffComplete || activeVoiceBoardCount < 1)
        return false;
    for (auto board = 0; board < activeVoiceBoardCount; ++board)
        if (sharedMemory->program[serviceOffset + static_cast<uint32_t>(board)] != 0xffu)
            return false;

    // The main CPU remains in its genuine service-wait loop. Once the WDV CPU
    // publishes this byte, normal cycle execution takes the firmware's own OK
    // branch and unwinds its real stack frames.
    return true;
}

uint8_t MasterFirmwareRuntime::ioByte(uint32_t address) const noexcept
{
    const auto found = ioRegisters.find(address & 0x00ffffffu);
    return found != ioRegisters.end() ? found->second : 0xffu;
}

uint8_t MasterFirmwareRuntime::localByte(uint32_t address) const noexcept
{
    return address < sharedMemory->mainRam.size() ? sharedMemory->mainRam[address] : 0xffu;
}

uint8_t MasterFirmwareRuntime::sharedProgramByte(uint32_t offset) const noexcept
{
    if (offset < sharedMemory->program.size())
        return sharedMemory->program[offset];
    offset -= static_cast<uint32_t>(sharedMemory->program.size());
    return offset < sharedMemory->work.size() ? sharedMemory->work[offset] : 0xffu;
}

bool MasterFirmwareRuntime::writePerformanceInstrumentByte(
    int instrument, uint32_t offset, uint8_t value) noexcept
{
    constexpr uint32_t instrumentTable = 64u;
    constexpr uint32_t instrumentRecordSize = 32u;
    if (!loaded || sharedMemory == nullptr || instrument < 0 || instrument >= 8
        || offset >= instrumentRecordSize)
        return false;
    const auto performance = currentPerformanceRecordOffset();
    if (!performance.has_value())
        return false;
    const auto address = *performance + instrumentTable
                         + static_cast<uint32_t>(instrument) * instrumentRecordSize
                         + offset;
    write8(sharedProgramBase + address, value);
    return true;
}

std::optional<int> MasterFirmwareRuntime::currentPerformanceId() const noexcept
{
    if (!loaded || sharedMemory == nullptr)
        return std::nullopt;

    constexpr uint32_t address = 0x54b40u;
    if (address + 1u >= sharedMemory->mainRam.size())
        return std::nullopt;
    return (static_cast<int>(sharedMemory->mainRam[address]) << 8)
           | static_cast<int>(sharedMemory->mainRam[address + 1u]);
}

std::optional<int> MasterFirmwareRuntime::currentPerformanceInstrument() const noexcept
{
    const auto performanceOffset = currentPerformanceRecordOffset();
    if (!performanceOffset.has_value() || sharedMemory == nullptr)
        return std::nullopt;

    return static_cast<int>(
        sharedProgramByte(*performanceOffset + 24u) & 0x0fu);
}

std::optional<uint32_t>
MasterFirmwareRuntime::currentPerformanceRecordOffset() const noexcept
{
    const auto performanceId = currentPerformanceId();
    if (!performanceId.has_value() || sharedMemory == nullptr)
        return std::nullopt;

    const auto& program = sharedMemory->program;
    const auto readProgramWord = [&program](uint32_t offset) {
        if (offset + 1u >= program.size())
            return uint16_t { 0xffffu };
        return static_cast<uint16_t>((static_cast<uint16_t>(program[offset]) << 8u)
                                     | program[offset + 1u]);
    };
    constexpr uint32_t specialPerformanceIdOffset = 0x5184u;
    constexpr uint32_t specialPerformanceOffset = 0x5600u;
    constexpr uint32_t fallbackPerformanceOffset = 0x5400u;
    constexpr uint32_t performanceBankOffset = 0x28000u;
    auto performanceOffset
        = static_cast<uint16_t>(*performanceId)
                  == readProgramWord(specialPerformanceIdOffset)
              ? specialPerformanceOffset
              : performanceBankOffset
                    + (static_cast<uint32_t>(*performanceId) & 0xffu) * 0x200u;
    if (sharedProgramByte(performanceOffset + 48u) != 0x55u)
        performanceOffset = fallbackPerformanceOffset;
    return performanceOffset;
}

std::optional<int> MasterFirmwareRuntime::currentInstrumentEditTarget() const noexcept
{
    if (!loaded || sharedMemory == nullptr)
        return std::nullopt;
    // OS 1.700's Instrument Edit fader handler ($14122) reads this page
    // target. Performance byte 24 can change when an Instrument is disabled
    // or its Performance is promoted to the edit buffer.
    // Group Edit reuses that byte as a parameter selector ($14142), so it
    // retains the Performance's selected Instrument for single-layer feedback.
    if (sharedMemory->mainRam[0x56ed8u] != 0u)
        return currentPerformanceInstrument();
    const auto target = static_cast<int>(sharedMemory->mainRam[0x56eeau]);
    return target < 8 ? std::optional<int>(target) : std::nullopt;
}

std::optional<int> MasterFirmwareRuntime::currentInstrumentEditPage() const noexcept
{
    if (!loaded || sharedMemory == nullptr)
        return std::nullopt;
    const auto& ram = sharedMemory->mainRam;
    const auto callback = (static_cast<uint32_t>(ram[0x56bb0u]) << 24u)
                          | (static_cast<uint32_t>(ram[0x56bb1u]) << 16u)
                          | (static_cast<uint32_t>(ram[0x56bb2u]) << 8u)
                          | ram[0x56bb3u];
    // OS 1.700's Instrument renderer $13F78 uses this persistent page index.
    // Re-entering Instrument Edit preserves it; the mode key does not reset
    // it. Group/External Edit have different fader destinations.
    if (callback != 0x13f78u || ram[0x56ee8u] != 0x49u || ram[0x56ed8u] != 0u)
        return std::nullopt;
    return static_cast<int>(ram[0x56ee9u] & 3u);
}

std::optional<uint32_t> MasterFirmwareRuntime::currentSoundRecordOffset() const noexcept
{
    const auto instrument = currentPerformanceInstrument();
    return instrument.has_value()
               ? performanceInstrumentSoundRecordOffset(*instrument)
               : std::nullopt;
}

std::optional<uint32_t>
MasterFirmwareRuntime::performanceInstrumentSoundRecordOffset(
    int requestedInstrument) const noexcept
{
    if (!loaded || sharedMemory == nullptr)
        return std::nullopt;

    const auto& program = sharedMemory->program;
    const auto readProgramWord = [&program](uint32_t offset) {
        if (offset + 1u >= program.size())
            return uint16_t { 0xffffu };
        return static_cast<uint16_t>((static_cast<uint16_t>(program[offset]) << 8u)
                                     | program[offset + 1u]);
    };

    // These are the same record-resolution tables used by OS 1.700 at
    // $A8A2/$A8D4/$AE38/$AB7C. The firmware edits the selected Instrument's
    // live Sound record, which is not necessarily the $5300 startup record.
    constexpr uint32_t soundCacheIdsOffset = 0x5186u;
    constexpr uint32_t soundRegistryOffset = 0x5200u;
    constexpr uint32_t soundCacheOffset = 0x5800u;
    constexpr uint32_t editableSoundBankOffset = 0x8000u;
    constexpr uint32_t storedSoundBankOffset = 0x18000u;
    constexpr uint32_t fallbackSoundOffset = 0x5300u;
    constexpr uint32_t soundRecordSize = 0x100u;

    const auto performance = currentPerformanceRecordOffset();
    if (!performance.has_value())
        return std::nullopt;
    const auto performanceOffset = *performance;

    if (requestedInstrument < 0 || requestedInstrument >= 8)
        return std::nullopt;
    const auto instrument = static_cast<uint32_t>(requestedInstrument);
    const auto instrumentOffset
        = instrument < 8u
              ? performanceOffset + 64u + instrument * 32u
              : performanceOffset + 64u + 256u + (instrument & 7u) * 24u;
    const auto soundId = static_cast<uint32_t>(
        (static_cast<uint32_t>(sharedProgramByte(instrumentOffset + 1u)) << 7u
         | static_cast<uint32_t>(sharedProgramByte(instrumentOffset)))
        & 0xffu);

    for (int slot = 7; slot >= 0; --slot)
    {
        const auto entry = readProgramWord(
            soundCacheIdsOffset + static_cast<uint32_t>(slot * 2));
        if ((entry & 0x7fffu) == soundId && (entry & 0x8000u) == 0u)
        {
            const auto offset = soundCacheOffset
                                + static_cast<uint32_t>(slot) * soundRecordSize;
            if (offset + soundRecordSize <= program.size())
                return offset;
            return std::nullopt;
        }
    }

    const auto registryOffset = soundRegistryOffset + soundId;
    if (registryOffset >= program.size())
        return std::nullopt;
    const auto offset = (program[registryOffset] == 1u ? editableSoundBankOffset
                                                       : storedSoundBankOffset)
                        + soundId * soundRecordSize;
    if (offset + soundRecordSize > program.size())
        return std::nullopt;

    // The accelerated INIT loader supplies $5300 before OS 1.700 has promoted
    // that Sound into its normal bank/cache. An empty bank slot is therefore
    // not an authoritative Sound record yet.
    if (std::all_of(program.begin() + offset,
                    program.begin() + offset + soundRecordSize,
                    [](uint8_t value) { return value == 0u; }))
        return fallbackSoundOffset;
    return offset;
}

uint8_t MasterFirmwareRuntime::currentSoundRecordByte(uint32_t offset) const noexcept
{
    const auto record = currentSoundRecordOffset();
    return record.has_value() && offset < 0x100u
               ? sharedProgramByte(*record + offset) : 0xffu;
}

bool MasterFirmwareRuntime::writeCurrentSoundRecordByte(uint32_t offset,
                                                         uint8_t value) noexcept
{
    const auto record = currentSoundRecordOffset();
    if (!record.has_value() || offset >= 0x100u)
        return false;
    sharedMemory->program[*record + offset] = value;
    return true;
}

bool MasterFirmwareRuntime::installPerformanceInstrumentSoundRecord(
    int instrument, std::span<const uint8_t, 256> sound) noexcept
{
    const auto record = performanceInstrumentSoundRecordOffset(instrument);
    if (!record.has_value() || *record + sound.size() > sharedMemory->program.size())
        return false;
    std::copy(sound.begin(), sound.end(),
              sharedMemory->program.begin() + *record);
    return true;
}

bool MasterFirmwareRuntime::installCurrentPerformanceRecord(
    std::span<const uint8_t, 512> performance) noexcept
{
    const auto record = currentPerformanceRecordOffset();
    if (!record.has_value() || sharedMemory == nullptr)
        return false;
    for (size_t index = 0; index < performance.size(); ++index)
        write8(sharedProgramBase + *record + static_cast<uint32_t>(index),
               performance[index]);
    return true;
}

void MasterFirmwareRuntime::setFilterCalibrationCode(int voice, uint16_t code) noexcept
{
    // Absolute $15A680 is $1A680 into the shared-work SRAM mapped at $140000.
    constexpr uint32_t tableOffset = 0x1a680u;
    if (sharedMemory == nullptr || voice < 0 || voice >= 48)
        return;
    const auto offset = tableOffset + static_cast<uint32_t>(voice * 2);
    const auto value = static_cast<uint16_t>(code & 0x0fffu);
    sharedMemory->work[offset] = static_cast<uint8_t>(value >> 8u);
    sharedMemory->work[offset + 1u] = static_cast<uint8_t>(value);
}

uint16_t MasterFirmwareRuntime::filterCalibrationCode(int voice) const noexcept
{
    constexpr uint32_t tableOffset = 0x1a680u;
    if (sharedMemory == nullptr || voice < 0 || voice >= 48)
        return 0x0800u;
    const auto offset = tableOffset + static_cast<uint32_t>(voice * 2);
    const auto high = static_cast<uint16_t>(sharedMemory->work[offset]);
    const auto low = static_cast<uint16_t>(sharedMemory->work[offset + 1u]);
    return static_cast<uint16_t>(
        static_cast<uint16_t>((high << 8u) | low) & uint16_t { 0x0fffu });
}

uint8_t MasterFirmwareRuntime::sharedWorkByte(uint32_t offset) const noexcept
{
    return offset < sharedMemory->work.size() ? sharedMemory->work[offset] : 0xffu;
}

uint8_t MasterFirmwareRuntime::lcdVideoByte(uint32_t offset) const noexcept
{
    return offset < lcdVideoRam.size()
               ? lcdVideoRam[offset].load(std::memory_order_relaxed)
               : 0xffu;
}

MasterFirmwareRuntime::LcdVideoSnapshot MasterFirmwareRuntime::lcdVideoSnapshot() const noexcept
{
    LcdVideoSnapshot snapshot{};
    for (size_t index = 0; index < lcdVideoRam.size(); ++index)
        snapshot[index] = lcdVideoRam[index].load(std::memory_order_relaxed);
    return snapshot;
}

uint8_t MasterFirmwareRuntime::lcdDisplayPage() const noexcept
{
    return lcdPage.load(std::memory_order_relaxed);
}

uint8_t MasterFirmwareRuntime::read8(uint32_t address) noexcept
{
    if (address < sharedMemory->mainRam.size())
        return sharedMemory->mainRam[address];
    if (address >= sharedProgramBase && address < sharedProgramBase + sharedMemory->program.size())
        return sharedMemory->program[address - sharedProgramBase];
    if (address >= sharedWorkBase && address < sharedWorkBase + sharedMemory->work.size())
        return sharedMemory->work[address - sharedWorkBase];
    if (address >= lcdVideoBase && address < lcdVideoBase + lcdVideoRam.size())
        return lcdVideoRam[address - lcdVideoBase].load(std::memory_order_relaxed);

    // Active-low keyboard/encoder matrix inputs. No host controls currently
    // occupy these two banks, so every contact remains electrically open.
    if ((address & 0x00fc0000u) == 0x00900000u)
        return 0xffu;

    if (address >= 0x980000u && address < 0x980200u)
    {
        const auto bank = sparseBankIndex(address - 0x980000u & ~1u);
        if (bank >= 0)
        {
            const auto word = panelSwitchWords[static_cast<size_t>(bank)].load(
                std::memory_order_relaxed);
            return (address & 1u) == 0u ? static_cast<uint8_t>(word >> 8u)
                                        : static_cast<uint8_t>(word);
        }
    }
    if (address >= 0x9c0000u && address < 0x9c0200u)
    {
        selectedAnalogChannel = static_cast<uint8_t>(((address - 0x9c0000u) >> 1u) & 0x7fu);
        return 0x00;
    }
    if (address == 0x9e0001u)
        return panelAnalogValues[selectedAnalogChannel].load(std::memory_order_relaxed);

    // CPU board sheet 7 shifts 68000 A1-A3 onto the DP8473 A0-A2 inputs.
    // OS 1.700 additionally uses GAL-generated DMA acknowledge and terminal
    // count aliases at $A8001B/$A8002B, and $A80031 to acknowledge its FDC IRQ.
    if (address >= 0xa80000u && address < 0xa80100u)
    {
        const auto value = floppy.read(address);
        if (floppy.interruptPending())
            sharedMemory->mainRam[0x54fbfu] = 1;
        return value;
    }
    if (address >= 0xbe0001u && address <= 0xbe001fu && (address & 1u) != 0u)
    {
        const auto interruptWasAsserted = via.interruptAsserted();
        const auto reg = static_cast<uint8_t>((address - 0xbe0001u) >> 1u);
        // Port A bit 7 is the CPU-board floppy DRQ input.
        const auto value = via.read(
            reg, static_cast<uint8_t>(0x7fu | (floppy.drqAsserted() ? 0x80u : 0x00u)));
        if (interruptWasAsserted && !via.interruptAsserted())
            cpu.endTimeslice();
        return value;
    }

    for (size_t port = 0; port < serialPorts.size(); ++port)
    {
        const auto base = 0xb80000u + static_cast<uint32_t>(port) * 0x020000u;
        if (address == base + 1u)
            return serialPorts[port].readStatus();
        if (address == base + 3u)
            return serialPorts[port].readData();
    }
    if (isDecodedIo(address))
        return ioByte(address);

    ++unmappedReads;
    lastUnmappedRead = address;
    lastUnmappedReadPc = cpu.programCounter();
    return 0xffu;
}

void MasterFirmwareRuntime::write8(uint32_t address, uint8_t value) noexcept
{
    if (address < sharedMemory->mainRam.size())
    {
        sharedMemory->mainRam[address] = value;
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
    if (address >= lcdVideoBase && address < lcdVideoBase + lcdVideoRam.size())
    {
        lcdVideoRam[address - lcdVideoBase].store(value, std::memory_order_relaxed);
        lcdWrites.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (address >= 0xa80000u && address < 0xa80100u)
    {
        floppy.write(address, value);
        if (floppy.interruptPending())
            sharedMemory->mainRam[0x54fbfu] = 1;
        return;
    }
    if (address >= 0x9a0000u && address < 0x9a0200u)
    {
        const auto bank = sparseBankIndex(address - 0x9a0000u & ~1u);
        if (bank >= 0)
        {
            auto& destination = panelLedWords[static_cast<size_t>(bank)];
            const auto word = destination.load(std::memory_order_relaxed);
            const auto updated = (address & 1u) == 0u
                                     ? static_cast<uint16_t>(
                                           (word & 0x00ffu)
                                           | static_cast<uint16_t>(
                                               static_cast<uint16_t>(value) << 8u))
                                     : static_cast<uint16_t>((word & 0xff00u) | value);
            destination.store(updated, std::memory_order_relaxed);
        }
        return;
    }
    if (address >= 0xbe0001u && address <= 0xbe001fu && (address & 1u) != 0u)
    {
        const auto interruptWasAsserted = via.interruptAsserted();
        const auto reg = static_cast<uint8_t>((address - 0xbe0001u) >> 1u);
        via.write(reg, value);
        ioRegisters[address] = value;
        if (address == 0xbe0001u)
            lcdPage.store(static_cast<uint8_t>((value >> 5u) & 0x03u),
                          std::memory_order_relaxed);
        coldHardwareSetupComplete = coldHardwareSetupComplete
                                    || (ioByte(0xb80001) == 0x03
                                        && ioByte(0xba0001) == 0x03
                                        && ioByte(0xbc0001) == 0x03
                                        && ioByte(0xbe001d) == 0x7f);
        if (interruptWasAsserted && !via.interruptAsserted())
            cpu.endTimeslice();
        return;
    }
    for (size_t port = 0; port < serialPorts.size(); ++port)
    {
        const auto base = 0xb80000u + static_cast<uint32_t>(port) * 0x020000u;
        if (address == base + 1u)
        {
            serialPorts[port].writeControl(value);
            ioRegisters[address] = value;
            return;
        }
        if (address == base + 3u)
        {
            serialPorts[port].writeData(value);
            ioRegisters[address] = value;
            return;
        }
    }
    if (isDecodedIo(address))
    {
        ioRegisters[address] = value;
        if (address == 0xbe0001u)
            lcdPage.store(static_cast<uint8_t>((value >> 5u) & 0x03u),
                          std::memory_order_relaxed);
        coldHardwareSetupComplete = coldHardwareSetupComplete
                                    || (ioByte(0xb80001) == 0x03
                                        && ioByte(0xba0001) == 0x03
                                        && ioByte(0xbc0001) == 0x03
                                        && ioByte(0xbe001d) == 0x7f);
    }
}

void MasterFirmwareRuntime::setPanelButton(int buttonId, bool pressed) noexcept
{
    if (buttonId < 0 || buttonId >= 128)
        return;
    panelButtonRequested[static_cast<size_t>(buttonId)].store(
        pressed, std::memory_order_release);
    const auto bank = static_cast<size_t>(buttonId / 16);
    const auto bit = static_cast<uint16_t>(1u << static_cast<unsigned int>(buttonId % 16));
    if (pressed)
    {
        panelReleasePending[static_cast<size_t>(buttonId)].store(
            false, std::memory_order_release);
        panelReleaseCycles[static_cast<size_t>(buttonId)].store(
            emulatedCycles.load(std::memory_order_relaxed) + minimumPanelPressCycles,
            std::memory_order_release);
        panelSwitchWords[bank].fetch_and(static_cast<uint16_t>(~bit),
                                         std::memory_order_relaxed);
    }
    else
    {
        const auto releaseCycle
            = panelReleaseCycles[static_cast<size_t>(buttonId)].load(
                std::memory_order_acquire);
        if (emulatedCycles.load(std::memory_order_relaxed) < releaseCycle)
        {
            // A mouse click may begin and end between two audio callbacks.
            // Keep the electrical contact closed long enough for OS 1.700's
            // genuine panel scanner and debounce logic to observe it.
            panelReleasePending[static_cast<size_t>(buttonId)].store(
                true, std::memory_order_release);
        }
        else
        {
            panelSwitchWords[bank].fetch_or(bit, std::memory_order_relaxed);
            panelReleasePending[static_cast<size_t>(buttonId)].store(
                false, std::memory_order_release);
        }
    }
}

void MasterFirmwareRuntime::releaseExpiredPanelButtons(
    uint64_t currentCycle) noexcept
{
    for (size_t button = 0; button < panelReleasePending.size(); ++button)
    {
        if (!panelReleasePending[button].load(std::memory_order_acquire)
            || currentCycle < panelReleaseCycles[button].load(std::memory_order_acquire)
            || panelButtonRequested[button].load(std::memory_order_acquire)
            || !panelReleasePending[button].exchange(false,
                                                     std::memory_order_acq_rel))
            continue;

        const auto bank = button / 16u;
        const auto bit = static_cast<uint16_t>(
            1u << static_cast<unsigned int>(button % 16u));
        panelSwitchWords[bank].fetch_or(bit, std::memory_order_relaxed);
    }
}

void MasterFirmwareRuntime::setPanelAnalog(int controlId, float normalised) noexcept
{
    if (controlId < 0 || controlId >= static_cast<int>(panelAnalogValues.size()))
        return;
    panelAnalogValues[static_cast<size_t>(controlId)].store(
        static_cast<uint8_t>(
            juce::roundToInt(juce::jlimit(0.0f, 1.0f, normalised) * 255.0f)),
        std::memory_order_relaxed);
}

uint8_t MasterFirmwareRuntime::panelAnalogByte(int controlId) const noexcept
{
    if (controlId < 0 || controlId >= static_cast<int>(panelAnalogValues.size()))
        return 0;
    return panelAnalogValues[static_cast<size_t>(controlId)].load(
        std::memory_order_relaxed);
}

void MasterFirmwareRuntime::rebaseRelativePanelPot(int diagnosticCode,
                                                    int adcChannel) noexcept
{
    // OS 1.700 keeps the last physical value used by Relative Knob Mode in a
    // separate table from both the ADC filter and the Sound record. A host
    // session restores those three pieces independently, just as a powered
    // hardware panel can disagree with its loaded Sound. Seed only this
    // physical reference; do not dispatch an edit or touch the Sound/LCD.
    constexpr uint32_t serialToControlTable = 0x28704u;
    constexpr uint32_t dispatchTable = 0x56070u;
    constexpr uint32_t relativeReferenceTable = 0x56c98u;
    if (!loaded || sharedMemory == nullptr || diagnosticCode < 0
        || diagnosticCode > 64 || adcChannel < 0
        || adcChannel >= static_cast<int>(panelAnalogValues.size()))
        return;

    const auto control = static_cast<uint32_t>(
        sharedMemory->mainRam[serialToControlTable
                              + static_cast<uint32_t>(diagnosticCode)]);
    if (control >= 64u)
        return;

    auto value = static_cast<uint16_t>(
        panelAnalogValues[static_cast<size_t>(adcChannel)].load(
            std::memory_order_relaxed) >> 1u);
    if (control > 7u)
        value ^= 0x7fu;
    value &= 0x7fu;

    const auto entry = dispatchTable + control * 16u;
    const auto readLong = [this](uint32_t address) {
        const auto& ram = sharedMemory->mainRam;
        return (static_cast<uint32_t>(ram[address]) << 24u)
               | (static_cast<uint32_t>(ram[address + 1u]) << 16u)
               | (static_cast<uint32_t>(ram[address + 2u]) << 8u)
               | static_cast<uint32_t>(ram[address + 3u]);
    };
    const auto range = readLong(entry + 4u);
    if (range + 2u < sharedMemory->mainRam.size())
    {
        const auto minimum = static_cast<uint16_t>(sharedMemory->mainRam[range]);
        const auto maximum = static_cast<uint16_t>(sharedMemory->mainRam[range + 1u]);
        const auto quantum = static_cast<uint16_t>(sharedMemory->mainRam[range + 2u]);
        if (minimum <= maximum)
        {
            value = static_cast<uint16_t>(
                minimum
                + ((value * static_cast<uint16_t>(maximum - minimum + 1u))
                   >> 7u));
            value &= 0x7fu;
            if (quantum > 1u)
                value = static_cast<uint16_t>(
                    value & static_cast<uint16_t>(~(quantum - 1u)));
        }
    }

    sharedMemory->mainRam[relativeReferenceTable + control]
        = static_cast<uint8_t>(value);
}

bool MasterFirmwareRuntime::panelButtonPressed(int buttonId) const noexcept
{
    if (buttonId < 0 || buttonId >= 128)
        return false;
    const auto word = panelSwitchWords[static_cast<size_t>(buttonId / 16)].load(
        std::memory_order_relaxed);
    const auto bit = static_cast<uint16_t>(
        1u << static_cast<unsigned int>(buttonId % 16));
    return (word & bit) == 0u;
}

bool MasterFirmwareRuntime::panelButtonRequestedDown(int buttonId) const noexcept
{
    if (buttonId < 0 || buttonId >= 128)
        return false;
    return panelButtonRequested[static_cast<size_t>(buttonId)].load(
        std::memory_order_acquire);
}

void MasterFirmwareRuntime::setPerformanceFaderValue(int faderIndex,
                                                       uint8_t value) noexcept
{
    // The panel scanner keeps these eight last-read values separately from
    // the assigned sound parameter. Update that hardware-facing table, then
    // run the exact OS 1.700 fader display handler installed by the current
    // Performance screen. The UI never writes or annotates LCD pixels.
    constexpr uint32_t faderValueTable = 0x57088u;
    if (faderIndex < 0 || faderIndex >= 8)
        return;
    sharedMemory->mainRam[faderValueTable + static_cast<uint32_t>(faderIndex)]
        = static_cast<uint8_t>(value & 0x7fu);
    redrawPerformanceFaderWithFirmware(faderIndex);
}

void MasterFirmwareRuntime::pushPanelEvent(uint8_t status, uint8_t data1,
                                           uint8_t data2) noexcept
{
    if (sharedMemory == nullptr)
        return;

    // The panel scanner writes fixed three-byte records into this genuine OS
    // ring. $468E consumes them as status, control number and value/delta.
    constexpr uint32_t readPointerAddress = 0x551f0u;
    constexpr uint32_t writePointerAddress = 0x551f4u;
    constexpr uint32_t ringBegin = 0x55840u;
    constexpr uint32_t ringEnd = 0x55b40u;
    auto& ram = sharedMemory->mainRam;
    const auto readPointer = [&ram](uint32_t address) {
        return (static_cast<uint32_t>(ram[address]) << 24u)
               | (static_cast<uint32_t>(ram[address + 1u]) << 16u)
               | (static_cast<uint32_t>(ram[address + 2u]) << 8u)
               | static_cast<uint32_t>(ram[address + 3u]);
    };
    const auto writePointer = [&ram](uint32_t address, uint32_t pointer) {
        ram[address] = static_cast<uint8_t>(pointer >> 24u);
        ram[address + 1u] = static_cast<uint8_t>(pointer >> 16u);
        ram[address + 2u] = static_cast<uint8_t>(pointer >> 8u);
        ram[address + 3u] = static_cast<uint8_t>(pointer);
    };
    auto producer = readPointer(writePointerAddress);
    const auto consumer = readPointer(readPointerAddress);
    if (producer < ringBegin || producer >= ringEnd
        || consumer < ringBegin || consumer >= ringEnd)
        return;

    const auto advance = [](uint32_t pointer) {
        ++pointer;
        return pointer >= ringEnd ? ringBegin : pointer;
    };
    auto end = producer;
    for (int byte = 0; byte < 3; ++byte)
    {
        end = advance(end);
        if (end == consumer)
            return;
    }
    for (const auto value : { status, data1, data2 })
    {
        ram[producer] = value;
        producer = advance(producer);
    }
    writePointer(writePointerAddress, producer);
}

int MasterFirmwareRuntime::discardPendingPanelButtonEvents(int buttonId) noexcept
{
    if (sharedMemory == nullptr || buttonId < 0 || buttonId >= 128)
        return 0;

    constexpr uint32_t readPointerAddress = 0x551f0u;
    constexpr uint32_t writePointerAddress = 0x551f4u;
    constexpr uint32_t ringBegin = 0x55840u;
    constexpr uint32_t ringEnd = 0x55b40u;
    auto& ram = sharedMemory->mainRam;
    const auto readPointer = [&ram](uint32_t address) {
        return (static_cast<uint32_t>(ram[address]) << 24u)
               | (static_cast<uint32_t>(ram[address + 1u]) << 16u)
               | (static_cast<uint32_t>(ram[address + 2u]) << 8u)
               | static_cast<uint32_t>(ram[address + 3u]);
    };
    const auto writePointer = [&ram](uint32_t address, uint32_t pointer) {
        ram[address] = static_cast<uint8_t>(pointer >> 24u);
        ram[address + 1u] = static_cast<uint8_t>(pointer >> 16u);
        ram[address + 2u] = static_cast<uint8_t>(pointer >> 8u);
        ram[address + 3u] = static_cast<uint8_t>(pointer);
    };
    const auto advance = [](uint32_t pointer) {
        ++pointer;
        return pointer >= ringEnd ? ringBegin : pointer;
    };

    const auto consumer = readPointer(readPointerAddress);
    const auto producer = readPointer(writePointerAddress);
    if (consumer < ringBegin || consumer >= ringEnd
        || producer < ringBegin || producer >= ringEnd)
        return 0;

    std::array<uint8_t, ringEnd - ringBegin> retained{};
    auto retainedBytes = size_t{};
    auto removed = 0;
    auto cursor = consumer;
    while (cursor != producer)
    {
        std::array<uint8_t, 3> event{};
        for (auto& byte : event)
        {
            if (cursor == producer)
                return 0; // The controller ring must contain complete records.
            byte = ram[cursor];
            cursor = advance(cursor);
        }
        if ((event[0] & 0x80u) != 0u
            && event[1] == static_cast<uint8_t>(buttonId))
        {
            ++removed;
            continue;
        }
        for (const auto byte : event)
            retained[retainedBytes++] = byte;
    }

    cursor = consumer;
    for (size_t index = 0; index < retainedBytes; ++index)
    {
        ram[cursor] = retained[index];
        cursor = advance(cursor);
    }
    writePointer(writePointerAddress, cursor);
    return removed;
}

bool MasterFirmwareRuntime::panelEventPending(uint8_t status, uint8_t data1,
                                               uint8_t data2) const noexcept
{
    if (sharedMemory == nullptr)
        return false;
    constexpr uint32_t readPointerAddress = 0x551f0u;
    constexpr uint32_t writePointerAddress = 0x551f4u;
    constexpr uint32_t ringBegin = 0x55840u;
    constexpr uint32_t ringEnd = 0x55b40u;
    const auto& ram = sharedMemory->mainRam;
    const auto readPointer = [&ram](uint32_t address) {
        return (static_cast<uint32_t>(ram[address]) << 24u)
               | (static_cast<uint32_t>(ram[address + 1u]) << 16u)
               | (static_cast<uint32_t>(ram[address + 2u]) << 8u)
               | static_cast<uint32_t>(ram[address + 3u]);
    };
    auto consumer = readPointer(readPointerAddress);
    const auto producer = readPointer(writePointerAddress);
    if (consumer < ringBegin || consumer >= ringEnd
        || producer < ringBegin || producer >= ringEnd)
        return false;
    const auto advance = [](uint32_t pointer) {
        ++pointer;
        return pointer >= ringEnd ? ringBegin : pointer;
    };
    for (auto scanned = uint32_t{};
         consumer != producer && scanned < ringEnd - ringBegin; ++scanned)
    {
        const auto eventStatus = ram[consumer];
        consumer = advance(consumer);
        if ((eventStatus & 0x80u) == 0u || consumer == producer)
            continue;
        const auto eventData1 = ram[consumer];
        consumer = advance(consumer);
        if (consumer == producer)
            return false;
        const auto eventData2 = ram[consumer];
        consumer = advance(consumer);
        if (eventStatus == status && eventData1 == data1 && eventData2 == data2)
            return true;
    }
    return false;
}

bool MasterFirmwareRuntime::releasePanelEventLatch(int buttonId) noexcept
{
    if (sharedMemory == nullptr || buttonId < 0 || buttonId >= 87)
        return false;

    // OK/CANCEL share a chord latch and execute on release ($28416-$28474).
    // A missed scanner release can leave the panic state at $FF indefinitely.
    // Once both physical contacts are up, clear only that stale held state
    // before a new explicit response transaction.
    if ((buttonId == 70 || buttonId == 71)
        && !panelButtonRequestedDown(70) && !panelButtonRequestedDown(71)
        && !panelButtonPressed(70) && !panelButtonPressed(71))
    {
        sharedMemory->mainRam[0x58feau] = 0;
        return true;
    }

    // OS 1.700's event dispatcher maps panel serials through $2850E and keeps
    // the active auto-repeat action at $58FE4. Mirror its release branch at
    // $28362 after a host-generated click, so the next discrete click cannot
    // be mistaken for a continuation of the previous hold.
    constexpr uint32_t dispatchTable = 0x2850eu;
    constexpr uint32_t activeAction = 0x58fe4u;
    constexpr uint32_t repeatDelay = 0x58fe6u;
    constexpr uint32_t repeatCount = 0x58fe8u;
    auto& ram = sharedMemory->mainRam;
    const auto action = static_cast<uint16_t>(ram[dispatchTable
                                                  + static_cast<uint32_t>(buttonId)]);
    const auto active = static_cast<uint16_t>(
        (static_cast<uint16_t>(ram[activeAction]) << 8u)
        | static_cast<uint16_t>(ram[activeAction + 1u]));
    if (active != action)
        return false;

    const auto writeWord = [&ram](uint32_t address, uint16_t value) {
        ram[address] = static_cast<uint8_t>(value >> 8u);
        ram[address + 1u] = static_cast<uint8_t>(value);
    };
    writeWord(activeAction, 0u);
    writeWord(repeatDelay, 100u);
    writeWord(repeatCount, 6u);
    return true;
}

bool MasterFirmwareRuntime::releasePanelActionBit(int buttonId) noexcept
{
    if (sharedMemory == nullptr || buttonId < 0 || buttonId >= 87)
        return false;

    // For the eight display switches OS 1.700's dispatcher maps the physical
    // code to action 0-7, then its release branch at $28340 clears that action
    // in $56ED9. Apply that exact side effect synchronously so a page change
    // cannot overtake a queued release event and leave a soft key held.
    constexpr uint32_t dispatchTable = 0x2850eu;
    constexpr uint32_t activeDisplayActions = 0x56ed9u;
    const auto action = static_cast<unsigned int>(
        sharedMemory->mainRam[dispatchTable + static_cast<uint32_t>(buttonId)]);
    if (action >= 8u)
        return false;
    sharedMemory->mainRam[activeDisplayActions]
        = static_cast<uint8_t>(sharedMemory->mainRam[activeDisplayActions]
                               & static_cast<uint8_t>(~(1u << action)));
    return true;
}

bool MasterFirmwareRuntime::panelActionBitActive(int buttonId) const noexcept
{
    if (sharedMemory == nullptr || buttonId < 0 || buttonId >= 87)
        return false;

    constexpr uint32_t dispatchTable = 0x2850eu;
    constexpr uint32_t activeDisplayActions = 0x56ed9u;
    const auto action = static_cast<unsigned int>(
        sharedMemory->mainRam[dispatchTable + static_cast<uint32_t>(buttonId)]);
    return action < 8u
           && (sharedMemory->mainRam[activeDisplayActions]
               & static_cast<uint8_t>(1u << action)) != 0u;
}

void MasterFirmwareRuntime::pushMidiByte(int port, uint8_t value) noexcept
{
    if (port < 0 || port >= static_cast<int>(serialPorts.size()))
        return;

    // The physical MIDI IN ACIA normally enters OS 1.700 through its level-4
    // interrupt handler at $0038CC.  Until the board's interrupt-priority
    // encoder is modelled, asserting an invented autovector is unsafe.  For
    // port 0, reproduce the handler's documented side effect instead: append
    // the received byte to its genuine $520D0-$528CF input ring.  The normal
    // main-loop parser at $003AAA then consumes the byte and calls all genuine
    // MIDI/program-change handlers.
    if (port == 0)
    {
        constexpr uint32_t writePointerAddress = 0x4dda4u;
        constexpr uint32_t readPointerAddress = 0x4dda8u;
        constexpr uint32_t ringBegin = 0x520d0u;
        constexpr uint32_t ringEnd = 0x528d0u;
        const auto readPointer = [this](uint32_t address) {
            const auto& ram = sharedMemory->mainRam;
            return (static_cast<uint32_t>(ram[address]) << 24u)
                   | (static_cast<uint32_t>(ram[address + 1u]) << 16u)
                   | (static_cast<uint32_t>(ram[address + 2u]) << 8u)
                   | static_cast<uint32_t>(ram[address + 3u]);
        };
        const auto writePointer = [this](uint32_t address, uint32_t pointer) {
            auto& ram = sharedMemory->mainRam;
            ram[address] = static_cast<uint8_t>(pointer >> 24u);
            ram[address + 1u] = static_cast<uint8_t>(pointer >> 16u);
            ram[address + 2u] = static_cast<uint8_t>(pointer >> 8u);
            ram[address + 3u] = static_cast<uint8_t>(pointer);
        };

        auto destination = readPointer(writePointerAddress);
        const auto consumer = readPointer(readPointerAddress);
        if (destination >= ringBegin && destination < ringEnd
            && consumer >= ringBegin && consumer < ringEnd)
        {
            auto next = destination + 1u;
            if (next >= ringEnd)
                next = ringBegin;
            if (next != consumer)
            {
                sharedMemory->mainRam[destination] = value;
                writePointer(writePointerAddress, next);
            }
            return;
        }
    }

    serialPorts[static_cast<size_t>(port)].pushReceive(value);
}

void MasterFirmwareRuntime::pushKeyboardByte(uint8_t value) noexcept
{
    if (sharedMemory == nullptr)
        return;

    // The Wave keyboard/transport controller enters OS 1.700 through the
    // third ACIA ISR at $004E3E. Until interrupt arbitration is modelled,
    // reproduce that genuine handler's side effects: mark the controller as
    // connected and append its ASCII command to the firmware's own ring.
    constexpr uint32_t readPointerAddress = 0x55ce4u;
    constexpr uint32_t writePointerAddress = 0x55ce0u;
    constexpr uint32_t connectedAddress = 0x55cf4u;
    constexpr uint32_t ringBegin = 0x55d00u;
    constexpr uint32_t ringEnd = 0x55e00u;
    auto& ram = sharedMemory->mainRam;
    const auto readPointer = [&ram](uint32_t address) {
        return (static_cast<uint32_t>(ram[address]) << 24u)
               | (static_cast<uint32_t>(ram[address + 1u]) << 16u)
               | (static_cast<uint32_t>(ram[address + 2u]) << 8u)
               | static_cast<uint32_t>(ram[address + 3u]);
    };
    const auto writePointer = [&ram](uint32_t address, uint32_t pointer) {
        ram[address] = static_cast<uint8_t>(pointer >> 24u);
        ram[address + 1u] = static_cast<uint8_t>(pointer >> 16u);
        ram[address + 2u] = static_cast<uint8_t>(pointer >> 8u);
        ram[address + 3u] = static_cast<uint8_t>(pointer);
    };

    auto producer = readPointer(writePointerAddress);
    auto consumer = readPointer(readPointerAddress);
    if (producer < ringBegin || producer >= ringEnd
        || consumer < ringBegin || consumer >= ringEnd)
    {
        // The accelerated cold-start path can complete before OS routine
        // $4D6C has initialised this otherwise-idle peripheral. Its real reset
        // value is an empty ring, so establish exactly that state on first use.
        producer = ringBegin;
        consumer = ringBegin;
        writePointer(writePointerAddress, producer);
        writePointer(readPointerAddress, consumer);
    }

    auto next = producer + 1u;
    if (next >= ringEnd)
        next = ringBegin;
    if (next == consumer)
        return;

    ram[connectedAddress] = 0xffu;
    ram[producer] = value;
    writePointer(writePointerAddress, next);
}

bool MasterFirmwareRuntime::panelLed(int ledId) const noexcept
{
    if (ledId < 0 || ledId >= 128)
        return false;
    const auto word = panelLedWords[static_cast<size_t>(ledId / 16)].load(
        std::memory_order_relaxed);
    return (word & static_cast<uint16_t>(1u << static_cast<unsigned int>(ledId % 16))) != 0u;
}

uint32_t MasterFirmwareRuntime::bigEndian32(const uint8_t* bytes) noexcept
{
    return (static_cast<uint32_t>(bytes[0]) << 24u)
           | (static_cast<uint32_t>(bytes[1]) << 16u)
           | (static_cast<uint32_t>(bytes[2]) << 8u)
           | static_cast<uint32_t>(bytes[3]);
}

bool MasterFirmwareRuntime::isDecodedIo(uint32_t address) noexcept
{
    // Only windows reached by the authenticated cold-start path are exposed
    // at this stage. Individual device semantics are added only when supported
    // by schematics and observed firmware traffic.
    const auto page = address & 0x00ff0000u;
    return page == 0x00900000u || page == 0x00920000u
           || page == 0x00980000u || page == 0x009a0000u || page == 0x009c0000u
           || page == 0x009e0000u || page == 0x00a80000u || page == 0x00aa0000u
           || page == 0x00b80000u || page == 0x00ba0000u
           || page == 0x00bc0000u || page == 0x00be0000u;
}

juce::Result MasterFirmwareRuntime::mountDiskImage(const juce::File& imageFile)
{
    return floppy.mount(imageFile);
}

juce::Result MasterFirmwareRuntime::flushDiskImage()
{
    return floppy.flush();
}

juce::Result MasterFirmwareRuntime::ejectDiskImage()
{
    return floppy.eject();
}
} // namespace wave::firmware
