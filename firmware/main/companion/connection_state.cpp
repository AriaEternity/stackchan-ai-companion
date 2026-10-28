/*
 * SPDX-License-Identifier: MIT
 */
#include "connection_state.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace stackchan::companion {

const char* connectionStateName(ConnectionState state)
{
    switch (state) {
    case ConnectionState::unconfigured: return "UNCONFIGURED";
    case ConnectionState::wifiConnecting: return "WIFI CONNECTING";
    case ConnectionState::serverConnecting: return "SERVER CONNECTING";
    case ConnectionState::online: return "ONLINE";
    case ConnectionState::degraded: return "DEGRADED";
    case ConnectionState::offline: return "OFFLINE SAFE";
    }
    return "OFFLINE SAFE";
}

ConnectionController::ConnectionController(std::uint32_t jitterSeed)
    : randomState_(jitterSeed == 0 ? 0x51ACCAFEU : jitterSeed)
{
}

void ConnectionController::reset(bool configured, bool wifiConnected, std::uint64_t nowMs)
{
    configured_ = configured;
    wifiConnected_ = wifiConnected;
    transportConnected_ = false;
    awaitingWelcome_ = false;
    awaitingHeartbeatAck_ = false;
    consecutiveFailures_ = 0;
    missedHeartbeatAcks_ = 0;
    heartbeatSequence_ = -1;
    nextRetryAtMs_ = nowMs;
    lastRetryDelayMs_ = 0;
    actionHead_ = 0;
    actionCount_ = 0;

    if (!configured_) {
        state_ = ConnectionState::unconfigured;
    } else if (!wifiConnected_) {
        state_ = ConnectionState::wifiConnecting;
    } else {
        state_ = ConnectionState::serverConnecting;
        requestConnect();
    }
}

void ConnectionController::onWifiChanged(bool connected, std::uint64_t nowMs)
{
    if (connected == wifiConnected_) {
        return;
    }
    wifiConnected_ = connected;
    if (!configured_) {
        state_ = ConnectionState::unconfigured;
        return;
    }
    if (!wifiConnected_) {
        transportConnected_ = false;
        awaitingWelcome_ = false;
        awaitingHeartbeatAck_ = false;
        state_ = ConnectionState::wifiConnecting;
        pushAction(ConnectionActionType::closeTransport);
        pushAction(ConnectionActionType::clearRemoteCommands);
        pushAction(ConnectionActionType::enterOfflineSafety);
        return;
    }

    consecutiveFailures_ = 0;
    nextRetryAtMs_ = nowMs;
    state_ = ConnectionState::serverConnecting;
    requestConnect();
}

void ConnectionController::onTransportConnected(std::uint64_t nowMs)
{
    if (!configured_ || !wifiConnected_ || state_ != ConnectionState::serverConnecting) {
        pushAction(ConnectionActionType::closeTransport);
        return;
    }
    transportConnected_ = true;
    awaitingWelcome_ = true;
    welcomeDeadlineMs_ = nowMs + kWelcomeTimeoutMs;
}

void ConnectionController::onTransportFailure(ConnectionFailure failure, std::uint64_t nowMs)
{
    failConnection(failure, nowMs);
}

void ConnectionController::onTransportClosed(std::uint64_t nowMs)
{
    if (!transportConnected_ && (state_ == ConnectionState::offline
            || state_ == ConnectionState::wifiConnecting || state_ == ConnectionState::unconfigured)) {
        return;
    }
    failConnection(ConnectionFailure::unavailable, nowMs);
}

