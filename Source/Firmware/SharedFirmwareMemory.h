#pragma once

#include <array>
#include <cstdint>

namespace wave::firmware
{
struct SharedFirmwareMemory
{
    static constexpr uint32_t mainRamSize = 0x100000;
    static constexpr uint32_t programSize = 0x40000;
    static constexpr uint32_t workSize = 0x40000;
    // The shared SRAM is 512 KB (0x100000-0x17FFFF = program + work); nothing
    // is decoded above it, so accesses there count as unmapped.
    static constexpr uint32_t extensionSize = 0;

    void clear() noexcept
    {
        mainRam.fill(0);
        program.fill(0);
        work.fill(0);
        extension.fill(0);
    }

    std::array<uint8_t, mainRamSize> mainRam{};
    std::array<uint8_t, programSize> program{};
    std::array<uint8_t, workSize> work{};
    std::array<uint8_t, extensionSize> extension{};
};
} // namespace wave::firmware
