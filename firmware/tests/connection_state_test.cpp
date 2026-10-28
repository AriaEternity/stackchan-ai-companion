#include "companion/connection_state.h"

#include <cassert>
#include <string>
#include <vector>

using stackchan::companion::ConnectionAction;
using stackchan::companion::ConnectionActionType;
using stackchan::companion::ConnectionController;
using stackchan::companion::ConnectionFailure;
using stackchan::companion::ConnectionState;

namespace {

constexpr const char* kWelcome =
    "{\"protocol\":2,\"type\":\"welcome\",\"server_time_ms\":1700000000000,"
    "\"heartbeat_interval_ms\":15000,\"heartbeat_miss_limit\":3}";

std::vector<ConnectionAction> drain(ConnectionController& controller)
{
    std::vector<ConnectionAction> result;
    ConnectionAction action;
    while (controller.popAction(action)) {
        result.push_back(std::move(action));
    }
    return result;
}

bool hasAction(const std::vector<ConnectionAction>& actions, ConnectionActionType type)
{
    for (const auto& action : actions) {
        if (action.type == type) {
            return true;
        }
    }
    return false;
}

std::string firstPayload(const std::vector<ConnectionAction>& actions)
{
    for (const auto& action : actions) {
        if (action.type == ConnectionActionType::sendText) {
            return action.payload;
        }
    }
    return {};
}

void openFakeWss(ConnectionController& controller, std::uint64_t nowMs)
{
    const auto actions = drain(controller);
    assert(hasAction(actions, ConnectionActionType::connect));
    controller.onTransportConnected(nowMs);
    controller.onTextFrame(kWelcome, nowMs + 1);
    assert(controller.state() == ConnectionState::online);
    const auto heartbeat = firstPayload(drain(controller));
    assert(heartbeat.find("\"type\":\"heartbeat\"") != std::string::npos);
    assert(heartbeat.size() <= stackchan::companion::kMaxTextFrameBytes);
}

}  // namespace

