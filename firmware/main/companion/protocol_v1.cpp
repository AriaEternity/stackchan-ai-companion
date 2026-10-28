/*
 * SPDX-License-Identifier: MIT
 */
#include "protocol_v1.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace stackchan::companion {
namespace {

enum class JsonType {
    nullValue,
    boolean,
    number,
    string,
    object,
};

struct JsonValue {
    JsonType type = JsonType::nullValue;
    bool booleanValue = false;
    double numberValue = 0.0;
    bool isInteger = false;
    std::int64_t integerValue = 0;
    std::string stringValue;
    std::vector<std::pair<std::string, JsonValue>> objectValue;
};

class JsonParser {
public:
    explicit JsonParser(std::string_view input) : input_(input) {}

    bool parse(JsonValue& result)
    {
        if (input_.size() > kMaxTextFrameBytes || !isValidUtf8(input_)) {
            return false;
        }
        skipWhitespace();
        if (!parseValue(result, 1)) {
            return false;
        }
        skipWhitespace();
        return position_ == input_.size();
    }

private:
    bool parseValue(JsonValue& result, std::size_t depth)
    {
        if (depth > kMaxJsonDepth || position_ >= input_.size()) {
            return false;
        }
        const char current = input_[position_];
        if (current == '{') {
            return parseObject(result, depth);
        }
        if (current == '[') {
            return false;
        }
        if (current == '"') {
            result.type = JsonType::string;
            return parseString(result.stringValue);
        }
        if (current == 't') {
            result.type = JsonType::boolean;
            result.booleanValue = true;
            return consumeLiteral("true");
        }
        if (current == 'f') {
            result.type = JsonType::boolean;
            result.booleanValue = false;
            return consumeLiteral("false");
        }
        if (current == 'n') {
            result.type = JsonType::nullValue;
            return consumeLiteral("null");
        }
        if (current == '-' || (current >= '0' && current <= '9')) {
            return parseNumber(result);
        }
        return false;
    }

    bool parseObject(JsonValue& result, std::size_t depth)
    {
        result.type = JsonType::object;
        result.objectValue.clear();
        ++position_;
        skipWhitespace();
        if (consume('}')) {
            return true;
        }
        while (position_ < input_.size()) {
            if (result.objectValue.size() >= kMaxJsonObjectMembers || input_[position_] != '"') {
                return false;
            }
            std::string key;
            if (!parseString(key)) {
                return false;
            }
            if (std::any_of(result.objectValue.begin(), result.objectValue.end(),
                    [&key](const auto& item) { return item.first == key; })) {
                return false;
            }
            skipWhitespace();
            if (!consume(':')) {
                return false;
            }
            skipWhitespace();
            JsonValue child;
            if (!parseValue(child, depth + 1)) {
                return false;
            }
            result.objectValue.emplace_back(std::move(key), std::move(child));
            skipWhitespace();
            if (consume('}')) {
                return true;
            }
            if (!consume(',')) {
                return false;
            }
            skipWhitespace();
        }
        return false;
    }

    bool parseString(std::string& result)
    {
        if (!consume('"')) {
            return false;
        }
        result.clear();
        while (position_ < input_.size()) {
            const unsigned char current = static_cast<unsigned char>(input_[position_++]);
            if (current == '"') {
                return result.size() <= kMaxJsonStringBytes;
            }
            if (current < 0x20) {
                return false;
            }
            if (current != '\\') {
                result.push_back(static_cast<char>(current));
            } else {
                if (position_ >= input_.size()) {
                    return false;
                }
                const char escaped = input_[position_++];
                switch (escaped) {
                case '"': result.push_back('"'); break;
                case '\\': result.push_back('\\'); break;
                case '/': result.push_back('/'); break;
                case 'b': result.push_back('\b'); break;
                case 'f': result.push_back('\f'); break;
                case 'n': result.push_back('\n'); break;
                case 'r': result.push_back('\r'); break;
                case 't': result.push_back('\t'); break;
                case 'u':
                    if (!parseUnicodeEscape(result)) {
                        return false;
                    }
                    break;
                default:
                    return false;
                }
            }
            if (result.size() > kMaxJsonStringBytes) {
                return false;
            }
        }
        return false;
    }

