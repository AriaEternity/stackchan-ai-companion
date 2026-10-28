/*
 * SPDX-License-Identifier: MIT
 */
#include "companion_client.h"
#include "motion_safety_store.h"

#include <algorithm>
#include <utility>

#include <boards/common/board.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <mooncake_log.h>
#include <wifi_manager.h>

namespace stackchan::companion {
namespace {

constexpr const char* kTag = "COMPANION.NET";

std::uint64_t monotonicMs()
{
    return static_cast<std::uint64_t>(esp_timer_get_time() / 1000);
}

ConnectionFailure mapFailure(SecureWebSocketConnectResult result)
{
    switch (result) {
    case SecureWebSocketConnectResult::dnsFailure: return ConnectionFailure::dns;
    case SecureWebSocketConnectResult::tlsFailure: return ConnectionFailure::tls;
    case SecureWebSocketConnectResult::unauthorized: return ConnectionFailure::unauthorized;
    case SecureWebSocketConnectResult::forbidden: return ConnectionFailure::forbidden;
    case SecureWebSocketConnectResult::protocolError: return ConnectionFailure::protocol;
    case SecureWebSocketConnectResult::unavailable:
    case SecureWebSocketConnectResult::connected:
        return ConnectionFailure::unavailable;
    }
    return ConnectionFailure::unavailable;
}

}  // namespace

CompanionClient::CompanionClient() : controller_(esp_random())
{
}

CompanionClient::~CompanionClient()
{
    stop();
}

bool CompanionClient::start(const CompanionCredentials& credentials)
{
    if (running_.load()) {
        return false;
    }
    credentials_ = credentials;
    // Setup may have changed the local profile while this client was stopped.
    // Reload at every start so a profile can never become stale across an
    // app close/open cycle. Invalid or absent NVS data remains locked.
    scheduler_.setMotionSafety(loadProjectMotionSafety());
    stopRequested_.store(false);
    inboundOverflow_.store(false);
    touchEvents_.setOnline(false);
    clearInbound();

    const bool configured = credentials_.configured();
    const bool wifiConnected = WifiManager::GetInstance().IsConnected();
    controller_.reset(configured, wifiConnected, monotonicMs());
    state_.store(controller_.state());
    if (!configured) {
        wipeCredentials();
        return true;
    }

    running_.store(true);
    const BaseType_t created = xTaskCreate(taskEntry, "companion_wss", 8192, this, 2, &task_);
    if (created != pdPASS) {
        running_.store(false);
        task_ = nullptr;
        state_.store(ConnectionState::offline);
        wipeCredentials();
        return false;
    }
    return true;
}

void CompanionClient::stop()
{
    stopRequested_.store(true);
    scheduler_.cancelAll(false);
    touchEvents_.setOnline(false);
    if (state_.load() != ConnectionState::unconfigured) {
        state_.store(ConnectionState::offline);
    }
}

void CompanionClient::updateCommands(CommandActuator& actuator)
{
    scheduler_.tick(monotonicMs(), actuator);
}

bool CompanionClient::submitTouchGesture(TouchGesture gesture)
{
    if (stopRequested_.load()) {
        return false;
    }
    return touchEvents_.submitGesture(gesture, monotonicMs());
}

void CompanionClient::taskEntry(void* context)
{
    static_cast<CompanionClient*>(context)->run();
}

void CompanionClient::run()
{
    bool lastWifiConnected = WifiManager::GetInstance().IsConnected();
    while (!stopRequested_.load()) {
        const std::uint64_t nowMs = monotonicMs();
        const bool wifiConnected = WifiManager::GetInstance().IsConnected();
        if (wifiConnected != lastWifiConnected) {
            controller_.onWifiChanged(wifiConnected, nowMs);
            lastWifiConnected = wifiConnected;
        }
        if (inboundOverflow_.exchange(false)) {
            clearInbound();
            controller_.onTransportFailure(ConnectionFailure::protocol, nowMs);
        } else {
            processInbound(nowMs);
        }
        controller_.tick(nowMs, nowMs);
        processActions(nowMs);
        const auto controllerState = controller_.state();
        touchEvents_.setOnline(controllerState == ConnectionState::online
            || controllerState == ConnectionState::degraded);
        processSchedulerAcknowledgements(nowMs);
        processTouchEvents(nowMs);
        state_.store(controller_.state());
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (websocket_) {
        websocket_->close();
        websocket_.reset();
    }
    clearInbound();
    touchEvents_.setOnline(false);
    wipeCredentials();
    task_ = nullptr;
    running_.store(false);
    vTaskDelete(nullptr);
}

void CompanionClient::connectTransport(std::uint64_t nowMs)
{
    if (stopRequested_.load()) {
        return;
    }
    if (websocket_) {
        websocket_->close();
        websocket_.reset();
    }
    auto* network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        controller_.onTransportFailure(ConnectionFailure::unavailable, nowMs);
        return;
    }

    websocket_ = std::make_unique<BoundedWebSocket>(network);
    websocket_->onData([this](const char* data, std::size_t length, bool binary) {
        enqueue(binary ? InboundType::binary : InboundType::text, data, length);
    });
    websocket_->onDisconnected([this]() { enqueue(InboundType::disconnected); });
    websocket_->onProtocolError([this]() { enqueue(InboundType::protocolError); });

    std::string authorization = makeBearerAuthorization(credentials_.deviceToken);
    const bool headersAccepted = websocket_->setHeader(std::string(kAuthorizationHeaderName), authorization)
        && websocket_->setHeader(std::string(kDeviceIdHeaderName), credentials_.deviceId)
        && websocket_->setHeader(std::string(kProtocolHeaderName), std::string(kProtocolHeaderValue));
    std::fill(authorization.begin(), authorization.end(), '\0');
    if (!headersAccepted) {
        controller_.onTransportFailure(ConnectionFailure::protocol, nowMs);
        websocket_.reset();
        return;
    }

    const auto result = websocket_->connect(std::string(kCompanionServerUrl));
    if (result == SecureWebSocketConnectResult::connected) {
        controller_.onTransportConnected(monotonicMs());
        mclog::tagInfo(kTag, "secure transport connected; waiting for protocol welcome");
    } else {
        controller_.onTransportFailure(mapFailure(result), monotonicMs());
        websocket_.reset();
        mclog::tagWarn(kTag, "secure transport unavailable; bounded retry scheduled");
    }
}

void CompanionClient::processActions(std::uint64_t nowMs)
{
    ConnectionAction action;
    while (!stopRequested_.load() && controller_.popAction(action)) {
        switch (action.type) {
        case ConnectionActionType::connect:
            connectTransport(nowMs);
            break;
        case ConnectionActionType::sendText:
            if (!websocket_ || !websocket_->sendText(action.payload)) {
                controller_.onTransportFailure(ConnectionFailure::unavailable, monotonicMs());
            }
            break;
        case ConnectionActionType::closeTransport:
            if (websocket_) {
                websocket_->close();
                websocket_.reset();
            }
            break;
        case ConnectionActionType::clearRemoteCommands:
            clearInbound();
            scheduler_.cancelAll(false);
            touchEvents_.setOnline(false);
            break;
        case ConnectionActionType::enterOfflineSafety:
            break;
        case ConnectionActionType::dispatchCommand:
            scheduler_.submit(std::move(action.command), nowMs, action.commandDeadlineMs);
            break;
        }
    }
}

void CompanionClient::processSchedulerAcknowledgements(std::uint64_t nowMs)
{
    SchedulerAck acknowledgement;
    while (scheduler_.popAcknowledgement(acknowledgement)) {
        if (controller_.state() != ConnectionState::online
            && controller_.state() != ConnectionState::degraded) {
            continue;
        }
        controller_.queueCommandAcknowledgement(acknowledgement.commandId,
            schedulerAckStatusName(acknowledgement.status),
            schedulerRejectReasonName(acknowledgement.reason));
        processActions(nowMs);
    }
}

void CompanionClient::processTouchEvents(std::uint64_t nowMs)
{
    TouchEvent event;
    while (touchEvents_.pop(event)) {
        if (!controller_.queueDeviceEvent(serializeTouchEvent(event))) {
            touchEvents_.setOnline(false);
            return;
        }
        // Drain one generated event at a time so a send failure can enqueue and
        // execute close/offline safety actions without competing with stale sends.
        processActions(nowMs);
        if (controller_.state() != ConnectionState::online
            && controller_.state() != ConnectionState::degraded) {
            touchEvents_.setOnline(false);
            return;
        }
    }
}

void CompanionClient::processInbound(std::uint64_t nowMs)
{
    for (;;) {
        InboundEvent event;
        {
            std::lock_guard<std::mutex> lock(inboundMutex_);
            if (inbound_.empty()) {
                return;
            }
            event = std::move(inbound_.front());
            inbound_.pop_front();
        }
        switch (event.type) {
        case InboundType::text:
            controller_.onTextFrame(event.payload, nowMs);
            break;
        case InboundType::binary:
        case InboundType::protocolError:
            controller_.onBinaryFrame(nowMs);
            break;
        case InboundType::disconnected:
            controller_.onTransportClosed(nowMs);
            break;
        }
    }
}

void CompanionClient::enqueue(InboundType type, const char* data, std::size_t length)
{
    if (stopRequested_.load()) {
        return;
    }
    if (length > kMaxTextFrameBytes || (length > 0 && data == nullptr)) {
        inboundOverflow_.store(true);
        return;
    }
    std::lock_guard<std::mutex> lock(inboundMutex_);
    if (inbound_.size() >= kInboundCapacity) {
        inboundOverflow_.store(true);
        return;
    }
    InboundEvent event;
    event.type = type;
    if (data != nullptr && length > 0 && type == InboundType::text) {
        event.payload.assign(data, length);
    }
    inbound_.push_back(std::move(event));
}

void CompanionClient::clearInbound()
{
    std::lock_guard<std::mutex> lock(inboundMutex_);
    for (auto& event : inbound_) {
        std::fill(event.payload.begin(), event.payload.end(), '\0');
    }
    inbound_.clear();
}

void CompanionClient::wipeCredentials()
{
    std::fill(credentials_.deviceId.begin(), credentials_.deviceId.end(), '\0');
    std::fill(credentials_.deviceToken.begin(), credentials_.deviceToken.end(), '\0');
    credentials_ = {};
}

}  // namespace stackchan::companion
