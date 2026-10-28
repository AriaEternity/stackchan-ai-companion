/*
 * SPDX-License-Identifier: MIT
 */
#include "websocket_policy.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace stackchan::companion {
namespace {

std::string trim(std::string value)
{
    const auto first = std::find_if_not(value.begin(), value.end(),
        [](unsigned char character) { return std::isspace(character) != 0; });
    const auto last = std::find_if_not(value.rbegin(), value.rend(),
        [](unsigned char character) { return std::isspace(character) != 0; }).base();
    return first < last ? std::string(first, last) : std::string();
}

std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return value;
}

bool hasTrailingStatus(const std::string& statusLine, std::string_view status)
{
    return statusLine.find(std::string(" ") + std::string(status) + " ") != std::string::npos
        || (statusLine.size() >= status.size() + 1
            && statusLine.compare(statusLine.size() - status.size(), status.size(), status) == 0
            && statusLine[statusLine.size() - status.size() - 1] == ' ');
}

}  // namespace

bool parseSecureWebSocketUrl(std::string_view url, SecureWebSocketTarget& result)
{
    constexpr std::string_view prefix = "wss://";
    if (url.substr(0, prefix.size()) != prefix || url.find('?') != std::string_view::npos
        || url.find('#') != std::string_view::npos || url.find('@') != std::string_view::npos) {
        return false;
    }
    const std::size_t authorityStart = prefix.size();
    const std::size_t pathStart = url.find('/', authorityStart);
    const std::string authority(url.substr(authorityStart,
        pathStart == std::string_view::npos ? std::string_view::npos : pathStart - authorityStart));
    result.path = pathStart == std::string_view::npos ? "/" : std::string(url.substr(pathStart));
    if (authority.empty() || result.path.empty() || result.path[0] != '/') {
        return false;
    }
    const std::size_t colon = authority.rfind(':');
    if (colon == std::string::npos) {
        result.host = authority;
        result.port = 443;
    } else {
        result.host = authority.substr(0, colon);
        const std::string portText = authority.substr(colon + 1);
        if (result.host.empty() || portText.empty()
            || !std::all_of(portText.begin(), portText.end(), [](char value) { return value >= '0' && value <= '9'; })) {
            return false;
        }
        const unsigned long parsedPort = std::strtoul(portText.c_str(), nullptr, 10);
        if (parsedPort == 0 || parsedPort > 65535) {
            return false;
        }
        result.port = static_cast<int>(parsedPort);
    }
    return !result.host.empty()
        && std::all_of(result.host.begin(), result.host.end(), [](unsigned char value) {
            return std::isalnum(value) != 0 || value == '.' || value == '-';
        });
}

std::string makeBearerAuthorization(std::string_view token)
{
    std::string result = "Bearer ";
    result.append(token);
    return result;
}

WebSocketHandshakeStatus validateWebSocketHandshake(std::string_view responseView,
    std::string_view expectedAccept)
{
    if (responseView.size() > 4096 || expectedAccept.empty()) {
        return WebSocketHandshakeStatus::protocolError;
    }
    const std::string response(responseView);
    const std::size_t firstLineEnd = response.find("\r\n");
    if (firstLineEnd == std::string::npos) {
        return WebSocketHandshakeStatus::protocolError;
    }
    const std::string statusLine = response.substr(0, firstLineEnd);
    if (hasTrailingStatus(statusLine, "401")) {
        return WebSocketHandshakeStatus::unauthorized;
    }
    if (hasTrailingStatus(statusLine, "403")) {
        return WebSocketHandshakeStatus::forbidden;
    }
    if (statusLine != "HTTP/1.1 101 Switching Protocols") {
        return WebSocketHandshakeStatus::unavailable;
    }

    bool upgrade = false;
    bool connection = false;
    bool accept = false;
    unsigned int upgradeCount = 0;
    unsigned int connectionCount = 0;
    unsigned int acceptCount = 0;
    std::size_t position = firstLineEnd + 2;
    while (position < response.size()) {
        const std::size_t lineEnd = response.find("\r\n", position);
        if (lineEnd == std::string::npos || lineEnd == position) {
            break;
        }
        const std::string line = response.substr(position, lineEnd - position);
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) {
            return WebSocketHandshakeStatus::protocolError;
        }
        const std::string name = lower(trim(line.substr(0, colon)));
        const std::string value = trim(line.substr(colon + 1));
        if (name == "upgrade") {
            ++upgradeCount;
            upgrade = lower(value) == "websocket";
        } else if (name == "connection") {
            ++connectionCount;
            connection = lower(value).find("upgrade") != std::string::npos;
        } else if (name == "sec-websocket-accept") {
            ++acceptCount;
            accept = value == expectedAccept;
        }
        position = lineEnd + 2;
    }
    return upgrade && connection && accept && upgradeCount == 1 && connectionCount == 1 && acceptCount == 1
        ? WebSocketHandshakeStatus::accepted
        : WebSocketHandshakeStatus::protocolError;
}

}  // namespace stackchan::companion