    bool parseUnicodeEscape(std::string& result)
    {
        if (position_ + 4 > input_.size()) {
            return false;
        }
        unsigned int codePoint = 0;
        for (int index = 0; index < 4; ++index) {
            const char value = input_[position_++];
            codePoint <<= 4;
            if (value >= '0' && value <= '9') {
                codePoint += static_cast<unsigned int>(value - '0');
            } else if (value >= 'a' && value <= 'f') {
                codePoint += static_cast<unsigned int>(value - 'a' + 10);
            } else if (value >= 'A' && value <= 'F') {
                codePoint += static_cast<unsigned int>(value - 'A' + 10);
            } else {
                return false;
            }
        }
        if (codePoint >= 0xD800 && codePoint <= 0xDFFF) {
            return false;
        }
        if (codePoint <= 0x7F) {
            result.push_back(static_cast<char>(codePoint));
        } else if (codePoint <= 0x7FF) {
            result.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
            result.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        } else {
            result.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
            result.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        }
        return true;
    }

    bool parseNumber(JsonValue& result)
    {
        const std::size_t start = position_;
        if (consume('-') && position_ >= input_.size()) {
            return false;
        }
        if (consume('0')) {
            if (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') {
                return false;
            }
        } else {
            if (position_ >= input_.size() || input_[position_] < '1' || input_[position_] > '9') {
                return false;
            }
            while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') {
                ++position_;
            }
        }
        bool integer = true;
        if (consume('.')) {
            integer = false;
            if (position_ >= input_.size() || input_[position_] < '0' || input_[position_] > '9') {
                return false;
            }
            while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') {
                ++position_;
            }
        }
        if (position_ < input_.size() && (input_[position_] == 'e' || input_[position_] == 'E')) {
            integer = false;
            ++position_;
            if (position_ < input_.size() && (input_[position_] == '+' || input_[position_] == '-')) {
                ++position_;
            }
            if (position_ >= input_.size() || input_[position_] < '0' || input_[position_] > '9') {
                return false;
            }
            while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') {
                ++position_;
            }
        }

        const std::string text(input_.substr(start, position_ - start));
        char* end = nullptr;
        result.numberValue = std::strtod(text.c_str(), &end);
        if (end != text.c_str() + text.size() || !std::isfinite(result.numberValue)) {
            return false;
        }
        result.type = JsonType::number;
        result.isInteger = false;
        if (integer) {
            std::int64_t parsed = 0;
            const auto conversion = std::from_chars(text.data(), text.data() + text.size(), parsed);
            if (conversion.ec == std::errc() && conversion.ptr == text.data() + text.size()) {
                result.isInteger = true;
                result.integerValue = parsed;
            }
        }
        return true;
    }

    bool consumeLiteral(std::string_view literal)
    {
        if (input_.substr(position_, literal.size()) != literal) {
            return false;
        }
        position_ += literal.size();
        return true;
    }

    bool consume(char expected)
    {
        if (position_ >= input_.size() || input_[position_] != expected) {
            return false;
        }
        ++position_;
        return true;
    }

    void skipWhitespace()
    {
        while (position_ < input_.size()) {
            const char value = input_[position_];
            if (value != ' ' && value != '\t' && value != '\r' && value != '\n') {
                return;
            }
            ++position_;
        }
    }

    std::string_view input_;
    std::size_t position_ = 0;
};

const JsonValue* field(const JsonValue& object, std::string_view name)
{
    if (object.type != JsonType::object) {
        return nullptr;
    }
    const auto found = std::find_if(object.objectValue.begin(), object.objectValue.end(),
        [name](const auto& item) { return item.first == name; });
    return found == object.objectValue.end() ? nullptr : &found->second;
}

bool exactFields(const JsonValue& object, std::initializer_list<std::string_view> names)
{
    if (object.type != JsonType::object || object.objectValue.size() != names.size()) {
        return false;
    }
    return std::all_of(names.begin(), names.end(),
        [&object](std::string_view name) { return field(object, name) != nullptr; });
}

bool stringEquals(const JsonValue* value, std::string_view expected)
{
    return value != nullptr && value->type == JsonType::string && value->stringValue == expected;
}

bool isCanonicalUuid4(std::string_view value)
{
    if (value.size() != 36 || value[8] != '-' || value[13] != '-' || value[18] != '-' || value[23] != '-' || value[14] != '4') {
        return false;
    }
    if (value[19] != '8' && value[19] != '9' && value[19] != 'a' && value[19] != 'b') {
        return false;
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            continue;
        }
        const char current = value[index];
        if (!((current >= '0' && current <= '9') || (current >= 'a' && current <= 'f'))) {
            return false;
        }
    }
    return true;
}

bool inSet(std::string_view value, std::initializer_list<std::string_view> values)
{
    return std::find(values.begin(), values.end(), value) != values.end();
}

