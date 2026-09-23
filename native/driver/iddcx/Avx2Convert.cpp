#include "Avx2Convert.h"

#include <immintrin.h>

#include <cstring>

namespace
{
std::uint8_t ClampByte(int value) noexcept
{
    return static_cast<std::uint8_t>(value < 0 ? 0 : value > 255 ? 255 : value);
}

__m256i LoadBgraChannel(
    const std::uint8_t* source,
    __m256i channelMask,
    int shift) noexcept
{
    const __m256i pixels = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(source));
    return _mm256_and_si256(_mm256_srli_epi32(pixels, shift), channelMask);
}

__m256i AverageChromaChannel(__m256i row0, __m256i row1, __m256i zero) noexcept
{
    const __m256i rowSum = _mm256_add_epi32(row0, row1);
    return _mm256_srli_epi32(_mm256_hadd_epi32(rowSum, zero), 2);
}

__m256i ConvertChroma(
    __m256i red,
    __m256i green,
    __m256i blue,
    int redCoefficient,
    int greenCoefficient,
    int blueCoefficient,
    __m256i bias,
    __m256i zero,
    __m256i maxValue) noexcept
{
    __m256i values = _mm256_add_epi32(
        _mm256_add_epi32(
            _mm256_mullo_epi32(red, _mm256_set1_epi32(redCoefficient)),
            _mm256_mullo_epi32(green, _mm256_set1_epi32(greenCoefficient))),
        _mm256_mullo_epi32(blue, _mm256_set1_epi32(blueCoefficient)));
    values = _mm256_add_epi32(_mm256_srai_epi32(_mm256_add_epi32(values, bias), 8), bias);
    return _mm256_min_epi32(_mm256_max_epi32(values, zero), maxValue);
}

__m128i CompactFour(__m256i values) noexcept
{
    const __m128i low = _mm256_castsi256_si128(values);
    const __m128i high = _mm256_extracti128_si256(values, 1);
    return _mm_unpacklo_epi64(low, high);
}

void ConvertYRow(
    const std::uint8_t* sourceRow,
    std::uint32_t width,
    std::uint8_t* destinationRow) noexcept
{
    const __m256i channelMask = _mm256_set1_epi32(0xff);
    const __m256i zero = _mm256_setzero_si256();
    const __m256i yBias = _mm256_set1_epi32(128);
    std::uint32_t x = 0;
    for (; x + 8 <= width; x += 8)
    {
        const std::uint8_t* pixels = sourceRow + static_cast<std::size_t>(x) * 4;
        const __m256i blue = LoadBgraChannel(pixels, channelMask, 0);
        const __m256i green = LoadBgraChannel(pixels, channelMask, 8);
        const __m256i red = LoadBgraChannel(pixels, channelMask, 16);
        __m256i values = _mm256_add_epi32(
            _mm256_add_epi32(
                _mm256_mullo_epi32(blue, _mm256_set1_epi32(25)),
                _mm256_mullo_epi32(green, _mm256_set1_epi32(129))),
            _mm256_mullo_epi32(red, _mm256_set1_epi32(66)));
        values = _mm256_add_epi32(_mm256_srli_epi32(_mm256_add_epi32(values, yBias), 8), _mm256_set1_epi32(16));
        const __m128i y16 = _mm_packs_epi32(
            _mm256_castsi256_si128(values),
            _mm256_extracti128_si256(values, 1));
        const __m128i y8 = _mm_packus_epi16(y16, _mm_setzero_si128());
        _mm_storel_epi64(reinterpret_cast<__m128i*>(destinationRow + x), y8);
    }

    for (; x < width; ++x)
    {
        const auto* pixel = sourceRow + static_cast<std::size_t>(x) * 4;
        destinationRow[x] = ClampByte(
            ((66 * pixel[2] + 129 * pixel[1] + 25 * pixel[0] + 128) >> 8) + 16);
    }
}

