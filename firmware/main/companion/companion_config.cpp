/*
 * SPDX-License-Identifier: MIT
 */
#include "companion_config.h"

#include <algorithm>
#include <cctype>
#ifndef COMPANION_HOST_TEST
#include <settings.h>
#endif

namespace stackchan::companion {
namespace {

#ifndef COMPANION_HOST_TEST
constexpr const char* kSettingsNamespace = "companion";
constexpr const char* kDeviceIdKey = "device_id";
constexpr const char* kDeviceTokenKey = "device_token";
#endif

bool allCharactersMatch(std::string_view value, bool (*predicate)(unsigned char))
{
    return std::all_of(value.begin(), value.end(), [predicate](char current) {
        return predicate(static_cast<unsigned char>(current));
    });
}

bool validDeviceIdCharacter(unsigned char value)
{
    return std::isalnum(value) != 0 || value == '-' || value == '_' || value == '.';
}

bool validTokenCharacter(unsigned char value)
{
    return std::isalnum(value) != 0 || value == '-' || value == '.' || value == '_' || value == '~'
        || value == '+' || value == '/' || value == '=';
}

}  // namespace

bool CompanionCredentials::configured() const
{
    return isValidDeviceId(deviceId) && isValidDeviceToken(deviceToken);
}

bool isValidDeviceId(std::string_view value)
{
    return !value.empty() && value.size() <= kDeviceIdMaximumBytes
        && allCharactersMatch(value, validDeviceIdCharacter);
}

bool isValidDeviceToken(std::string_view value)
{
    return !value.empty() && value.size() <= kDeviceTokenMaximumBytes
        && allCharactersMatch(value, validTokenCharacter);
}

std::string maskedTokenStatus(std::string_view value)
{
    if (value.empty()) {
        return "NOT SET";
    }
    if (value.size() < 4) {
        return "SET";
    }
    return "SET (..." + std::string(value.substr(value.size() - 4)) + ")";
}

CompanionStartupState startupState(const CompanionCredentials& credentials)
{
    return credentials.configured() ? CompanionStartupState::offlineSafe
                                    : CompanionStartupState::unconfigured;
}

#ifndef COMPANION_HOST_TEST
CompanionCredentials CompanionConfigStore::load() const
{
    Settings settings(kSettingsNamespace, false);
    return {
        settings.GetString(kDeviceIdKey, ""),
        settings.GetString(kDeviceTokenKey, ""),
    };
}

bool CompanionConfigStore::save(const CompanionCredentials& credentials) const
{
    if (!credentials.configured()) {
        return false;
    }
    Settings settings(kSettingsNamespace, true);
    settings.SetString(kDeviceIdKey, credentials.deviceId);
    settings.SetString(kDeviceTokenKey, credentials.deviceToken);
    return true;
}

void CompanionConfigStore::clear() const
{
    Settings settings(kSettingsNamespace, true);
    settings.SetString(kDeviceIdKey, "");
    settings.SetString(kDeviceTokenKey, "");
}
#endif

}  // namespace stackchan::companion
