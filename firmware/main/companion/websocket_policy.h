/*
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <string>
#include <string_view>

namespace stackchan::companion {

inline constexpr std::string_view kAuthorizationHeaderName = "Authorization";
inline constexpr std::string_view kDeviceIdHeaderName = "X-StackChan-Device-ID";
inline constexpr std::string_view kProtocolHeaderName = "X-StackChan-Protocol";
inline constexpr std::string_view kProtocolHeaderValue = "2";

struct SecureWebSocketTarget {
    std::string host;
    std::string path;
    int port = 443;
};

enum class WebSocketHandshakeStatus {
    accepted,
    unauthorized,
    forbidden,
    unavailable,
    protocolError,
};

bool parseSecureWebSocketUrl(std::string_view url, SecureWebSocketTarget& result);
std::string makeBearerAuthorization(std::string_view token);
WebSocketHandshakeStatus validateWebSocketHandshake(std::string_view response,
    std::string_view expectedAccept);

}  // namespace stackchan::companion
