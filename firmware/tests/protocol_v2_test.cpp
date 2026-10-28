#include "companion/protocol_v2.h"

#include <cassert>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>

#ifndef PROTOCOL_V2_EXAMPLES_DIR
#error "PROTOCOL_V2_EXAMPLES_DIR must point to the ASCII-mirrored v2 examples"
#endif

using stackchan::companion::CommandValidationStatus;
using stackchan::companion::EventRateLimiter;
using stackchan::companion::RecentCommandIds;
using stackchan::companion::HeartbeatAcknowledgement;
using stackchan::companion::WelcomeMessage;
using stackchan::companion::isValidUtf8;
using stackchan::companion::validateCommandJson;
using stackchan::companion::validateHeartbeatAckJson;
using stackchan::companion::validateWelcomeJson;

namespace {

std::string loadExample(const char* name)
{
    std::ifstream input(std::string(PROTOCOL_V2_EXAMPLES_DIR) + "/" + name, std::ios::binary);
    assert(input.good());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

}  // namespace

int main()
{
    constexpr std::int64_t receivedAt = 1700000005000;
    for (const char* name : {"accepted-expression.json", "accepted-gesture.json",
             "accepted-display-text.json", "duplicate-command.json"}) {
        const auto result = validateCommandJson(loadExample(name), receivedAt);
        assert(result.status == CommandValidationStatus::accepted);
        assert(!result.command.id.empty());
        assert(result.command.issuedAtMs > 0);
        assert(result.command.expiresAtMs > result.command.issuedAtMs);
    }

    const auto expired = validateCommandJson(loadExample("expired-command.json"), 1700000001000);
    assert(expired.status == CommandValidationStatus::expired);

    for (const char* name : {"out-of-bounds-command.json", "unknown-command.json",
             "unknown-field-command.json", "wrong-protocol-command.json",
             "empty-display-text.json"}) {
        assert(validateCommandJson(loadExample(name), receivedAt).status == CommandValidationStatus::rejected);
    }

    const auto duplicate = validateCommandJson(loadExample("duplicate-command.json"), receivedAt);
    RecentCommandIds recent;
    assert(recent.remember(duplicate.command.id));
    assert(recent.seen(duplicate.command.id));
    assert(!recent.remember(duplicate.command.id));
    for (int index = 0; index < 16; ++index) {
        std::string id = "90000000-0000-4000-8000-00000000000";
        id.back() = static_cast<char>('0' + index % 10);
        if (index >= 10) {
            id[id.size() - 2] = '1';
        }
        assert(recent.remember(id));
    }
    assert(!recent.seen(duplicate.command.id));

    EventRateLimiter limiter;
    for (std::size_t index = 0; index < stackchan::companion::kEventRateLimit; ++index) {
        assert(limiter.allow(1000 + index));
    }
    assert(!limiter.allow(1019));
    assert(limiter.allow(2000));
    assert(!limiter.allow(1999));

    assert(validateCommandJson(std::string(4097, ' '), receivedAt).status == CommandValidationStatus::rejected);
    assert(validateCommandJson("{\"protocol\":2,\"protocol\":2}", receivedAt).status == CommandValidationStatus::rejected);
    assert(validateCommandJson("{\"a\":{\"b\":{\"c\":{\"d\":1}}}}", receivedAt).status == CommandValidationStatus::rejected);
    assert(validateCommandJson("[]", receivedAt).status == CommandValidationStatus::rejected);

    WelcomeMessage welcome;
    assert(validateWelcomeJson(
        "{\"protocol\":2,\"type\":\"welcome\",\"server_time_ms\":1700000000000,\"heartbeat_interval_ms\":15000,\"heartbeat_miss_limit\":3}",
        welcome));
    assert(welcome.heartbeatIntervalMs == 15000);
    assert(!validateWelcomeJson(
        "{\"protocol\":2,\"type\":\"welcome\",\"server_time_ms\":1700000000000,\"heartbeat_interval_ms\":1,\"heartbeat_miss_limit\":3}",
        welcome));

    HeartbeatAcknowledgement acknowledgement;
    assert(validateHeartbeatAckJson(
        "{\"protocol\":2,\"type\":\"heartbeat_ack\",\"seq\":42,\"server_time_ms\":1700000000000}",
        acknowledgement));
    assert(acknowledgement.sequence == 42);
    assert(!validateHeartbeatAckJson(
        "{\"protocol\":2,\"type\":\"heartbeat_ack\",\"seq\":-1,\"server_time_ms\":1700000000000}",
        acknowledgement));

    assert(isValidUtf8("StackChan \xE5\xAE\x89\xE5\x85\xA8"));
    assert(!isValidUtf8(std::string("\xC0\xAF", 2)));
    assert(!isValidUtf8(std::string("\xED\xA0\x80", 3)));

    const std::string commandPrefix =
        "{\"protocol\":2,\"type\":\"command\",\"id\":\"bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb\","
        "\"issued_at_ms\":1700000000000,\"expires_at_ms\":1700000010000,"
        "\"command\":{\"name\":\"show_text\",\"text\":\"";
    const std::string commandSuffix = "\"}}";
    assert(validateCommandJson(commandPrefix + std::string(512, 'x') + commandSuffix,
               receivedAt).status == CommandValidationStatus::accepted);
    assert(validateCommandJson(commandPrefix + std::string(513, 'x') + commandSuffix,
               receivedAt).status == CommandValidationStatus::rejected);
    const auto emoji = validateCommandJson(commandPrefix + "\\uD83D\\uDE3A" + commandSuffix,
        receivedAt);
    assert(emoji.status == CommandValidationStatus::accepted);
    assert(emoji.command.value.size() == 4);
    assert(validateCommandJson(commandPrefix + "\\u0000" + commandSuffix,
               receivedAt).status == CommandValidationStatus::rejected);
    assert(validateCommandJson(commandPrefix + "\\n" + commandSuffix,
               receivedAt).status == CommandValidationStatus::rejected);

    return 0;
}
