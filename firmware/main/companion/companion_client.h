/*
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "bounded_websocket.h"
#include "command_scheduler.h"
#include "companion_config.h"
#include "connection_state.h"
#include "touch_session.h"

#include <atomic>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace stackchan::companion {

class CompanionClient {
public:
    CompanionClient();
    ~CompanionClient();

    CompanionClient(const CompanionClient&) = delete;
    CompanionClient& operator=(const CompanionClient&) = delete;

    bool start(const CompanionCredentials& credentials);
    void stop();
    void updateCommands(CommandActuator& actuator);
    bool submitTouchGesture(TouchGesture gesture);
    ConnectionState state() const { return state_.load(); }
    bool running() const { return running_.load(); }

private:
    enum class InboundType {
        text,
        binary,
        disconnected,
        protocolError,
    };

    struct InboundEvent {
        InboundType type = InboundType::disconnected;
        std::string payload;
    };

    static void taskEntry(void* context);
    void run();
    void connectTransport(std::uint64_t nowMs);
    void processActions(std::uint64_t nowMs);
    void processSchedulerAcknowledgements(std::uint64_t nowMs);
    void processTouchEvents(std::uint64_t nowMs);
    void processInbound(std::uint64_t nowMs);
    void enqueue(InboundType type, const char* data = nullptr, std::size_t length = 0);
    void clearInbound();
    void wipeCredentials();

    CompanionCredentials credentials_;
    ConnectionController controller_;
    CommandScheduler scheduler_;
    TouchEventQueue touchEvents_;
    std::unique_ptr<BoundedWebSocket> websocket_;
    std::deque<InboundEvent> inbound_;
    std::mutex inboundMutex_;
    std::atomic<ConnectionState> state_{ConnectionState::unconfigured};
    std::atomic<bool> running_{false};
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> inboundOverflow_{false};
    TaskHandle_t task_ = nullptr;

    static constexpr std::size_t kInboundCapacity = 4;
};

}  // namespace stackchan::companion
