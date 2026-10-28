/*
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace stackchan::companion {

constexpr int kProtocolVersion = 1;
constexpr std::size_t kMaxTextFrameBytes = 4096;
constexpr std::size_t kMaxJsonDepth = 4;
constexpr std::size_t kMaxJsonStringBytes = 128;
constexpr std::size_t kMaxJsonObjectMembers = 16;
constexpr std::int64_t kMaxCommandTtlMs = 30000;
constexpr std::int64_t kMaxCommandFutureSkewMs = 5000;
constexpr std::size_t kRecentCommandIdCapacity = 16;
constexpr std::size_t kEventRateLimit = 20;
constexpr std::uint64_t kEventRateWindowMs = 1000;
constexpr std::uint64_t kHeartbeatIntervalMs = 15000;
constexpr std::uint32_t kHeartbeatMissLimit = 3;

enum class CommandValidationStatus {
    accepted,
    expired,
    rejected,
};

struct ProtocolCommand {
    std::string id;
    std::string name;
    std::string value;
    double intensity = 0.0;
    std::int64_t issuedAtMs = 0;
    std::int64_t expiresAtMs = 0;
};

struct CommandValidationResult {
    CommandValidationStatus status = CommandValidationStatus::rejected;
    ProtocolCommand command;
};

struct WelcomeMessage {
    std::int64_t serverTimeMs = 0;
    std::uint64_t heartbeatIntervalMs = 0;
    std::uint32_t heartbeatMissLimit = 0;
};

struct HeartbeatAcknowledgement {
    std::int32_t sequence = 0;
    std::int64_t serverTimeMs = 0;
};

bool isValidUtf8(std::string_view value);
CommandValidationResult validateCommandJson(std::string_view frame, std::int64_t receivedAtMs);
bool validateWelcomeJson(std::string_view frame, WelcomeMessage& result);
bool validateHeartbeatAckJson(std::string_view frame, HeartbeatAcknowledgement& result);

class RecentCommandIds {
public:
    bool seen(std::string_view commandId) const;
    bool remember(std::string_view commandId);

private:
    std::array<std::string, kRecentCommandIdCapacity> ids_{};
    std::size_t count_ = 0;
    std::size_t next_ = 0;
};

class EventRateLimiter {
public:
    bool allow(std::uint64_t nowMs);

private:
    std::array<std::uint64_t, kEventRateLimit> events_{};
    std::size_t head_ = 0;
    std::size_t count_ = 0;
    std::uint64_t lastNowMs_ = 0;
    bool hasLastNow_ = false;
};

}  // namespace stackchan::companion
