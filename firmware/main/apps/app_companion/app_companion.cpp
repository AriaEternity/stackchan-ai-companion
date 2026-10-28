/*
 * SPDX-License-Identifier: MIT
 */
#include "app_companion.h"

#include <apps/common/common.h>
#include <assets/assets.h>
#include <companion/command_scheduler.h>
#include <companion/companion_client.h>
#include <companion/companion_config.h>
#include <companion/provisioning_frame.h>
#include <companion/screen_layout_policy.h>
#include <esp_app_desc.h>
#include <esp_timer.h>
#include <hal/hal.h>
#include <mooncake_log.h>
#include <smooth_lvgl.hpp>
#include <stackchan/stackchan.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <fcntl.h>
#include <memory>
#include <string>
#include <unistd.h>

using namespace mooncake;
using namespace smooth_ui_toolkit::lvgl_cpp;
using namespace stackchan;
using stackchan::companion::CompanionConfigStore;
using stackchan::companion::CompanionClient;
using stackchan::companion::CompanionCredentials;
using stackchan::companion::CompanionExpression;
using stackchan::companion::CommandActuator;
using stackchan::companion::ConnectionState;
using stackchan::companion::MotionTarget;
using stackchan::companion::ProvisioningFeedResult;
using stackchan::companion::ProvisioningFrameParser;
using stackchan::companion::TouchGesture;
using stackchan::companion::connectionStateName;
using stackchan::companion::screenTextDeadlineReached;
using stackchan::companion::shouldUseFullScreenText;

extern "C" {
LV_FONT_DECLARE(font_puhui_16_4);
}

