// Runs the real WDV.SYS in VoiceFirmwareRuntime from a command script and prints
// the logged ASIC/CV/routing-latch write stream ("W <cycle> <addr> <byte>").
// The oracle gate gate_e_wdv_note_trace.py (Waldorf Wave firmware repo) runs the
// same script on the Musashi oracle and diffs the two streams.
//
// Script commands: poke <hexaddr> <hexbyte> (shared RAM 0x100000..0x1fffff),
// until_pc <hexpc> (single-step), scan (wait for one full voice scan),
// run <cycles>, mark <name>. An optional third argument selects the board
// (0-2, default 0; the board's alive flag is 0x10508e + board).
#include "Firmware/VoiceFirmwareRuntime.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

using namespace wave::firmware;

namespace
{
constexpr uint32_t loopAliveFlag = 0x10508e; // st.b after every 0x79c call, + board id
constexpr int scanSliceCycles = 200;
constexpr long stepLimit = 50'000'000;

uint8_t* sharedAt(SharedFirmwareMemory& memory, uint32_t address)
{
    if (address >= 0x180000 && address < 0x180000 + memory.extension.size())
        return &memory.extension[address - 0x180000];
    if (address >= 0x140000 && address < 0x140000 + memory.work.size())
        return &memory.work[address - 0x140000];
    if (address >= 0x100000 && address < 0x100000 + memory.program.size())
        return &memory.program[address - 0x100000];
    return nullptr;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc != 3 && argc != 4)
    {
        std::fprintf(stderr, "usage: WaveVoiceTrace wdv.sys script [board]\n");
        return 2;
    }
    const auto board = argc == 4 ? std::atoi(argv[3]) : 0;
    juce::MemoryBlock image;
    if (!juce::File(argv[1]).loadFileAsData(image))
    {
        std::fprintf(stderr, "cannot read %s\n", argv[1]);
        return 2;
    }

    SharedFirmwareMemory memory;
    memory.clear();
    VoiceFirmwareRuntime runtime;
    runtime.attachSharedMemory(memory);
    if (!runtime.setBoardIndex(board))
    {
        std::fprintf(stderr, "board %d is not a valid strap slot\n", board);
        return 2;
    }
    if (!runtime.loadAndReset(image))
    {
        std::fprintf(stderr, "WDV image rejected\n");
        return 2;
    }

    const auto flush = [&] {
        for (const auto& write : runtime.pendingHardwareWrites())
            std::printf("W %llu %06x %02x\n", static_cast<unsigned long long>(write.cycle),
                        write.address, write.value);
        runtime.clearHardwareWrites();
    };

    std::ifstream script(argv[2]);
    std::string line;
    while (std::getline(script, line))
    {
        std::istringstream in(line);
        std::string command;
        in >> command;
        if (command == "poke")
        {
            uint32_t address = 0, value = 0;
            in >> std::hex >> address >> value;
            auto* byte = sharedAt(memory, address);
            if (byte == nullptr)
            {
                std::fprintf(stderr, "poke outside shared RAM: %06x\n", address);
                return 2;
            }
            *byte = static_cast<uint8_t>(value);
        }
        else if (command == "until_pc")
        {
            uint32_t pc = 0;
            in >> std::hex >> pc;
            long steps = 0;
            while (runtime.programCounter() != pc)
            {
                runtime.runCycles(1);
                if (++steps > stepLimit)
                {
                    std::fprintf(stderr, "PC %06x not reached\n", pc);
                    return 3;
                }
            }
        }
        else if (command == "scan")
        {
            const auto alive = loopAliveFlag + static_cast<uint32_t>(board);
            *sharedAt(memory, alive) = 0;
            long slices = 0;
            while (runtime.sharedByte(alive - 0x100000) == 0)
            {
                runtime.runCycles(scanSliceCycles);
                if (++slices > stepLimit)
                {
                    std::fprintf(stderr, "scan did not finish\n");
                    return 3;
                }
            }
        }
        else if (command == "run")
        {
            int cycles = 0;
            in >> cycles;
            runtime.runCycles(cycles);
        }
        else if (command == "mark")
        {
            std::string name;
            in >> name;
            flush();
            std::printf("M %s\n", name.c_str());
        }
    }
    flush();
    return 0;
}
