/*
 * SPDX-License-Identifier: MIT
 */
#include "command_scheduler.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace stackchan::companion {
namespace {

constexpr std::uint64_t kGestureBudgetWindowMs = 60000;
// A positive named gesture must be large enough to observe during D11. The
// gesture-specific fractions and persisted profile remain the hard bounds.
constexpr double kMinimumNonzeroGestureScale = 0.75;
// Each keyframe must physically settle before its visible hold begins. Keep a
// bounded allowance per target so stalled motion still terminates safely.
constexpr std::uint32_t kExecutionTimeoutMarginMs = 1000;
constexpr std::uint32_t kTargetSettleAllowanceMs = 3000;
constexpr std::uint32_t kPreparationHoldMs = 100;
constexpr std::uint32_t kVisibleGestureHoldMs = 900;

int boundedOffset(int center, int amplitude, int direction, int limitMinimum,
    int limitMaximum, double fraction, double scale)
{
    const double raw = static_cast<double>(center)
        + static_cast<double>(direction) * static_cast<double>(amplitude) * fraction * scale;
    const int rounded = static_cast<int>(std::lround(raw));
    return std::clamp(rounded, limitMinimum, limitMaximum);
}

int gestureSpeed(double intensity, int gestureMaximum, const ProjectMotionSafety& safety)
{
    const int scaled = static_cast<int>(std::lround(120.0 + intensity * 280.0));
    return std::clamp(scaled, 1, std::min(gestureMaximum, safety.maximumSpeed));
}

bool appendAbsoluteTarget(GesturePlan& plan, const ProjectMotionSafety& safety,
    int yaw, int pitch, int speed, std::uint32_t holdMs)
{
    if (plan.count >= plan.targets.size()) {
        return false;
    }
    plan.targets[plan.count++] = {
        std::clamp(yaw, safety.yawLimitMinimum, safety.yawLimitMaximum),
        std::clamp(pitch, safety.pitchLimitMinimum, safety.pitchLimitMaximum),
        std::clamp(speed, 1, safety.maximumSpeed),
        holdMs,
    };
    return true;
}

bool appendHomeTarget(GesturePlan& plan, const ProjectMotionSafety& safety,
    int speed, std::uint32_t holdMs)
{
    return appendAbsoluteTarget(plan, safety, kProjectHomeYaw, kProjectHomePitch, speed, holdMs);
}

bool safeAdd(std::uint64_t left, std::uint64_t right, std::uint64_t& result)
{
    if (left > std::numeric_limits<std::uint64_t>::max() - right) {
        result = std::numeric_limits<std::uint64_t>::max();
        return false;
    }
    result = left + right;
    return true;
}

}  // namespace

bool parseCompanionExpression(std::string_view value, CompanionExpression& result)
{
    if (value == "neutral") result = CompanionExpression::neutral;
    else if (value == "happy") result = CompanionExpression::happy;
    else if (value == "shy") result = CompanionExpression::shy;
    else if (value == "sad") result = CompanionExpression::sad;
    else if (value == "angry") result = CompanionExpression::angry;
    else if (value == "sleepy") result = CompanionExpression::sleepy;
    else if (value == "surprised") result = CompanionExpression::surprised;
    else return false;
    return true;
}

bool parseCompanionGesture(std::string_view value, CompanionGesture& result)
{
    if (value == "still") result = CompanionGesture::still;
    else if (value == "nod") result = CompanionGesture::nod;
    else if (value == "shake") result = CompanionGesture::shake;
    else if (value == "look_left") result = CompanionGesture::lookLeft;
    else if (value == "look_right") result = CompanionGesture::lookRight;
    else if (value == "look_up") result = CompanionGesture::lookUp;
    else if (value == "look_down") result = CompanionGesture::lookDown;
    else if (value == "nuzzle") result = CompanionGesture::nuzzle;
    else return false;
    return true;
}

