/*
 * SPDX-License-Identifier: MIT
 */
#include "provisioning_frame.h"

#include <algorithm>
#include <string_view>

namespace stackchan::companion {

ProvisioningFeedResult ProvisioningFrameParser::feed(char value)
{
    if (value == '\n') {
        return finishFrame();
    }
    if (value == '\0'
        || (static_cast<unsigned char>(value) < 0x20 && value != '\r')
        || value == 0x7f) {
        overflowed_ = true;
        return ProvisioningFeedResult::waiting;
    }
    if (length_ >= buffer_.size()) {
        overflowed_ = true;
        return ProvisioningFeedResult::waiting;
    }
    buffer_[length_++] = value;
    return ProvisioningFeedResult::waiting;
}

ProvisioningFeedResult ProvisioningFrameParser::finishFrame()
{
    if (overflowed_) {
        reset();
        return ProvisioningFeedResult::rejected;
    }

    std::string_view frame(buffer_.data(), length_);
    if (!frame.empty() && frame.back() == '\r') {
        frame.remove_suffix(1);
    }
    constexpr std::string_view prefix(kProvisioningFramePrefix,
        sizeof(kProvisioningFramePrefix) - 1);
    if (frame.size() < prefix.size() || frame.substr(0, prefix.size()) != prefix) {
        reset();
        return ProvisioningFeedResult::rejected;
    }

    const std::string_view payload = frame.substr(prefix.size());
    const std::size_t separator = payload.find(' ');
    if (separator == std::string_view::npos || separator == 0
        || separator + 1 >= payload.size()
        || payload.find(' ', separator + 1) != std::string_view::npos) {
        reset();
        return ProvisioningFeedResult::rejected;
    }

    CompanionCredentials parsed{
        std::string(payload.substr(0, separator)),
        std::string(payload.substr(separator + 1)),
    };
    if (!parsed.configured()) {
        reset();
        return ProvisioningFeedResult::rejected;
    }

    std::fill(buffer_.begin(), buffer_.end(), '\0');
    length_ = 0;
    overflowed_ = false;
    credentials_ = std::move(parsed);
    return ProvisioningFeedResult::accepted;
}

CompanionCredentials ProvisioningFrameParser::takeCredentials()
{
    CompanionCredentials result = std::move(credentials_);
    credentials_ = {};
    return result;
}

void ProvisioningFrameParser::reset()
{
    std::fill(buffer_.begin(), buffer_.end(), '\0');
    length_ = 0;
    overflowed_ = false;
    std::fill(credentials_.deviceId.begin(), credentials_.deviceId.end(), '\0');
    std::fill(credentials_.deviceToken.begin(), credentials_.deviceToken.end(), '\0');
    credentials_ = {};
}

}  // namespace stackchan::companion
