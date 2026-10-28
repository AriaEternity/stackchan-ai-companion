/*
 * SPDX-License-Identifier: MIT
 */
#include "bounded_websocket.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string_view>

#include <esp_random.h>
#include <esp_tls_errors.h>
#include <mbedtls/sha1.h>
#include <network_interface.h>
#include <sdkconfig.h>
#include <tcp.h>

#if !CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#error "Companion WSS requires the ESP-IDF certificate bundle"
#endif

#ifdef CONFIG_ESP_TLS_INSECURE
#error "Companion WSS forbids insecure ESP-TLS configuration"
#endif

namespace stackchan::companion {
namespace {

constexpr std::string_view kWebSocketGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
constexpr std::uint64_t kHandshakeTimeoutMs = 10000;

}  // namespace

BoundedWebSocket::BoundedWebSocket(NetworkInterface* network) : network_(network)
{
    handshakeEvents_ = xEventGroupCreate();
}

BoundedWebSocket::~BoundedWebSocket()
{
    close();
    wipeHeaders();
    wipeExpectedAccept();
    if (handshakeEvents_ != nullptr) {
        vEventGroupDelete(handshakeEvents_);
        handshakeEvents_ = nullptr;
    }
}

bool BoundedWebSocket::setHeader(const std::string& key, const std::string& value)
{
    if (!validHeader(key, value)) {
        return false;
    }
    headers_[key] = value;
    return true;
}

SecureWebSocketConnectResult BoundedWebSocket::connect(const std::string& secureUrl)
{
    SecureWebSocketTarget parsed;
    if (network_ == nullptr || handshakeEvents_ == nullptr || !parseSecureWebSocketUrl(secureUrl, parsed)) {
        return SecureWebSocketConnectResult::protocolError;
    }

    close();
    {
        std::lock_guard<std::mutex> lock(receiveMutex_);
        receiveBuffer_.clear();
        fragmentedMessage_.clear();
        fragmented_ = false;
        fragmentedBinary_ = false;
        handshakeComplete_ = false;
        protocolError_ = false;
        handshakeResult_ = SecureWebSocketConnectResult::unavailable;
    }
    xEventGroupClearBits(handshakeEvents_, kHandshakeCompleteBit | kHandshakeFailedBit);

    unsigned char randomKey[16] = {};
    esp_fill_random(randomKey, sizeof(randomKey));
    const std::string encodedKey = base64Encode(randomKey, sizeof(randomKey));
    std::fill(std::begin(randomKey), std::end(randomKey), 0);
    expectedAccept_ = expectedAcceptValue(encodedKey);

    tcp_ = network_->CreateSsl(2);
    if (!tcp_) {
        wipeHeaders();
        wipeExpectedAccept();
        return SecureWebSocketConnectResult::unavailable;
    }
    if (!tcp_->Connect(parsed.host, parsed.port)) {
        const int error = tcp_->GetLastError();
        wipeHeaders();
        wipeExpectedAccept();
        tcp_.reset();
        return error == ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME
            ? SecureWebSocketConnectResult::dnsFailure
            : SecureWebSocketConnectResult::tlsFailure;
    }

    tcp_->OnStream([this](const std::string& data) { handleTcpData(data); });
    tcp_->OnDisconnected([this]() {
        const bool wasConnected = connected_.exchange(false);
        if (wasConnected && disconnectedCallback_) {
            disconnectedCallback_();
        }
    });

    std::string request = "GET " + parsed.path + " HTTP/1.1\r\nHost: " + parsed.host + "\r\n";
    request += "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\n";
    request += "Sec-WebSocket-Key: " + encodedKey + "\r\n";
    for (const auto& header : headers_) {
        request += header.first + ": " + header.second + "\r\n";
    }
    request += "\r\n";
    const int sent = tcp_->Send(request);
    std::fill(request.begin(), request.end(), '\0');
    wipeHeaders();
    if (sent < 0) {
        wipeExpectedAccept();
        close();
        return SecureWebSocketConnectResult::unavailable;
    }

    const EventBits_t bits = xEventGroupWaitBits(handshakeEvents_,
        kHandshakeCompleteBit | kHandshakeFailedBit, pdFALSE, pdFALSE, pdMS_TO_TICKS(kHandshakeTimeoutMs));
    if ((bits & kHandshakeCompleteBit) != 0) {
        connected_.store(true);
        wipeExpectedAccept();
        return SecureWebSocketConnectResult::connected;
    }
    const auto result = (bits & kHandshakeFailedBit) != 0
        ? handshakeResult_
        : SecureWebSocketConnectResult::unavailable;
    wipeExpectedAccept();
    close();
    return result;
}

bool BoundedWebSocket::sendText(const std::string& payload)
{
    return payload.size() <= kMaxTextFrameBytes && isValidUtf8(payload)
        && sendFrame(0x1, payload.data(), payload.size());
}

void BoundedWebSocket::close()
{
    if (!tcp_) {
        connected_.store(false);
        return;
    }
    if (connected_.exchange(false)) {
        sendFrame(0x8, nullptr, 0);
    }
    tcp_->Disconnect();
    tcp_.reset();
}

void BoundedWebSocket::onData(std::function<void(const char*, std::size_t, bool)> callback)
{
    dataCallback_ = std::move(callback);
}

void BoundedWebSocket::onDisconnected(std::function<void()> callback)
{
    disconnectedCallback_ = std::move(callback);
}

void BoundedWebSocket::onProtocolError(std::function<void()> callback)
{
    protocolErrorCallback_ = std::move(callback);
}

std::string BoundedWebSocket::base64Encode(const unsigned char* data, std::size_t length)
{
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve(((length + 2) / 3) * 4);
    for (std::size_t index = 0; index < length; index += 3) {
        const std::uint32_t first = data[index];
        const std::uint32_t second = index + 1 < length ? data[index + 1] : 0;
        const std::uint32_t third = index + 2 < length ? data[index + 2] : 0;
        const std::uint32_t value = (first << 16) | (second << 8) | third;
        result.push_back(alphabet[(value >> 18) & 0x3F]);
        result.push_back(alphabet[(value >> 12) & 0x3F]);
        result.push_back(index + 1 < length ? alphabet[(value >> 6) & 0x3F] : '=');
        result.push_back(index + 2 < length ? alphabet[value & 0x3F] : '=');
    }
    return result;
}

std::string BoundedWebSocket::expectedAcceptValue(const std::string& key)
{
    std::string source = key;
    source.append(kWebSocketGuid);
    unsigned char digest[20] = {};
    if (mbedtls_sha1(reinterpret_cast<const unsigned char*>(source.data()), source.size(), digest) != 0) {
        std::fill(source.begin(), source.end(), '\0');
        return {};
    }
    std::fill(source.begin(), source.end(), '\0');
    const std::string result = base64Encode(digest, sizeof(digest));
    std::fill(std::begin(digest), std::end(digest), 0);
    return result;
}

bool BoundedWebSocket::validHeader(const std::string& key, const std::string& value)
{
    return !key.empty() && key.size() <= 64 && value.size() <= 256
        && key.find_first_of("\r\n:") == std::string::npos
        && value.find_first_of("\r\n") == std::string::npos;
}

void BoundedWebSocket::handleTcpData(const std::string& data)
{
    std::lock_guard<std::mutex> lock(receiveMutex_);
    if (protocolError_ || data.empty()) {
        return;
    }
    if (receiveBuffer_.size() + data.size() > kMaximumBufferedBytes) {
        markProtocolError();
        return;
    }
    receiveBuffer_.append(data);

    if (!handshakeComplete_) {
        const std::size_t headerEnd = receiveBuffer_.find("\r\n\r\n");
        if (headerEnd == std::string::npos) {
            if (receiveBuffer_.size() > kMaximumHandshakeBytes) {
                markProtocolError();
            }
            return;
        }
        const std::string response = receiveBuffer_.substr(0, headerEnd + 4);
        receiveBuffer_.erase(0, headerEnd + 4);
        const auto status = validateWebSocketHandshake(response, expectedAccept_);
        if (status != WebSocketHandshakeStatus::accepted) {
            switch (status) {
            case WebSocketHandshakeStatus::unauthorized:
                handshakeResult_ = SecureWebSocketConnectResult::unauthorized;
                break;
            case WebSocketHandshakeStatus::forbidden:
                handshakeResult_ = SecureWebSocketConnectResult::forbidden;
                break;
            case WebSocketHandshakeStatus::unavailable:
                handshakeResult_ = SecureWebSocketConnectResult::unavailable;
                break;
            case WebSocketHandshakeStatus::protocolError:
            case WebSocketHandshakeStatus::accepted:
                handshakeResult_ = SecureWebSocketConnectResult::protocolError;
                break;
            }
            xEventGroupSetBits(handshakeEvents_, kHandshakeFailedBit);
            return;
        }
        handshakeComplete_ = true;
        xEventGroupSetBits(handshakeEvents_, kHandshakeCompleteBit);
    }
    processFrames();
}

void BoundedWebSocket::processFrames()
{
    while (receiveBuffer_.size() >= 2 && !protocolError_) {
        const auto* bytes = reinterpret_cast<const unsigned char*>(receiveBuffer_.data());
        const bool fin = (bytes[0] & 0x80) != 0;
        const std::uint8_t opcode = bytes[0] & 0x0F;
        const bool masked = (bytes[1] & 0x80) != 0;
        std::uint64_t payloadLength = bytes[1] & 0x7F;
        std::size_t headerLength = 2;
        if (masked) {
            markProtocolError();
            return;
        }
        if (payloadLength == 126) {
            if (receiveBuffer_.size() < 4) {
                return;
            }
            payloadLength = (static_cast<std::uint64_t>(bytes[2]) << 8) | bytes[3];
            headerLength = 4;
        } else if (payloadLength == 127) {
            if (receiveBuffer_.size() < 10) {
                return;
            }
            payloadLength = 0;
            for (int index = 0; index < 8; ++index) {
                payloadLength = (payloadLength << 8) | bytes[2 + index];
            }
            headerLength = 10;
        }
        const bool control = opcode >= 0x8;
        if (payloadLength > kMaxTextFrameBytes || (control && (!fin || payloadLength > 125))
            || payloadLength > std::numeric_limits<std::size_t>::max() - headerLength) {
            markProtocolError();
            return;
        }
        const std::size_t frameLength = headerLength + static_cast<std::size_t>(payloadLength);
        if (receiveBuffer_.size() < frameLength) {
            return;
        }
        const char* payload = receiveBuffer_.data() + headerLength;

        if (opcode == 0x8) {
            connected_.store(false);
            receiveBuffer_.erase(0, frameLength);
            if (disconnectedCallback_) {
                disconnectedCallback_();
            }
            return;
        }
        if (opcode == 0x9) {
            sendFrame(0xA, payload, static_cast<std::size_t>(payloadLength));
        } else if (opcode == 0xA) {
            // WebSocket pong is transport-local; protocol heartbeat remains authoritative.
        } else if (opcode == 0x1 || opcode == 0x2 || opcode == 0x0) {
            if (opcode == 0x0) {
                if (!fragmented_) {
                    markProtocolError();
                    return;
                }
            } else {
                if (fragmented_) {
                    markProtocolError();
                    return;
                }
                fragmented_ = !fin;
                fragmentedBinary_ = opcode == 0x2;
                fragmentedMessage_.clear();
            }
            if (fragmentedMessage_.size() + payloadLength > kMaxTextFrameBytes) {
                markProtocolError();
                return;
            }
            fragmentedMessage_.insert(fragmentedMessage_.end(), payload, payload + payloadLength);
            if (fin) {
                const bool binary = opcode == 0x2 || fragmentedBinary_;
                if (!binary && !isValidUtf8(std::string_view(fragmentedMessage_.data(), fragmentedMessage_.size()))) {
                    markProtocolError();
                    return;
                }
                if (dataCallback_) {
                    dataCallback_(fragmentedMessage_.data(), fragmentedMessage_.size(), binary);
                }
                fragmentedMessage_.clear();
                fragmented_ = false;
                fragmentedBinary_ = false;
            }
        } else {
            markProtocolError();
            return;
        }
        receiveBuffer_.erase(0, frameLength);
    }
}

bool BoundedWebSocket::sendFrame(std::uint8_t opcode, const void* data, std::size_t length)
{
    if (!tcp_ || length > kMaxTextFrameBytes || (opcode >= 0x8 && length > 125)) {
        return false;
    }
    std::string frame;
    frame.reserve(length + 8);
    frame.push_back(static_cast<char>(0x80 | opcode));
    if (length < 126) {
        frame.push_back(static_cast<char>(0x80 | length));
    } else {
        frame.push_back(static_cast<char>(0x80 | 126));
        frame.push_back(static_cast<char>((length >> 8) & 0xFF));
        frame.push_back(static_cast<char>(length & 0xFF));
    }
    unsigned char mask[4] = {};
    esp_fill_random(mask, sizeof(mask));
    frame.append(reinterpret_cast<const char*>(mask), sizeof(mask));
    const auto* payload = static_cast<const unsigned char*>(data);
    for (std::size_t index = 0; index < length; ++index) {
        frame.push_back(static_cast<char>(payload[index] ^ mask[index % sizeof(mask)]));
    }
    std::fill(std::begin(mask), std::end(mask), 0);
    std::lock_guard<std::mutex> lock(sendMutex_);
    return tcp_->Send(frame) >= 0;
}

void BoundedWebSocket::markProtocolError()
{
    if (protocolError_) {
        return;
    }
    protocolError_ = true;
    handshakeResult_ = SecureWebSocketConnectResult::protocolError;
    if (!handshakeComplete_) {
        xEventGroupSetBits(handshakeEvents_, kHandshakeFailedBit);
    } else if (protocolErrorCallback_) {
        protocolErrorCallback_();
    }
}

void BoundedWebSocket::wipeHeaders()
{
    for (auto& header : headers_) {
        std::fill(header.second.begin(), header.second.end(), '\0');
    }
    headers_.clear();
}

void BoundedWebSocket::wipeExpectedAccept()
{
    std::fill(expectedAccept_.begin(), expectedAccept_.end(), '\0');
    expectedAccept_.clear();
}

}  // namespace stackchan::companion