bool validProjectMotionSafety(const ProjectMotionSafety& safety)
{
    if (!safety.calibrated || safety.yawAmplitude <= 0 || safety.pitchAmplitude <= 0
        || safety.yawAmplitude > kProjectMaximumYawAmplitude
        || safety.pitchAmplitude > kProjectMaximumPitchAmplitude
        || (safety.yawDirection != -1 && safety.yawDirection != 1)
        || (safety.pitchDirection != -1 && safety.pitchDirection != 1)
        || safety.maximumSpeed <= 0 || safety.maximumSpeed > kProjectMaximumSpeed
        || safety.maximumGesturesPerMinute == 0
        || safety.maximumGesturesPerMinute > kGestureBudgetCapacity) {
        return false;
    }
    const auto fits = [](int center, int amplitude, int limitMinimum, int limitMaximum,
                         int hardMinimum, int hardMaximum) {
        if (limitMinimum < hardMinimum || limitMaximum > hardMaximum || limitMinimum >= limitMaximum) {
            return false;
        }
        if (center < limitMinimum || center > limitMaximum) {
            return false;
        }
        return amplitude <= center - limitMinimum && amplitude <= limitMaximum - center;
    };
    return fits(safety.yawCenter, safety.yawAmplitude, safety.yawLimitMinimum,
             safety.yawLimitMaximum, kProjectYawLimitMinimum, kProjectYawLimitMaximum)
        && fits(safety.pitchCenter, safety.pitchAmplitude, safety.pitchLimitMinimum,
             safety.pitchLimitMaximum, kProjectPitchLimitMinimum, kProjectPitchLimitMaximum);
}