bool validatePayload(const JsonValue& payload, ProtocolCommand& command)
{
    const JsonValue* name = field(payload, "name");
    if (name == nullptr || name->type != JsonType::string) {
        return false;
    }
    command.name = name->stringValue;
    if (command.name == "set_expression") {
        const JsonValue* expression = field(payload, "expression");
        if (!exactFields(payload, {"name", "expression"}) || expression == nullptr || expression->type != JsonType::string
            || !inSet(expression->stringValue, {"neutral", "happy", "shy", "sad", "angry", "sleepy", "surprised"})) {
            return false;
        }
        command.value = expression->stringValue;
        return true;
    }
    if (command.name == "play_gesture") {
        const JsonValue* gesture = field(payload, "gesture");
        const JsonValue* intensity = field(payload, "intensity");
        if (!exactFields(payload, {"name", "gesture", "intensity"}) || gesture == nullptr || gesture->type != JsonType::string
            || !inSet(gesture->stringValue, {"still", "nod", "shake", "look_left", "look_right", "look_up", "look_down", "nuzzle"})
            || intensity == nullptr || intensity->type != JsonType::number || intensity->numberValue < 0.0 || intensity->numberValue > 1.0) {
            return false;
        }
        command.value = gesture->stringValue;
        command.intensity = intensity->numberValue;
        return true;
    }
    if (command.name == "set_idle_mode") {
        const JsonValue* mode = field(payload, "mode");
        if (!exactFields(payload, {"name", "mode"}) || mode == nullptr || mode->type != JsonType::string
            || !inSet(mode->stringValue, {"on", "off"})) {
            return false;
        }
        command.value = mode->stringValue;
        return true;
    }
    return false;
}

}  // namespace

bool isValidUtf8(std::string_view value)
{
    std::size_t index = 0;
    while (index < value.size()) {
        const auto first = static_cast<unsigned char>(value[index]);
        if (first <= 0x7F) {
            ++index;
            continue;
        }

        std::size_t continuationCount = 0;
        std::uint32_t codePoint = 0;
        if (first >= 0xC2 && first <= 0xDF) {
            continuationCount = 1;
            codePoint = first & 0x1F;
        } else if (first >= 0xE0 && first <= 0xEF) {
            continuationCount = 2;
            codePoint = first & 0x0F;
        } else if (first >= 0xF0 && first <= 0xF4) {
            continuationCount = 3;
            codePoint = first & 0x07;
        } else {
            return false;
        }
        if (index + continuationCount >= value.size()) {
            return false;
        }
        for (std::size_t offset = 1; offset <= continuationCount; ++offset) {
            const auto continuation = static_cast<unsigned char>(value[index + offset]);
            if ((continuation & 0xC0) != 0x80) {
                return false;
            }
            codePoint = (codePoint << 6) | (continuation & 0x3F);
        }
        if ((continuationCount == 2 && codePoint < 0x800)
            || (continuationCount == 3 && codePoint < 0x10000)
            || (codePoint >= 0xD800 && codePoint <= 0xDFFF) || codePoint > 0x10FFFF) {
            return false;
        }
        index += continuationCount + 1;
    }
    return true;
}

CommandValidationResult validateCommandJson(std::string_view frame, std::int64_t receivedAtMs)
{
    CommandValidationResult result;
    JsonValue root;
    JsonParser parser(frame);
    if (!parser.parse(root) || !exactFields(root, {"protocol", "type", "id", "issued_at_ms", "expires_at_ms", "command"})) {
        return result;
    }

    const JsonValue* protocol = field(root, "protocol");
    const JsonValue* identifier = field(root, "id");
    const JsonValue* issuedAt = field(root, "issued_at_ms");
    const JsonValue* expiresAt = field(root, "expires_at_ms");
    const JsonValue* payload = field(root, "command");
    if (protocol == nullptr || protocol->type != JsonType::number || !protocol->isInteger || protocol->integerValue != kProtocolVersion
        || !stringEquals(field(root, "type"), "command") || identifier == nullptr || identifier->type != JsonType::string
        || !isCanonicalUuid4(identifier->stringValue) || issuedAt == nullptr || expiresAt == nullptr
        || issuedAt->type != JsonType::number || expiresAt->type != JsonType::number || !issuedAt->isInteger || !expiresAt->isInteger
        || issuedAt->integerValue < 0 || expiresAt->integerValue < 0 || receivedAtMs < 0 || payload == nullptr) {
        return result;
    }

    const std::int64_t issued = issuedAt->integerValue;
    const std::int64_t expires = expiresAt->integerValue;
    if (expires <= issued || expires - issued > kMaxCommandTtlMs
        || (issued > receivedAtMs && issued - receivedAtMs > kMaxCommandFutureSkewMs)) {
        return result;
    }
    result.command.id = identifier->stringValue;
    result.command.issuedAtMs = issued;
    result.command.expiresAtMs = expires;
    if (!validatePayload(*payload, result.command)) {
        return result;
    }
    result.status = receivedAtMs > expires ? CommandValidationStatus::expired : CommandValidationStatus::accepted;
    return result;
}

