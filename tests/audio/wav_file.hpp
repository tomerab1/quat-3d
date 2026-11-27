#pragma once

// Test fixture: writes a tiny 16-bit PCM mono WAV to the GoogleTest temp
// directory, so the audio tests need no asset files.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace quat::test {

// A constant-amplitude (DC) signal: after mixing, the output level is the
// product of every gain on the path — easy to measure exactly.
[[nodiscard]] inline std::string write_test_wav(const std::string& name, float seconds,
                                                float amplitude = 0.5F,
                                                std::uint32_t sample_rate = 48000) {
    const auto frames = static_cast<std::uint32_t>(seconds * static_cast<float>(sample_rate));
    const auto sample = static_cast<std::int16_t>(amplitude * 32767.0F);
    std::vector<std::uint8_t> bytes;
    const auto u32 = [&bytes](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    };
    const auto u16 = [&bytes](std::uint16_t v) {
        bytes.push_back(static_cast<std::uint8_t>(v & 0xFFU));
        bytes.push_back(static_cast<std::uint8_t>(v >> 8U));
    };
    const auto tag = [&bytes](const char* t) { bytes.insert(bytes.end(), t, t + 4); };
    const std::uint32_t data_size = frames * 2U;
    tag("RIFF");
    u32(36U + data_size);
    tag("WAVE");
    tag("fmt ");
    u32(16);              // fmt chunk size
    u16(1);               // PCM
    u16(1);               // mono
    u32(sample_rate);
    u32(sample_rate * 2); // byte rate
    u16(2);               // block align
    u16(16);              // bits per sample
    tag("data");
    u32(data_size);
    for (std::uint32_t i = 0; i < frames; ++i) u16(static_cast<std::uint16_t>(sample));

    const std::filesystem::path path = std::filesystem::path(::testing::TempDir()) / name;
    std::FILE* f = std::fopen(path.string().c_str(), "wb");
    if (f == nullptr) return {};
    const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    std::fclose(f);
    return ok ? path.string() : std::string();
}

} // namespace quat::test