bool buildGesturePlan(CompanionGesture gesture, double intensity,
    const ProjectMotionSafety& safety, GesturePlan& result)
{
    result = {};
    if (!std::isfinite(intensity) || intensity < 0.0 || intensity > 1.0) {
        return false;
    }
    if (gesture == CompanionGesture::still || intensity == 0.0) {
        result.timeoutMs = kExecutionTimeoutMarginMs;
        return true;
    }
    if (!validProjectMotionSafety(safety)) {
        return false;
    }

    const double scale = kMinimumNonzeroGestureScale
        + intensity * (1.0 - kMinimumNonzeroGestureScale);
    int speed = gestureSpeed(intensity, 360, safety);
    bool ok = true;
    switch (gesture) {
    case CompanionGesture::still:
        break;
    case CompanionGesture::nod:
        speed = gestureSpeed(intensity, 320, safety);
        ok = appendAbsoluteTarget(result, safety, kProjectHomeYaw,
                boundedOffset(safety.pitchCenter, safety.pitchAmplitude, safety.pitchDirection,
                    safety.pitchLimitMinimum, safety.pitchLimitMaximum, 1.0, scale),
                speed, kVisibleGestureHoldMs)
            && appendAbsoluteTarget(result, safety, kProjectHomeYaw,
                boundedOffset(safety.pitchCenter, safety.pitchAmplitude, safety.pitchDirection,
                    safety.pitchLimitMinimum, safety.pitchLimitMaximum, -1.0, scale),
                speed, kVisibleGestureHoldMs)
            && appendHomeTarget(result, safety, speed, kVisibleGestureHoldMs);
        break;
    case CompanionGesture::shake:
        speed = gestureSpeed(intensity, 340, safety);
        ok = appendAbsoluteTarget(result, safety,
                boundedOffset(safety.yawCenter, safety.yawAmplitude, safety.yawDirection,
                    safety.yawLimitMinimum, safety.yawLimitMaximum, 1.0, scale),
                kProjectHomePitch, speed, kVisibleGestureHoldMs)
            && appendAbsoluteTarget(result, safety,
                boundedOffset(safety.yawCenter, safety.yawAmplitude, safety.yawDirection,
                    safety.yawLimitMinimum, safety.yawLimitMaximum, -1.0, scale),
                kProjectHomePitch, speed, kVisibleGestureHoldMs)
            && appendHomeTarget(result, safety, speed, kVisibleGestureHoldMs);
        break;
    case CompanionGesture::lookLeft:
        ok = appendAbsoluteTarget(result, safety,
                boundedOffset(safety.yawCenter, safety.yawAmplitude, safety.yawDirection,
                    safety.yawLimitMinimum, safety.yawLimitMaximum, 1.0, 1.0),
                kProjectHomePitch, speed, kVisibleGestureHoldMs)
            && appendHomeTarget(result, safety, speed, kVisibleGestureHoldMs);
        break;
    case CompanionGesture::lookRight:
        ok = appendAbsoluteTarget(result, safety,
                boundedOffset(safety.yawCenter, safety.yawAmplitude, safety.yawDirection,
                    safety.yawLimitMinimum, safety.yawLimitMaximum, -1.0, 1.0),
                kProjectHomePitch, speed, kVisibleGestureHoldMs)
            && appendHomeTarget(result, safety, speed, kVisibleGestureHoldMs);
        break;
    case CompanionGesture::lookUp:
        ok = appendAbsoluteTarget(result, safety, kProjectHomeYaw,
                boundedOffset(safety.pitchCenter, safety.pitchAmplitude, safety.pitchDirection,
                    safety.pitchLimitMinimum, safety.pitchLimitMaximum, 1.0, scale),
                speed, kVisibleGestureHoldMs)
            && appendHomeTarget(result, safety, speed, kVisibleGestureHoldMs);
        break;
    case CompanionGesture::lookDown:
        // Home is the calibrated lower pitch bound. Move to the neutral center
        // without a visible hold, then make the downward stroke the held pose.
        ok = appendAbsoluteTarget(result, safety, kProjectHomeYaw,
                safety.pitchCenter, speed, kPreparationHoldMs)
            && appendAbsoluteTarget(result, safety, kProjectHomeYaw,
                boundedOffset(safety.pitchCenter, safety.pitchAmplitude, safety.pitchDirection,
                    safety.pitchLimitMinimum, safety.pitchLimitMaximum, -1.0, scale),
                speed, kVisibleGestureHoldMs)
            && appendHomeTarget(result, safety, speed, kVisibleGestureHoldMs);
        break;
    case CompanionGesture::nuzzle:
        speed = gestureSpeed(intensity, 280, safety);
        ok = appendAbsoluteTarget(result, safety,
                boundedOffset(safety.yawCenter, safety.yawAmplitude, safety.yawDirection,
                    safety.yawLimitMinimum, safety.yawLimitMaximum, -0.75, scale),
                boundedOffset(safety.pitchCenter, safety.pitchAmplitude, safety.pitchDirection,
                    safety.pitchLimitMinimum, safety.pitchLimitMaximum, 0.50, scale),
                speed, kVisibleGestureHoldMs)
            && appendAbsoluteTarget(result, safety,
                boundedOffset(safety.yawCenter, safety.yawAmplitude, safety.yawDirection,
                    safety.yawLimitMinimum, safety.yawLimitMaximum, 0.75, scale),
                safety.pitchCenter,
                speed, kVisibleGestureHoldMs)
            && appendAbsoluteTarget(result, safety,
                boundedOffset(safety.yawCenter, safety.yawAmplitude, safety.yawDirection,
                    safety.yawLimitMinimum, safety.yawLimitMaximum, -0.50, scale),
                boundedOffset(safety.pitchCenter, safety.pitchAmplitude, safety.pitchDirection,
                    safety.pitchLimitMinimum, safety.pitchLimitMaximum, 0.35, scale),
                speed, kVisibleGestureHoldMs)
            && appendHomeTarget(result, safety, speed, kVisibleGestureHoldMs);
        break;
    }
    if (!ok || result.count == 0) {
        result = {};
        return false;
    }
    std::uint64_t total = kExecutionTimeoutMarginMs;
    for (std::size_t index = 0; index < result.count; ++index) {
        total += kTargetSettleAllowanceMs + result.targets[index].holdMs;
    }
    result.timeoutMs = static_cast<std::uint32_t>(std::min<std::uint64_t>(total, kMaxCommandTtlMs));
    return true;
}

