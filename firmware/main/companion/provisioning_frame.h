/*
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "companion_config.h"

#include <array>
#include <cstddef>

namespace stackchan::companion {

inline constexpr char kProvisioningFramePrefix[] = "SC-PROVISION-1 ";
inline constexpr std::size_t kProvisioningFrameMaximumBytes = 256;

enum class ProvisioningFeedResult {
    waiting,
    accepted,
    rejected,
};

class ProvisioningFrameParser {
public:
    ProvisioningFeedResult feed(char value);
    CompanionCredentials takeCredentials();
    void reset();

private:
    ProvisioningFeedResult finishFrame();

    std::array<char, kProvisioningFrameMaximumBytes> buffer_{};
    std::size_t length_ = 0;
    bool overflowed_ = false;
    CompanionCredentials credentials_;
};

}  // namespace stackchan::companion
