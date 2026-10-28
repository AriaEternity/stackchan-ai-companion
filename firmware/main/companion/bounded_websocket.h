/*
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "protocol_v2.h"
#include "websocket_policy.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

class NetworkInterface;
class Tcp;

namespace stackchan::companion {

enum class SecureWebSocketConnectResult {
    connected,
    dnsFailure,
    tlsFailure,
    unauthorized,
    forbidden,
    unavailable,
    protocolError,
};

class BoundedWebSocket {
public:
    explicit BoundedWebSocket(NetworkInterface* network);
    ~BoundedWebSocket();

    BoundedWebSocket(const BoundedWebSocket&) = delete;
    BoundedWebSocket& operator=(const BoundedWebSocket&) = delete;

    bool setHeader(const std::string& key, const std::string& value);
    SecureWebSocketConnectResult connect(const std::string& secureUrl);
    bool sendText(const std::string& payload);
    bool connected() const { return connected_.load(); }
    void close();

    void onData(std::function<void(const char*, std::size_t, bool)> callback);
    void onDisconnected(std::function<void()> callback);
    void onProtocolError(std::function<void()> callback);

private:
    static std::string base64Encode(const unsigned char* data, std::size_t length);
    static std::string expectedAcceptValue(const std::string& key);
    static bool validHeader(const std::string& key, const std::string& value);

    void handleTcpData(const std::string& data);
    void processFrames();
    bool sendFrame(std::uint8_t opcode, const void* data, std::size_t length);
    void markProtocolError();
    void wipeHeaders();
    void wipeExpectedAccept();

    NetworkInterface* network_ = nullptr;
    std::unique_ptr<Tcp> tcp_;
    EventGroupHandle_t handshakeEvents_ = nullptr;
    std::map<std::string, std::string> headers_;
    std::string expectedAccept_;
    std::string receiveBuffer_;
    std::vector<char> fragmentedMessage_;
    bool handshakeComplete_ = false;
    bool fragmented_ = false;
    bool fragmentedBinary_ = false;
    bool protocolError_ = false;
    SecureWebSocketConnectResult handshakeResult_ = SecureWebSocketConnectResult::unavailable;
    std::atomic<bool> connected_{false};
    std::mutex receiveMutex_;
    std::mutex sendMutex_;
    std::function<void(const char*, std::size_t, bool)> dataCallback_;
    std::function<void()> disconnectedCallback_;
    std::function<void()> protocolErrorCallback_;

    static constexpr EventBits_t kHandshakeCompleteBit = BIT0;
    static constexpr EventBits_t kHandshakeFailedBit = BIT1;
    static constexpr std::size_t kMaximumHandshakeBytes = 4096;
    static constexpr std::size_t kMaximumBufferedBytes = kMaximumHandshakeBytes + kMaxTextFrameBytes + 32;
};

}  // namespace stackchan::companion