void ConnectionController::onTextFrame(std::string_view frame, std::uint64_t nowMs)
{
    if (frame.size() > kMaxTextFrameBytes || !isValidUtf8(frame)) {
        failConnection(ConnectionFailure::protocol, nowMs);
        return;
    }

    if (state_ == ConnectionState::serverConnecting && transportConnected_ && awaitingWelcome_) {
        WelcomeMessage welcome;
        if (!validateWelcomeJson(frame, welcome)) {
            failConnection(ConnectionFailure::protocol, nowMs);
            return;
        }
        awaitingWelcome_ = false;
        state_ = ConnectionState::online;
        consecutiveFailures_ = 0;
        missedHeartbeatAcks_ = 0;
        heartbeatSequence_ = -1;
        serverTimeAtWelcomeMs_ = static_cast<std::uint64_t>(welcome.serverTimeMs);
        monotonicAtWelcomeMs_ = nowMs;
        sendHeartbeat(nowMs);
        nextHeartbeatAtMs_ = nowMs + welcome.heartbeatIntervalMs;
        return;
    }

    if (state_ != ConnectionState::online && state_ != ConnectionState::degraded) {
        return;
    }

    HeartbeatAcknowledgement acknowledgement;
    if (validateHeartbeatAckJson(frame, acknowledgement)) {
        if (acknowledgement.sequence == heartbeatSequence_) {
            awaitingHeartbeatAck_ = false;
            missedHeartbeatAcks_ = 0;
            state_ = ConnectionState::online;
            serverTimeAtWelcomeMs_ = static_cast<std::uint64_t>(acknowledgement.serverTimeMs);
            monotonicAtWelcomeMs_ = nowMs;
        }
        return;
    }

    const std::int64_t serverNowMs = estimatedServerTime(nowMs);
    auto command = validateCommandJson(frame, serverNowMs);
    if (command.status == CommandValidationStatus::accepted) {
        const std::uint64_t remainingMs = command.command.expiresAtMs > serverNowMs
            ? static_cast<std::uint64_t>(command.command.expiresAtMs - serverNowMs)
            : 0;
        const std::uint64_t deadlineMs = nowMs > std::numeric_limits<std::uint64_t>::max() - remainingMs
            ? std::numeric_limits<std::uint64_t>::max()
            : nowMs + remainingMs;
        pushCommandAction(std::move(command.command), deadlineMs);
        return;
    }
    if (command.status == CommandValidationStatus::expired && !command.command.id.empty()) {
        sendCommandAck(command.command.id, "expired");
        return;
    }
    if (!command.command.id.empty()) {
        sendCommandAck(command.command.id, "rejected", "invalid");
        return;
    }
    // There is no protocol-valid command ID to acknowledge. Discard the malformed
    // application message without executing partial data or turning an isolated
    // parse failure into a reconnect loop. Transport-level violations still close.
}

void ConnectionController::onBinaryFrame(std::uint64_t nowMs)
{
    failConnection(ConnectionFailure::protocol, nowMs);
}

void ConnectionController::queueCommandAcknowledgement(std::string_view commandId,
    std::string_view status, std::string_view reason)
{
    sendCommandAck(commandId, status, reason);
}

bool ConnectionController::queueDeviceEvent(std::string payload)
{
    if ((state_ != ConnectionState::online && state_ != ConnectionState::degraded)
        || payload.empty() || payload.size() > kMaxTextFrameBytes
        || actionCount_ >= actions_.size()) {
        return false;
    }
    pushAction(ConnectionActionType::sendText, std::move(payload));
    return true;
}

void ConnectionController::tick(std::uint64_t nowMs, std::uint64_t uptimeMs)
{
    if (!configured_ || !wifiConnected_) {
        return;
    }
    if (state_ == ConnectionState::serverConnecting && transportConnected_ && awaitingWelcome_
        && nowMs >= welcomeDeadlineMs_) {
        failConnection(ConnectionFailure::unavailable, nowMs);
        return;
    }
    if (state_ == ConnectionState::offline && nowMs >= nextRetryAtMs_) {
        state_ = ConnectionState::serverConnecting;
        requestConnect();
        return;
    }
    if ((state_ != ConnectionState::online && state_ != ConnectionState::degraded)
        || nowMs < nextHeartbeatAtMs_) {
        return;
    }

    if (awaitingHeartbeatAck_) {
        ++missedHeartbeatAcks_;
        if (missedHeartbeatAcks_ >= kHeartbeatMissLimit) {
            failConnection(ConnectionFailure::unavailable, nowMs);
            return;
        }
        state_ = ConnectionState::degraded;
    }
    sendHeartbeat(uptimeMs);
    nextHeartbeatAtMs_ = nowMs + kHeartbeatIntervalMs;
}

bool ConnectionController::popAction(ConnectionAction& action)
{
    if (actionCount_ == 0) {
        return false;
    }
    action = std::move(actions_[actionHead_]);
    actions_[actionHead_] = {};
    actionHead_ = (actionHead_ + 1) % actions_.size();
    --actionCount_;
    return true;
}

void ConnectionController::requestConnect()
{
    transportConnected_ = false;
    awaitingWelcome_ = false;
    awaitingHeartbeatAck_ = false;
    pushAction(ConnectionActionType::connect);
}

