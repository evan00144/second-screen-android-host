#pragma once

#include <cstddef>
#include <cstdint>

bool ConvertBgraToNv12Avx2(
    const std::uint8_t* source,
    std::size_t rowPitch,
    std::uint32_t width,
    std::uint32_t height,
    std::uint8_t* destination) noexcept;