void ConvertUvRow(
    const std::uint8_t* row0,
    const std::uint8_t* row1,
    std::uint32_t width,
    std::uint8_t* destinationRow) noexcept
{
    const __m256i channelMask = _mm256_set1_epi32(0xff);
    const __m256i zero = _mm256_setzero_si256();
    const __m256i bias = _mm256_set1_epi32(128);
    const __m256i maxValue = _mm256_set1_epi32(255);
    std::uint32_t x = 0;
    for (; x + 8 <= width; x += 8)
    {
        const std::size_t offset = static_cast<std::size_t>(x) * 4;
        const auto* pixels0 = row0 + offset;
        const auto* pixels1 = row1 + offset;
        const __m256i blue0 = LoadBgraChannel(pixels0, channelMask, 0);
        const __m256i green0 = LoadBgraChannel(pixels0, channelMask, 8);
        const __m256i red0 = LoadBgraChannel(pixels0, channelMask, 16);
        const __m256i blue1 = LoadBgraChannel(pixels1, channelMask, 0);
        const __m256i green1 = LoadBgraChannel(pixels1, channelMask, 8);
        const __m256i red1 = LoadBgraChannel(pixels1, channelMask, 16);
        const __m256i blue = AverageChromaChannel(blue0, blue1, zero);
        const __m256i green = AverageChromaChannel(green0, green1, zero);
        const __m256i red = AverageChromaChannel(red0, red1, zero);
        const __m256i u = ConvertChroma(red, green, blue, -38, -74, 112, bias, zero, maxValue);
        const __m256i v = ConvertChroma(red, green, blue, 112, -94, -18, bias, zero, maxValue);
        const __m128i u16 = _mm_packs_epi32(CompactFour(u), _mm_setzero_si128());
        const __m128i v16 = _mm_packs_epi32(CompactFour(v), _mm_setzero_si128());
        const __m128i u8 = _mm_packus_epi16(u16, _mm_setzero_si128());
        const __m128i v8 = _mm_packus_epi16(v16, _mm_setzero_si128());
        const __m128i uv = _mm_unpacklo_epi8(u8, v8);
        _mm_storel_epi64(reinterpret_cast<__m128i*>(destinationRow + x), uv);
    }

    for (; x < width; x += 2)
    {
        int blue = 0;
        int green = 0;
        int red = 0;
        for (std::uint32_t dy = 0; dy < 2; ++dy)
        {
            const auto* row = dy == 0 ? row0 : row1;
            for (std::uint32_t dx = 0; dx < 2; ++dx)
            {
                const auto* pixel = row + static_cast<std::size_t>(x + dx) * 4;
                blue += pixel[0];
                green += pixel[1];
                red += pixel[2];
            }
        }
        blue /= 4;
        green /= 4;
        red /= 4;
        destinationRow[x] = ClampByte(((-38 * red - 74 * green + 112 * blue + 128) >> 8) + 128);
        destinationRow[x + 1] = ClampByte(((112 * red - 94 * green - 18 * blue + 128) >> 8) + 128);
    }
}
}

bool ConvertBgraToNv12Avx2(
    const std::uint8_t* source,
    std::size_t rowPitch,
    std::uint32_t width,
    std::uint32_t height,
    std::uint8_t* destination) noexcept
{
    if (source == nullptr || destination == nullptr ||
        width == 0 || height == 0 || (width & 1u) != 0 || (height & 1u) != 0 ||
        rowPitch < static_cast<std::size_t>(width) * 4)
    {
        return false;
    }

    const std::size_t yPlaneBytes = static_cast<std::size_t>(width) * height;
    auto* yPlane = destination;
    auto* uvPlane = destination + yPlaneBytes;
    for (std::uint32_t y = 0; y < height; ++y)
    {
        ConvertYRow(
            source + static_cast<std::size_t>(y) * rowPitch,
            width,
            yPlane + static_cast<std::size_t>(y) * width);
    }
    for (std::uint32_t y = 0; y < height; y += 2)
    {
        ConvertUvRow(
            source + static_cast<std::size_t>(y) * rowPitch,
            source + static_cast<std::size_t>(y + 1) * rowPitch,
            width,
            uvPlane + static_cast<std::size_t>(y / 2) * width);
    }
    return true;
}