const char* schedulerAckStatusName(SchedulerAckStatus status)
{
    switch (status) {
    case SchedulerAckStatus::accepted: return "accepted";
    case SchedulerAckStatus::completed: return "completed";
    case SchedulerAckStatus::rejected: return "rejected";
    case SchedulerAckStatus::expired: return "expired";
    case SchedulerAckStatus::busy: return "busy";
    }
    return "rejected";
}

const char* schedulerRejectReasonName(SchedulerRejectReason reason)
{
    switch (reason) {
    case SchedulerRejectReason::none: return "";
    case SchedulerRejectReason::invalid: return "invalid";
    case SchedulerRejectReason::unsupported: return "unsupported";
    case SchedulerRejectReason::unsafe: return "unsafe";
    case SchedulerRejectReason::internal: return "internal";
    }
    return "internal";
}

CommandScheduler::CommandScheduler(ProjectMotionSafety safety) : safety_(safety)
{
}

void CommandScheduler::setMotionSafety(ProjectMotionSafety safety)
{
    std::lock_guard<std::mutex> lock(mutex_);
    // A profile change is only valid at a lifecycle boundary. Canceling the
    // old queue prevents a target admitted under one profile from running
    // after the profile has changed.
    cancelAllLocked(false);
    safety_ = validProjectMotionSafety(safety) ? safety : ProjectMotionSafety{};
}

void CommandScheduler::submit(ProtocolCommand command, std::uint64_t nowMs,
    std::uint64_t deadlineMs)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (command.id.empty()) {
        return;
    }
    if (const Receipt* receipt = findReceipt(command.id)) {
        pushAcknowledgement({command.id, receipt->status, receipt->reason});
        return;
    }
    if (deadlineMs < nowMs) {
        rememberReceipt(command.id, SchedulerAckStatus::expired);
        pushAcknowledgement({command.id, SchedulerAckStatus::expired});
        return;
    }

    ScheduledCommand scheduled;
    scheduled.command = std::move(command);
    scheduled.deadlineMs = deadlineMs;
    const std::string commandId = scheduled.command.id;
    if (!classify(scheduled.command, scheduled)) {
        rememberReceipt(scheduled.command.id, SchedulerAckStatus::rejected,
            SchedulerRejectReason::unsupported);
        pushAcknowledgement({scheduled.command.id, SchedulerAckStatus::rejected,
            SchedulerRejectReason::unsupported});
        return;
    }

    if (scheduled.kind == CommandKind::gesture) {
        if (scheduled.gesture == CompanionGesture::still) {
            cancelAllLocked(true);
        } else if (scheduled.command.intensity > 0.0) {
            GesturePlan plan;
            if (!buildGesturePlan(scheduled.gesture, scheduled.command.intensity, safety_, plan)) {
                rememberReceipt(scheduled.command.id, SchedulerAckStatus::rejected,
                    SchedulerRejectReason::unsafe);
                pushAcknowledgement({scheduled.command.id, SchedulerAckStatus::rejected,
                    SchedulerRejectReason::unsafe});
                return;
            }
        }
    }
    const std::size_t outstanding = queueCount_ + (hasActive_ ? 1U : 0U);
    if (outstanding >= kSchedulerQueueCapacity) {
        pushAcknowledgement({commandId, SchedulerAckStatus::busy});
        return;
    }
    if (scheduled.kind == CommandKind::gesture
        && scheduled.gesture != CompanionGesture::still
        && scheduled.command.intensity > 0.0 && !admitGestureBudget(nowMs)) {
        pushAcknowledgement({commandId, SchedulerAckStatus::busy});
        return;
    }
    if (!pushQueued(std::move(scheduled))) {
        pushAcknowledgement({commandId, SchedulerAckStatus::busy});
        return;
    }
    const auto& admitted = queue_[(queueHead_ + queueCount_ - 1) % queue_.size()];
    rememberReceipt(admitted.command.id, SchedulerAckStatus::accepted);
    pushAcknowledgement({admitted.command.id, SchedulerAckStatus::accepted});
}

