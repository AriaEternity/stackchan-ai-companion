/*
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace stackchan::companion {

constexpr std::size_t kFullScreenTextThresholdBytes = 64;
constexpr std::uint32_t kFullScreenTextDurationMs = 15'000;

inline bool shouldUseFullScreenText(std::string_view text)
{
    return text.find('\n') != std::string_view::npos
        || text.size() > kFullScreenTextThresholdBytes;
}

inline bool screenTextDeadlineReached(std::uint32_t nowMs, std::uint32_t deadlineMs)
{
    return deadlineMs != 0
        && static_cast<std::int32_t>(nowMs - deadlineMs) >= 0;
}

}  // namespace stackchan::companion