namespace {

std::unique_ptr<Label> titleLabel;
std::unique_ptr<Label> versionLabel;
std::unique_ptr<Label> stateLabel;
std::unique_ptr<Label> safetyLabel;
std::unique_ptr<Button> backButton;
std::unique_ptr<CompanionClient> companionClient;
ConnectionState displayedState = ConnectionState::unconfigured;
std::atomic<bool> localTouchFeedback{false};
std::atomic<bool> screenDismissRequested{false};
bool remoteIdleEnabled = true;
std::uint32_t screenTextDeadlineMs = 0;
ProvisioningFrameParser provisioningParser;
bool localProvisioningActive = false;
int originalStdinFlags = -1;
unsigned int rejectedProvisioningFrames = 0;
int64_t lastProvisioningReadyUs = 0;

constexpr std::string_view kProvisioningReady = "SC-PROVISION-READY\n";
constexpr std::string_view kProvisioningOk = "SC-PROVISION-OK\n";
constexpr std::string_view kProvisioningError = "SC-PROVISION-ERROR\n";
constexpr std::string_view kProvisioningLocked = "SC-PROVISION-LOCKED\n";
constexpr unsigned int kProvisioningAttemptLimit = 5;
constexpr int64_t kProvisioningReadyIntervalUs = 2'000'000;
constexpr std::uint32_t kNavigationButtonColor = 0xFFFFFF;
constexpr std::uint32_t kNavigationBackgroundColor = 0x000000;

void writeProvisioningStatus(std::string_view status)
{
    const char* current = status.data();
    std::size_t remaining = status.size();
    while (remaining > 0) {
        const ssize_t written = write(STDOUT_FILENO, current, remaining);
        if (written <= 0) {
            return;
        }
        current += written;
        remaining -= static_cast<std::size_t>(written);
    }
}

void stopLocalProvisioning()
{
    if (originalStdinFlags >= 0) {
        (void)fcntl(STDIN_FILENO, F_SETFL, originalStdinFlags);
    }
    originalStdinFlags = -1;
    localProvisioningActive = false;
    rejectedProvisioningFrames = 0;
    lastProvisioningReadyUs = 0;
    provisioningParser.reset();
}

void startLocalProvisioning(const CompanionCredentials& credentials)
{
    stopLocalProvisioning();
    if (credentials.configured()) {
        return;
    }
    originalStdinFlags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (originalStdinFlags < 0
        || fcntl(STDIN_FILENO, F_SETFL, originalStdinFlags | O_NONBLOCK) < 0) {
        originalStdinFlags = -1;
        return;
    }
    localProvisioningActive = true;
}

bool pollLocalProvisioning(CompanionCredentials& credentials)
{
    if (!localProvisioningActive) {
        return false;
    }

    const int64_t nowUs = esp_timer_get_time();
    if (lastProvisioningReadyUs == 0
        || nowUs - lastProvisioningReadyUs >= kProvisioningReadyIntervalUs) {
        writeProvisioningStatus(kProvisioningReady);
        lastProvisioningReadyUs = nowUs;
    }

    std::array<char, 64> input{};
    while (true) {
        const ssize_t received = read(STDIN_FILENO, input.data(), input.size());
        if (received <= 0) {
            break;
        }
        for (ssize_t index = 0; index < received; ++index) {
            const auto result = provisioningParser.feed(input[static_cast<std::size_t>(index)]);
            if (result == ProvisioningFeedResult::accepted) {
                credentials = provisioningParser.takeCredentials();
                std::fill(input.begin(), input.end(), '\0');
                return true;
            }
            if (result == ProvisioningFeedResult::rejected) {
                rejectedProvisioningFrames++;
                writeProvisioningStatus(kProvisioningError);
                if (rejectedProvisioningFrames >= kProvisioningAttemptLimit) {
                    writeProvisioningStatus(kProvisioningLocked);
                    stopLocalProvisioning();
                    std::fill(input.begin(), input.end(), '\0');
                    return false;
                }
            }
        }
        std::fill(input.begin(), input.end(), '\0');
    }
    return false;
}

void setConversationChrome(bool showingReply)
{
    for (auto* object : {stateLabel ? stateLabel->get() : nullptr,
             safetyLabel ? safetyLabel->get() : nullptr}) {
        if (object == nullptr) {
            continue;
        }
        if (showingReply) {
            lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void setConversationLayout(bool textOnly)
{
    if (!GetStackChan().hasAvatar()) {
        return;
    }

    const auto& elements = GetStackChan().avatar().getKeyElements();
    for (auto* feature : {elements.leftEye.get(), elements.rightEye.get(), elements.mouth.get()}) {
        if (feature != nullptr) {
            feature->setVisible(!textOnly);
        }
    }
    if (elements.speechBubble) {
        elements.speechBubble->setLongTextMode(textOnly);
    }
}

void restoreFaceLayout()
{
    screenTextDeadlineMs = 0;
    if (!GetStackChan().hasAvatar()) {
        return;
    }
    GetStackChan().avatar().clearSpeech();
    setConversationLayout(false);
    setConversationChrome(true);
}

class StackChanCommandActuator final : public CommandActuator {
public:
    bool applyExpression(CompanionExpression expression) override
    {
        if (!GetStackChan().hasAvatar()) {
            return false;
        }
        avatar::Emotion mapped = avatar::Emotion::Neutral;
        switch (expression) {
        case CompanionExpression::neutral: mapped = avatar::Emotion::Neutral; break;
        case CompanionExpression::happy: mapped = avatar::Emotion::Happy; break;
        case CompanionExpression::shy: mapped = avatar::Emotion::Sad; break;
        case CompanionExpression::sad: mapped = avatar::Emotion::Sad; break;
        case CompanionExpression::angry: mapped = avatar::Emotion::Angry; break;
        case CompanionExpression::sleepy: mapped = avatar::Emotion::Sleepy; break;
        case CompanionExpression::surprised: mapped = avatar::Emotion::Doubt; break;
        }
        restoreFaceLayout();
        GetStackChan().avatar().setEmotion(mapped);
        return true;
    }

    bool showText(std::string_view text) override
    {
        if (!GetStackChan().hasAvatar()) {
            return false;
        }
        if (shouldUseFullScreenText(text)) {
            setConversationLayout(true);
            GetStackChan().avatar().setSpeech(text);
            screenTextDeadlineMs = GetHAL().millis()
                + stackchan::companion::kFullScreenTextDurationMs;
        } else {
            restoreFaceLayout();
        }
        setConversationChrome(true);
        return true;
    }

    bool setIdleEnabled(bool enabled) override
    {
        // D7 has no calibrated autonomous servo-idle motion. Keep the local
        // blink/breath safety feedback and only record the remote idle policy.
        remoteIdleEnabled = enabled;
        if (safetyLabel) {
            safetyLabel->setText(remoteIdleEnabled
                    ? "Safety: MOTION LOCKED / MEDIA OFF"
                    : "Safety: IDLE OFF / MOTION LOCKED");
        }
        return true;
    }

    bool moveHead(const MotionTarget& target) override
    {
        // The scheduler has already validated the persisted project profile;
        // targets are in the BSP's bounded yaw/pitch angle domains.
        // Auto release deliberately disables torque at rest, so every remote
        // target must re-enable it before issuing a new position command.
        GetStackChan().motion().setTorqueEnabled(true);
        GetStackChan().motion().setAutoTorqueReleaseEnabled(true);
        GetStackChan().motion().moveWithSpeed(target.yaw, target.pitch, target.speed);
        return true;
    }

    bool motionSettled() override
    {
        return !GetStackChan().motion().isMoving();
    }

    void stopAndRelease() override
    {
        GetStackChan().motion().stop();
        GetStackChan().motion().setAutoTorqueReleaseEnabled(true);
    }
};

StackChanCommandActuator commandActuator;

bool isOfflineSafetyState(ConnectionState state)
{
    return state == ConnectionState::unconfigured || state == ConnectionState::wifiConnecting
        || state == ConnectionState::degraded || state == ConnectionState::offline;
}

}  // namespace

AppCompanion::AppCompanion()
{
    setAppInfo().name = "COMPANION";
    static auto icon = assets::get_image("icon_ai_agent.bin");
    setAppInfo().icon = static_cast<void*>(&icon);
    static uint32_t themeColor = 0x8B5CF6;
    setAppInfo().userData = static_cast<void*>(&themeColor);
}

void AppCompanion::onCreate()
{
    mclog::tagInfo(getAppInfo().name, "on create; media services disabled");
}

void AppCompanion::onOpen()
{
    // The custom launcher can open Companion without running the original
    // setup worker, so ensure the shared Wi-Fi station is started first.
    GetHAL().startNetwork([](std::string_view) {});

    const auto credentials = CompanionConfigStore().load();
    mclog::tagInfo(getAppInfo().name, "on open; credentials configured: {}; media services disabled",
        credentials.configured());

    if (!companionClient) {
        companionClient = std::make_unique<CompanionClient>();
    }
    companionClient->start(credentials);
    displayedState = companionClient->state();
    startLocalProvisioning(credentials);

    LvglLockGuard lock;
    auto* screen = lv_screen_active();

    auto avatar = std::make_unique<avatar::DefaultAvatar>();
    avatar->init(screen, &font_puhui_16_4);
    avatar->getPanel()->onClick().connect([]() { screenDismissRequested.store(true); });
    GetStackChan().attachAvatar(std::move(avatar));
    setConversationLayout(false);
    GetStackChan().clearModifiers();
    GetStackChan().addModifier(std::make_unique<BreathModifier>());
    GetStackChan().addModifier(std::make_unique<BlinkModifier>());
    GetStackChan().motion().stop();
    GetStackChan().motion().setAutoTorqueReleaseEnabled(true);
    remoteIdleEnabled = true;

    GetHAL().onHeadPetGesture.connect([](HeadPetGesture gesture) {
        TouchGesture companionGesture = TouchGesture::none;
        switch (gesture) {
        case HeadPetGesture::Press: companionGesture = TouchGesture::press; break;
        case HeadPetGesture::Release: companionGesture = TouchGesture::release; break;
        case HeadPetGesture::SwipeForward: companionGesture = TouchGesture::swipeLowToHigh; break;
        case HeadPetGesture::SwipeBackward: companionGesture = TouchGesture::swipeHighToLow; break;
        case HeadPetGesture::None: break;
        }
        if (gesture == HeadPetGesture::Press || gesture == HeadPetGesture::SwipeForward
            || gesture == HeadPetGesture::SwipeBackward) {
            localTouchFeedback.store(true);
        }
        if (companionClient && companionGesture != TouchGesture::none) {
            companionClient->submitTouchGesture(companionGesture);
        }
    });

    titleLabel = std::make_unique<Label>(screen);
    titleLabel->setText("StackChan Companion");
    titleLabel->setAlign(LV_ALIGN_CENTER);
    titleLabel->setPos(0, -104);
    titleLabel->setHidden(true);

    versionLabel = std::make_unique<Label>(screen);
    versionLabel->setText(std::string("Version: ") + esp_app_get_description()->version);
    versionLabel->setAlign(LV_ALIGN_CENTER);
    versionLabel->setPos(0, -82);
    versionLabel->setHidden(true);

    stateLabel = std::make_unique<Label>(screen);
    stateLabel->setText(std::string("State: ") + connectionStateName(displayedState));
    stateLabel->setAlign(LV_ALIGN_CENTER);
    stateLabel->setPos(0, 70);
    stateLabel->setHidden(true);

    safetyLabel = std::make_unique<Label>(screen);
    safetyLabel->setText("Safety: MOTION LOCKED / MEDIA OFF");
    safetyLabel->setAlign(LV_ALIGN_CENTER);
    safetyLabel->setPos(0, 92);
    safetyLabel->setHidden(true);

    backButton = std::make_unique<Button>(screen);
    backButton->setAlign(LV_ALIGN_TOP_RIGHT);
    backButton->setPos(-4, 4);
    backButton->setSize(64, 30);
    backButton->label().setText("Back");
    backButton->setHidden(true);
    backButton->onClick().connect([this]() { close(); });

    view::create_home_indicator([this]() { close(); }, kNavigationButtonColor,
        kNavigationBackgroundColor);
    view::create_status_bar(kNavigationBackgroundColor, kNavigationButtonColor);
}

void AppCompanion::onRunning()
{
    GetHAL().feedTheDog();
    if (!companionClient || !stateLabel) {
        return;
    }
    CompanionCredentials provisionedCredentials;
    if (pollLocalProvisioning(provisionedCredentials)) {
        CompanionConfigStore store;
        const bool saved = store.save(provisionedCredentials);
        const auto verified = store.load();
        if (saved && verified.deviceId == provisionedCredentials.deviceId
            && verified.deviceToken == provisionedCredentials.deviceToken) {
            companionClient->stop();
            companionClient->start(verified);
            writeProvisioningStatus(kProvisioningOk);
            stopLocalProvisioning();
        } else {
            writeProvisioningStatus(kProvisioningError);
        }
        std::fill(provisionedCredentials.deviceId.begin(), provisionedCredentials.deviceId.end(), '\0');
        std::fill(provisionedCredentials.deviceToken.begin(), provisionedCredentials.deviceToken.end(), '\0');
    }
    const auto currentState = companionClient->state();
    LvglLockGuard lock;
    companionClient->updateCommands(commandActuator);
    if (currentState != displayedState) {
        const bool lostRemoteConnection = (displayedState == ConnectionState::online
                || displayedState == ConnectionState::degraded)
            && currentState != ConnectionState::online
            && currentState != ConnectionState::degraded;
        const bool enteringOfflineSafety = isOfflineSafetyState(currentState)
            && !isOfflineSafetyState(displayedState);
        displayedState = currentState;
        stateLabel->setText(std::string("State: ") + connectionStateName(displayedState));
        if (lostRemoteConnection && GetStackChan().hasAvatar()) {
            GetStackChan().avatar().clearSpeech();
            setConversationLayout(false);
            setConversationChrome(false);
        }
        if (enteringOfflineSafety) {
            GetStackChan().motion().stop();
            GetStackChan().motion().setAutoTorqueReleaseEnabled(true);
        }
    }
    if (localTouchFeedback.exchange(false) && GetStackChan().hasAvatar()) {
        restoreFaceLayout();
        GetStackChan().addModifier(std::make_unique<TimedEmotionModifier>(avatar::Emotion::Happy, 1200));
    }
    if ((screenDismissRequested.exchange(false)
            || screenTextDeadlineReached(GetHAL().millis(), screenTextDeadlineMs))
        && GetStackChan().hasAvatar()) {
        restoreFaceLayout();
    }
    GetStackChan().update();
    view::update_home_indicator();
    view::update_status_bar();
}

void AppCompanion::onClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");
    if (companionClient) {
        companionClient->stop();
    }
    stopLocalProvisioning();
    LvglLockGuard lock;
    GetHAL().onHeadPetGesture.clear();
    localTouchFeedback.store(false);
    screenDismissRequested.store(false);
    screenTextDeadlineMs = 0;
    GetStackChan().motion().stop();
    GetStackChan().motion().setAutoTorqueReleaseEnabled(true);
    if (GetStackChan().hasAvatar()) {
        GetStackChan().avatar().clearSpeech();
        setConversationLayout(false);
    }
    GetStackChan().clearModifiers();
    GetStackChan().resetAvatar();
    view::destroy_home_indicator();
    view::destroy_status_bar();
    backButton.reset();
    safetyLabel.reset();
    stateLabel.reset();
    versionLabel.reset();
    titleLabel.reset();
}