void CommandScheduler::tick(std::uint64_t nowMs, CommandActuator& actuator)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopRequested_) {
        actuator.stopAndRelease();
        stopRequested_ = false;
    }

    for (std::size_t iteration = 0; iteration <= kSchedulerQueueCapacity; ++iteration) {
        if (!hasActive_) {
            startNext(nowMs);
            if (!hasActive_) {
                return;
            }
        }

        if (nowMs > active_.scheduled.deadlineMs) {
            if (active_.scheduled.kind == CommandKind::gesture) {
                actuator.stopAndRelease();
            }
            finishActive(SchedulerAckStatus::rejected, SchedulerRejectReason::internal);
            continue;
        }

        if (active_.scheduled.kind == CommandKind::displayText) {
            const bool applied = actuator.showText(active_.scheduled.command.value);
            finishActive(applied ? SchedulerAckStatus::completed : SchedulerAckStatus::rejected,
                applied ? SchedulerRejectReason::none : SchedulerRejectReason::internal);
            continue;
        }
        if (active_.scheduled.kind == CommandKind::expression) {
            const bool applied = actuator.applyExpression(active_.scheduled.expression);
            finishActive(applied ? SchedulerAckStatus::completed : SchedulerAckStatus::rejected,
                applied ? SchedulerRejectReason::none : SchedulerRejectReason::internal);
            continue;
        }
        if (active_.scheduled.kind == CommandKind::idle) {
            const bool applied = actuator.setIdleEnabled(active_.scheduled.idleEnabled);
            finishActive(applied ? SchedulerAckStatus::completed : SchedulerAckStatus::rejected,
                applied ? SchedulerRejectReason::none : SchedulerRejectReason::internal);
            continue;
        }

        if (active_.scheduled.gesture == CompanionGesture::still || active_.plan.count == 0) {
            actuator.stopAndRelease();
            finishActive(SchedulerAckStatus::completed);
            continue;
        }
        if (nowMs > active_.timeoutAtMs) {
            actuator.stopAndRelease();
            finishActive(SchedulerAckStatus::rejected, SchedulerRejectReason::internal);
            continue;
        }
        if (!active_.targetApplied) {
            const MotionTarget& target = active_.plan.targets[active_.nextTarget];
            if (!actuator.moveHead(target)) {
                actuator.stopAndRelease();
                finishActive(SchedulerAckStatus::rejected, SchedulerRejectReason::internal);
                continue;
            }
            active_.targetApplied = true;
            active_.targetSettled = false;
            active_.targetDueMs = 0;
            return;
        }
        const MotionTarget& target = active_.plan.targets[active_.nextTarget];
        if (!active_.targetSettled) {
            if (!actuator.motionSettled()) {
                return;
            }
            active_.targetSettled = true;
            safeAdd(nowMs, target.holdMs, active_.targetDueMs);
        }
        if (nowMs < active_.targetDueMs) {
            return;
        }
        if (active_.nextTarget + 1 >= active_.plan.count) {
            actuator.stopAndRelease();
            finishActive(SchedulerAckStatus::completed);
            continue;
        }
        ++active_.nextTarget;
        active_.targetApplied = false;
        active_.targetSettled = false;
        return;
    }
}

void CommandScheduler::cancelAll(bool emitTerminalAcknowledgements)
{
    std::lock_guard<std::mutex> lock(mutex_);
    cancelAllLocked(emitTerminalAcknowledgements);
}

