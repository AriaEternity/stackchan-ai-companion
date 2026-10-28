/*
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace stackchan::companion {

// 在构建前把示例地址改为自己的 HTTPS 网关域名；此地址不包含凭据。
inline constexpr std::string_view kCompanionServerUrl = "wss://stackchan.example.com/ws/device";
inline constexpr std::size_t kDeviceIdMaximumBytes = 64;
inline constexpr std::size_t kDeviceTokenMaximumBytes = 128;

enum class CompanionStartupState {
    unconfigured,
    offlineSafe,
};

struct CompanionCredentials {
    std::string deviceId;
    std::string deviceToken;

    bool configured() const;
};

bool isValidDeviceId(std::string_view value);
bool isValidDeviceToken(std::string_view value);
std::string maskedTokenStatus(std::string_view value);
CompanionStartupState startupState(const CompanionCredentials& credentials);

class CompanionConfigStore {
public:
    CompanionCredentials load() const;
    bool save(const CompanionCredentials& credentials) const;
    void clear() const;
};

}  // namespace stackchan::companion