int main()
{
    ConnectionController unconfigured(1);
    unconfigured.reset(false, true, 0);
    assert(unconfigured.state() == ConnectionState::unconfigured);
    assert(drain(unconfigured).empty());

    ConnectionController wifi(2);
    wifi.reset(true, false, 0);
    assert(wifi.state() == ConnectionState::wifiConnecting);
    wifi.onWifiChanged(true, 10);
    assert(wifi.state() == ConnectionState::serverConnecting);
    assert(hasAction(drain(wifi), ConnectionActionType::connect));
    wifi.onTransportConnected(11);
    wifi.onWifiChanged(false, 12);
    const auto wifiLost = drain(wifi);
    assert(wifi.state() == ConnectionState::wifiConnecting);
    assert(hasAction(wifiLost, ConnectionActionType::closeTransport));
    assert(hasAction(wifiLost, ConnectionActionType::clearRemoteCommands));
    assert(hasAction(wifiLost, ConnectionActionType::enterOfflineSafety));

    ConnectionController healthy(3);
    healthy.reset(true, true, 0);
    openFakeWss(healthy, 0);
    healthy.onTextFrame(
        "{\"protocol\":2,\"type\":\"heartbeat_ack\",\"seq\":0,\"server_time_ms\":1700000000001}", 2);
    assert(healthy.state() == ConnectionState::online);
    assert(drain(healthy).empty());

    const std::string validCommand =
        "{\"protocol\":2,\"type\":\"command\",\"id\":\"11111111-1111-4111-8111-111111111111\","
        "\"issued_at_ms\":1700000000000,\"expires_at_ms\":1700000010000,"
        "\"command\":{\"name\":\"set_expression\",\"expression\":\"happy\"}}";
    healthy.onTextFrame(validCommand, 3);
    const auto dispatched = drain(healthy);
    assert(hasAction(dispatched, ConnectionActionType::dispatchCommand));
    bool foundDispatch = false;
    for (const auto& action : dispatched) {
        if (action.type == ConnectionActionType::dispatchCommand) {
            foundDispatch = true;
            assert(action.command.name == "set_expression");
            assert(action.command.value == "happy");
            assert(action.command.expiresAtMs == 1700000010000);
            assert(action.commandDeadlineMs > 3);
        }
    }
    assert(foundDispatch);
    assert(healthy.queueDeviceEvent(
        "{\"protocol\":2,\"type\":\"touch_start\",\"zone\":\"top\"}"));
    const auto touchPayload = firstPayload(drain(healthy));
    assert(touchPayload.find("\"type\":\"touch_start\"") != std::string::npos);

    ConnectionController burst(31);
    burst.reset(true, true, 0);
    openFakeWss(burst, 0);
    for (unsigned index = 0; index < stackchan::companion::kPendingCommandActionCapacity + 1; ++index) {
        std::string frame = validCommand;
        frame.replace(frame.find("11111111-1111-4111-8111-111111111111"), 36,
            std::string("11111111-1111-4111-8111-11111111111") + std::to_string(index));
        burst.onTextFrame(frame, 3);
    }
    const auto burstActions = drain(burst);
    std::size_t dispatchedCount = 0;
    bool busyReturned = false;
    for (const auto& action : burstActions) {
        if (action.type == ConnectionActionType::dispatchCommand) {
            ++dispatchedCount;
        } else if (action.type == ConnectionActionType::sendText
            && action.payload.find("\"status\":\"busy\"") != std::string::npos) {
            busyReturned = true;
        }
    }
    assert(dispatchedCount == stackchan::companion::kPendingCommandActionCapacity);
    assert(busyReturned);

    ConnectionController halfOpen(4);
    halfOpen.reset(true, true, 0);
    openFakeWss(halfOpen, 0);
    halfOpen.tick(15001, 15001);
    assert(halfOpen.state() == ConnectionState::degraded);
    drain(halfOpen);
    halfOpen.tick(30001, 30001);
    drain(halfOpen);
    halfOpen.tick(45001, 45001);
    const auto halfOpenActions = drain(halfOpen);
    assert(halfOpen.state() == ConnectionState::offline);
    assert(hasAction(halfOpenActions, ConnectionActionType::closeTransport));
    assert(hasAction(halfOpenActions, ConnectionActionType::enterOfflineSafety));
    assert(halfOpen.lastRetryDelayMs() >= 1000 && halfOpen.lastRetryDelayMs() <= 1250);

    for (const auto failure : {ConnectionFailure::dns, ConnectionFailure::tls,
             ConnectionFailure::unavailable, ConnectionFailure::unauthorized, ConnectionFailure::forbidden}) {
        ConnectionController failed(5);
        failed.reset(true, true, 0);
        drain(failed);
        failed.onTransportFailure(failure, 100);
        const auto failureActions = drain(failed);
        assert(failed.state() == ConnectionState::offline);
        assert(hasAction(failureActions, ConnectionActionType::closeTransport));
        assert(hasAction(failureActions, ConnectionActionType::clearRemoteCommands));
        assert(failed.lastRetryDelayMs() <= stackchan::companion::kReconnectMaximumMs);
        if (failure == ConnectionFailure::unauthorized || failure == ConnectionFailure::forbidden) {
            assert(failed.lastRetryDelayMs() >= 30000);
        }
        failed.tick(failed.nextRetryAtMs(), failed.nextRetryAtMs());
        assert(failed.state() == ConnectionState::serverConnecting);
        assert(hasAction(drain(failed), ConnectionActionType::connect));
    }

    ConnectionController malformed(6);
    malformed.reset(true, true, 0);
    openFakeWss(malformed, 0);
    malformed.onTextFrame("{not-json", 2);
    assert(malformed.state() == ConnectionState::online);
    assert(drain(malformed).empty());

    ConnectionController oversized(7);
    oversized.reset(true, true, 0);
    openFakeWss(oversized, 0);
    oversized.onTextFrame(std::string(stackchan::companion::kMaxTextFrameBytes + 1, 'x'), 2);
    assert(oversized.state() == ConnectionState::offline);
    drain(oversized);

    ConnectionController binary(8);
    binary.reset(true, true, 0);
    openFakeWss(binary, 0);
    binary.onBinaryFrame(2);
    assert(binary.state() == ConnectionState::offline);
    drain(binary);

    ConnectionController serverClosed(9);
    serverClosed.reset(true, true, 0);
    openFakeWss(serverClosed, 0);
    serverClosed.onTransportClosed(2);
    assert(serverClosed.state() == ConnectionState::offline);
    drain(serverClosed);
    serverClosed.onTextFrame(validCommand, 3);
    assert(drain(serverClosed).empty());
    assert(!serverClosed.queueDeviceEvent(
        "{\"protocol\":2,\"type\":\"touch_start\",\"zone\":\"top\"}"));
    serverClosed.tick(serverClosed.nextRetryAtMs(), serverClosed.nextRetryAtMs());
    openFakeWss(serverClosed, serverClosed.nextRetryAtMs());

    return 0;
}