bool CommandScheduler::popAcknowledgement(SchedulerAck& acknowledgement)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (acknowledgementCount_ == 0) {
        return false;
    }
    acknowledgement = std::move(acknowledgements_[acknowledgementHead_]);
    acknowledgements_[acknowledgementHead_] = {};
    acknowledgementHead_ = (acknowledgementHead_ + 1) % acknowledgements_.size();
    --acknowledgementCount_;
    return true;
}

std::size_t CommandScheduler::queuedCount() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return queueCount_;
}

bool CommandScheduler::hasActiveCommand() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return hasActive_;
}

bool CommandScheduler::classify(const ProtocolCommand& command, ScheduledCommand& result) const
{
    if (command.name == "show_text") {
        result.kind = CommandKind::displayText;
        return isValidDisplayText(command.value);
    }
    if (command.name == "set_expression") {
        result.kind = CommandKind::expression;
        return parseCompanionExpression(command.value, result.expression);
    }
    if (command.name == "play_gesture") {
        result.kind = CommandKind::gesture;
        return std::isfinite(command.intensity) && command.intensity >= 0.0
            && command.intensity <= 1.0 && parseCompanionGesture(command.value, result.gesture);
    }
    if (command.name == "set_idle_mode") {
        result.kind = CommandKind::idle;
        if (command.value == "on") {
            result.idleEnabled = true;
            return true;
        }
        if (command.value == "off") {
            result.idleEnabled = false;
            return true;
        }
    }
    return false;
}

bool CommandScheduler::pushQueued(ScheduledCommand command)
{
    if (queueCount_ >= queue_.size()) {
        return false;
    }
    const std::size_t tail = (queueHead_ + queueCount_) % queue_.size();
    queue_[tail] = std::move(command);
    ++queueCount_;
    return true;
}

bool CommandScheduler::popQueued(ScheduledCommand& command)
{
    if (queueCount_ == 0) {
        return false;
    }
    command = std::move(queue_[queueHead_]);
    queue_[queueHead_] = {};
    queueHead_ = (queueHead_ + 1) % queue_.size();
    --queueCount_;
    return true;
}

void CommandScheduler::startNext(std::uint64_t nowMs)
{
    ScheduledCommand next;
    while (popQueued(next)) {
        if (nowMs > next.deadlineMs) {
            rememberReceipt(next.command.id, SchedulerAckStatus::expired);
            pushAcknowledgement({next.command.id, SchedulerAckStatus::expired});
            continue;
        }
        active_ = {};
        active_.scheduled = std::move(next);
        hasActive_ = true;
        if (active_.scheduled.kind == CommandKind::gesture) {
            if (!buildGesturePlan(active_.scheduled.gesture,
                    active_.scheduled.command.intensity, safety_, active_.plan)) {
                finishActive(SchedulerAckStatus::rejected, SchedulerRejectReason::unsafe);
                continue;
            }
            std::uint64_t planTimeout = 0;
            safeAdd(nowMs, active_.plan.timeoutMs, planTimeout);
            active_.timeoutAtMs = std::min(planTimeout, active_.scheduled.deadlineMs);
        }
        return;
    }
}

void CommandScheduler::finishActive(SchedulerAckStatus status, SchedulerRejectReason reason)
{
    if (!hasActive_) {
        return;
    }
    const std::string commandId = active_.scheduled.command.id;
    rememberReceipt(commandId, status, reason);
    pushAcknowledgement({commandId, status, reason});
    active_ = {};
    hasActive_ = false;
}