bool validateWelcomeJson(std::string_view frame, WelcomeMessage& result)
{
    JsonValue root;
    JsonParser parser(frame);
    if (!parser.parse(root)
        || !exactFields(root, {"protocol", "type", "server_time_ms", "heartbeat_interval_ms", "heartbeat_miss_limit"})) {
        return false;
    }
    const JsonValue* protocol = field(root, "protocol");
    const JsonValue* serverTime = field(root, "server_time_ms");
    const JsonValue* heartbeatInterval = field(root, "heartbeat_interval_ms");
    const JsonValue* heartbeatMissLimit = field(root, "heartbeat_miss_limit");
    if (protocol == nullptr || protocol->type != JsonType::number || !protocol->isInteger
        || protocol->integerValue != kProtocolVersion || !stringEquals(field(root, "type"), "welcome")
        || serverTime == nullptr || serverTime->type != JsonType::number || !serverTime->isInteger
        || serverTime->integerValue < 0 || heartbeatInterval == nullptr
        || heartbeatInterval->type != JsonType::number || !heartbeatInterval->isInteger
        || heartbeatInterval->integerValue != static_cast<std::int64_t>(kHeartbeatIntervalMs)
        || heartbeatMissLimit == nullptr || heartbeatMissLimit->type != JsonType::number
        || !heartbeatMissLimit->isInteger
        || heartbeatMissLimit->integerValue != static_cast<std::int64_t>(kHeartbeatMissLimit)) {
        return false;
    }
    result.serverTimeMs = serverTime->integerValue;
    result.heartbeatIntervalMs = static_cast<std::uint64_t>(heartbeatInterval->integerValue);
    result.heartbeatMissLimit = static_cast<std::uint32_t>(heartbeatMissLimit->integerValue);
    return true;
}

bool validateHeartbeatAckJson(std::string_view frame, HeartbeatAcknowledgement& result)
{
    JsonValue root;
    JsonParser parser(frame);
    if (!parser.parse(root) || !exactFields(root, {"protocol", "type", "seq", "server_time_ms"})) {
        return false;
    }
    const JsonValue* protocol = field(root, "protocol");
    const JsonValue* sequence = field(root, "seq");
    const JsonValue* serverTime = field(root, "server_time_ms");
    if (protocol == nullptr || protocol->type != JsonType::number || !protocol->isInteger
        || protocol->integerValue != kProtocolVersion || !stringEquals(field(root, "type"), "heartbeat_ack")
        || sequence == nullptr || sequence->type != JsonType::number || !sequence->isInteger
        || sequence->integerValue < 0 || sequence->integerValue > 2147483647
        || serverTime == nullptr || serverTime->type != JsonType::number || !serverTime->isInteger
        || serverTime->integerValue < 0) {
        return false;
    }
    result.sequence = static_cast<std::int32_t>(sequence->integerValue);
    result.serverTimeMs = serverTime->integerValue;
    return true;
}

bool RecentCommandIds::seen(std::string_view commandId) const
{
    return std::any_of(ids_.begin(), ids_.begin() + static_cast<std::ptrdiff_t>(count_),
        [commandId](const std::string& value) { return value == commandId; });
}

bool RecentCommandIds::remember(std::string_view commandId)
{
    if (seen(commandId)) {
        return false;
    }
    ids_[next_] = commandId;
    next_ = (next_ + 1) % ids_.size();
    if (count_ < ids_.size()) {
        ++count_;
    }
    return true;
}

bool EventRateLimiter::allow(std::uint64_t nowMs)
{
    if (hasLastNow_ && nowMs < lastNowMs_) {
        return false;
    }
    hasLastNow_ = true;
    lastNowMs_ = nowMs;

    while (count_ > 0 && nowMs - events_[head_] >= kEventRateWindowMs) {
        head_ = (head_ + 1) % events_.size();
        --count_;
    }
    if (count_ == events_.size()) {
        return false;
    }
    const std::size_t tail = (head_ + count_) % events_.size();
    events_[tail] = nowMs;
    ++count_;
    return true;
}

}  // namespace stackchan::companion