void ConnectionController::failConnection(ConnectionFailure failure, std::uint64_t nowMs)
{
    transportConnected_ = false;
    awaitingWelcome_ = false;
    awaitingHeartbeatAck_ = false;
    if (!configured_) {
        state_ = ConnectionState::unconfigured;
        return;
    }
    if (!wifiConnected_) {
        state_ = ConnectionState::wifiConnecting;
    } else {
        state_ = ConnectionState::offline;
        scheduleReconnect(failure, nowMs);
    }
    pushAction(ConnectionActionType::closeTransport);
    pushAction(ConnectionActionType::clearRemoteCommands);
    pushAction(ConnectionActionType::enterOfflineSafety);
}

void ConnectionController::scheduleReconnect(ConnectionFailure failure, std::uint64_t nowMs)
{
    ++consecutiveFailures_;
    const std::uint32_t shift = std::min<std::uint32_t>(consecutiveFailures_ - 1, 6);
    std::uint64_t base = std::min<std::uint64_t>(kReconnectMinimumMs << shift, kReconnectMaximumMs);
    if (failure == ConnectionFailure::unauthorized || failure == ConnectionFailure::forbidden) {
        base = std::max<std::uint64_t>(base, 30000);
    }
    const std::uint64_t jitterRange = base / 4;
    const std::uint64_t jitter = jitterRange == 0 ? 0 : nextRandom() % (jitterRange + 1);
    lastRetryDelayMs_ = std::min<std::uint64_t>(base + jitter, kReconnectMaximumMs);
    nextRetryAtMs_ = nowMs + lastRetryDelayMs_;
}

void ConnectionController::sendHeartbeat(std::uint64_t uptimeMs)
{
    if (heartbeatSequence_ == std::numeric_limits<std::int32_t>::max()) {
        heartbeatSequence_ = 0;
    } else {
        ++heartbeatSequence_;
    }
    const auto boundedUptime = std::min<std::uint64_t>(uptimeMs,
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()));
    std::string payload = "{\"protocol\":2,\"type\":\"heartbeat\",\"seq\":";
    payload += std::to_string(heartbeatSequence_);
    payload += ",\"uptime_ms\":";
    payload += std::to_string(boundedUptime);
    payload += '}';
    pushAction(ConnectionActionType::sendText, std::move(payload));
    awaitingHeartbeatAck_ = true;
}

void ConnectionController::sendCommandAck(std::string_view commandId, std::string_view status,
    std::string_view reason)
{
    std::string payload = "{\"protocol\":2,\"type\":\"command_ack\",\"id\":\"";
    payload.append(commandId);
    payload += "\",\"status\":\"";
    payload.append(status);
    payload += '"';
    if (!reason.empty()) {
        payload += ",\"reason\":\"";
        payload.append(reason);
        payload += '"';
    }
    payload += '}';
    pushAction(ConnectionActionType::sendText, std::move(payload));
}

void ConnectionController::pushAction(ConnectionActionType type, std::string payload)
{
    if (actionCount_ == actions_.size()) {
        // A full transport queue is itself unsafe. Discard stale work and preserve the newest action.
        actionHead_ = 0;
        actionCount_ = 0;
    }
    const std::size_t tail = (actionHead_ + actionCount_) % actions_.size();
    actions_[tail] = {type, std::move(payload)};
    ++actionCount_;
}

void ConnectionController::pushCommandAction(ProtocolCommand command, std::uint64_t deadlineMs)
{
    std::size_t pendingCommands = 0;
    for (std::size_t index = 0; index < actionCount_; ++index) {
        if (actions_[(actionHead_ + index) % actions_.size()].type
            == ConnectionActionType::dispatchCommand) {
            ++pendingCommands;
        }
    }
    if (pendingCommands >= kPendingCommandActionCapacity) {
        sendCommandAck(command.id, "busy");
        return;
    }
    const std::size_t tail = (actionHead_ + actionCount_) % actions_.size();
    actions_[tail].type = ConnectionActionType::dispatchCommand;
    actions_[tail].command = std::move(command);
    actions_[tail].commandDeadlineMs = deadlineMs;
    ++actionCount_;
}

std::uint32_t ConnectionController::nextRandom()
{
    randomState_ ^= randomState_ << 13;
    randomState_ ^= randomState_ >> 17;
    randomState_ ^= randomState_ << 5;
    return randomState_;
}

std::int64_t ConnectionController::estimatedServerTime(std::uint64_t nowMs) const
{
    const std::uint64_t elapsed = nowMs >= monotonicAtWelcomeMs_ ? nowMs - monotonicAtWelcomeMs_ : 0;
    const std::uint64_t estimate = serverTimeAtWelcomeMs_ > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) - elapsed
        ? static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
        : serverTimeAtWelcomeMs_ + elapsed;
    return static_cast<std::int64_t>(estimate);
}

}  // namespace stackchan::companion