void CommandScheduler::cancelAllLocked(bool emitTerminalAcknowledgements)
{
    if (!emitTerminalAcknowledgements) {
        acknowledgementHead_ = 0;
        acknowledgementCount_ = 0;
    }
    if (hasActive_) {
        const std::string id = active_.scheduled.command.id;
        rememberReceipt(id, SchedulerAckStatus::completed);
        if (emitTerminalAcknowledgements) {
            pushAcknowledgement({id, SchedulerAckStatus::completed});
        }
    }
    for (std::size_t index = 0; index < queueCount_; ++index) {
        const auto& item = queue_[(queueHead_ + index) % queue_.size()];
        rememberReceipt(item.command.id, SchedulerAckStatus::completed);
        if (emitTerminalAcknowledgements) {
            pushAcknowledgement({item.command.id, SchedulerAckStatus::completed});
        }
    }
    queue_ = {};
    queueHead_ = 0;
    queueCount_ = 0;
    active_ = {};
    hasActive_ = false;
    stopRequested_ = true;
}

bool CommandScheduler::admitGestureBudget(std::uint64_t nowMs)
{
    if (hasLastGestureAdmission_) {
        if (nowMs < lastGestureAdmissionMs_) {
            gestureAdmissionHead_ = 0;
            gestureAdmissionCount_ = 0;
            hasLastGestureAdmission_ = false;
        } else if (nowMs - lastGestureAdmissionMs_ < safety_.minimumCooldownMs) {
            return false;
        }
    }
    while (gestureAdmissionCount_ > 0) {
        const std::uint64_t oldest = gestureAdmissions_[gestureAdmissionHead_];
        if (nowMs >= oldest && nowMs - oldest >= kGestureBudgetWindowMs) {
            gestureAdmissionHead_ = (gestureAdmissionHead_ + 1) % gestureAdmissions_.size();
            --gestureAdmissionCount_;
        } else {
            break;
        }
    }
    if (gestureAdmissionCount_ >= safety_.maximumGesturesPerMinute) {
        return false;
    }
    const std::size_t tail = (gestureAdmissionHead_ + gestureAdmissionCount_)
        % gestureAdmissions_.size();
    gestureAdmissions_[tail] = nowMs;
    ++gestureAdmissionCount_;
    lastGestureAdmissionMs_ = nowMs;
    hasLastGestureAdmission_ = true;
    return true;
}

void CommandScheduler::pushAcknowledgement(SchedulerAck acknowledgement)
{
    if (acknowledgementCount_ == acknowledgements_.size()) {
        acknowledgements_[acknowledgementHead_] = {};
        acknowledgementHead_ = (acknowledgementHead_ + 1) % acknowledgements_.size();
        --acknowledgementCount_;
    }
    const std::size_t tail = (acknowledgementHead_ + acknowledgementCount_)
        % acknowledgements_.size();
    acknowledgements_[tail] = std::move(acknowledgement);
    ++acknowledgementCount_;
}

CommandScheduler::Receipt* CommandScheduler::findReceipt(std::string_view commandId)
{
    for (std::size_t index = 0; index < receiptCount_; ++index) {
        if (receipts_[index].commandId == commandId) {
            return &receipts_[index];
        }
    }
    return nullptr;
}

const CommandScheduler::Receipt* CommandScheduler::findReceipt(std::string_view commandId) const
{
    for (std::size_t index = 0; index < receiptCount_; ++index) {
        if (receipts_[index].commandId == commandId) {
            return &receipts_[index];
        }
    }
    return nullptr;
}

void CommandScheduler::rememberReceipt(std::string_view commandId, SchedulerAckStatus status,
    SchedulerRejectReason reason)
{
    if (Receipt* existing = findReceipt(commandId)) {
        existing->status = status;
        existing->reason = reason;
        return;
    }
    std::size_t index = 0;
    if (receiptCount_ < receipts_.size()) {
        index = receiptCount_++;
    } else {
        index = nextReceipt_;
        nextReceipt_ = (nextReceipt_ + 1) % receipts_.size();
    }
    receipts_[index] = {std::string(commandId), status, reason};
}

}  // namespace stackchan::companion
