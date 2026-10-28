/*
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "protocol_v2.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace stackchan::companion {

constexpr std::uint64_t kWelcomeTimeoutMs = 10000;
constexpr std::uint64_t kReconnectMinimumMs = 1000;
constexpr std::uint64_t kReconnectMaximumMs = 60000;
constexpr std::size_t kConnectionActionCapacity = 8;
constexpr std::size_t kPendingCommandActionCapacity = 4;

enum class ConnectionState {
    unconfigured,
    wifiConnecting,
    serverConnecting,
    online,
    degraded,
    offline,
};

enum class ConnectionFailure {
    dns,
    tls,
    unauthorized,
    forbidden,
    unavailable,
    protocol,
};

enum class ConnectionActionType {
    connect,
    sendText,
    closeTransport,
    clearRemoteCommands,
    enterOfflineSafety,
    dispatchCommand,
};

struct ConnectionAction {
    ConnectionActionType type = ConnectionActionType::connect;
    std::string payload;
    ProtocolCommand command;
    std::uint64_t commandDeadlineMs = 0;
};

const char* connectionStateName(ConnectionState state);

class ConnectionController {
public:
    explicit ConnectionController(std::uint32_t jitterSeed = 0x51ACCAFEU);

    void reset(bool configured, bool wifiConnected, std::uint64_t nowMs);
    void onWifiChanged(bool connected, std::uint64_t nowMs);
    void onTransportConnected(std::uint64_t nowMs);
    void onTransportFailure(ConnectionFailure failure, std::uint64_t nowMs);
    void onTransportClosed(std::uint64_t nowMs);
    void onTextFrame(std::string_view frame, std::uint64_t nowMs);
    void onBinaryFrame(std::uint64_t nowMs);
    void queueCommandAcknowledgement(std::string_view commandId, std::string_view status,
        std::string_view reason = {});
    bool queueDeviceEvent(std::string payload);
    void tick(std::uint64_t nowMs, std::uint64_t uptimeMs);

    ConnectionState state() const { return state_; }
    bool popAction(ConnectionAction& action);
    std::size_t pendingActionCount() const { return actionCount_; }
    std::uint64_t nextRetryAtMs() const { return nextRetryAtMs_; }
    std::uint64_t lastRetryDelayMs() const { return lastRetryDelayMs_; }

private:
    void requestConnect();
    void failConnection(ConnectionFailure failure, std::uint64_t nowMs);
    void scheduleReconnect(ConnectionFailure failure, std::uint64_t nowMs);
    void sendHeartbeat(std::uint64_t uptimeMs);
    void sendCommandAck(std::string_view commandId, std::string_view status,
        std::string_view reason = {});
    void pushAction(ConnectionActionType type, std::string payload = {});
    void pushCommandAction(ProtocolCommand command, std::uint64_t deadlineMs);
    std::uint32_t nextRandom();
    std::int64_t estimatedServerTime(std::uint64_t nowMs) const;

    ConnectionState state_ = ConnectionState::unconfigured;
    bool configured_ = false;
    bool wifiConnected_ = false;
    bool transportConnected_ = false;
    bool awaitingWelcome_ = false;
    bool awaitingHeartbeatAck_ = false;
    std::uint64_t welcomeDeadlineMs_ = 0;
    std::uint64_t nextHeartbeatAtMs_ = 0;
    std::uint64_t nextRetryAtMs_ = 0;
    std::uint64_t lastRetryDelayMs_ = 0;
    std::uint64_t serverTimeAtWelcomeMs_ = 0;
    std::uint64_t monotonicAtWelcomeMs_ = 0;
    std::uint32_t consecutiveFailures_ = 0;
    std::uint32_t missedHeartbeatAcks_ = 0;
    std::int32_t heartbeatSequence_ = -1;
    std::uint32_t randomState_ = 0;

    std::array<ConnectionAction, kConnectionActionCapacity> actions_{};
    std::size_t actionHead_ = 0;
    std::size_t actionCount_ = 0;
};

}  // namespace stackchan::companion
