/*
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "protocol_v2.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

namespace stackchan::companion {

constexpr std::size_t kSchedulerQueueCapacity = 4;
constexpr std::size_t kSchedulerAckCapacity = 16;
constexpr std::size_t kGestureKeyframeCapacity = 5;
constexpr std::size_t kGestureBudgetCapacity = 12;
constexpr int kProjectMaximumYawAmplitude = 300;
constexpr int kProjectMaximumPitchAmplitude = 200;
constexpr int kProjectMaximumSpeed = 500;
constexpr int kProjectConservativeMaximumSpeed = 120;
// MotionTarget values are passed directly to the BSP Servo API. Keep these
// hard bounds aligned with the original StackChan angle domains; the project
// profile may narrow them further but can never widen them.
constexpr int kProjectYawLimitMinimum = -1280;
constexpr int kProjectYawLimitMaximum = 1280;
// The pitch servo is not a signed axis in the original BSP. Its measured
// usable project range is 30..870 (the BSP clamps the wider 0..900 domain).
constexpr int kProjectPitchLimitMinimum = 30;
constexpr int kProjectPitchLimitMaximum = 870;
// The physical Home pose is the lower pitch bound. The persisted profile
// center remains at (0, 70) so the calibration test can exercise both pitch
// directions; named gestures use this separate pose as their physical rest target.
constexpr int kProjectHomeYaw = 0;
constexpr int kProjectHomePitch = kProjectPitchLimitMinimum;

enum class CompanionExpression {
    neutral,
    happy,
    shy,
    sad,
    angry,
    sleepy,
    surprised,
};

enum class CompanionGesture {
    still,
    nod,
    shake,
    lookLeft,
    lookRight,
    lookUp,
    lookDown,
    nuzzle,
};

enum class SchedulerAckStatus {
    accepted,
    completed,
    rejected,
    expired,
    busy,
};

enum class SchedulerRejectReason {
    none,
    invalid,
    unsupported,
    unsafe,
    internal,
};

struct SchedulerAck {
    std::string commandId;
    SchedulerAckStatus status = SchedulerAckStatus::rejected;
    SchedulerRejectReason reason = SchedulerRejectReason::none;
};

struct ProjectMotionSafety {
    bool calibrated = false;
    int yawCenter = 0;
    int pitchCenter = 0;
    int yawAmplitude = 0;
    int pitchAmplitude = 0;
    int yawDirection = 1;
    int pitchDirection = 1;
    int yawLimitMinimum = kProjectYawLimitMinimum;
    int yawLimitMaximum = kProjectYawLimitMaximum;
    int pitchLimitMinimum = kProjectPitchLimitMinimum;
    int pitchLimitMaximum = kProjectPitchLimitMaximum;
    int maximumSpeed = kProjectConservativeMaximumSpeed;
    std::uint64_t minimumCooldownMs = 1200;
    std::size_t maximumGesturesPerMinute = 12;
};

struct MotionTarget {
    int yaw = 0;
    int pitch = 0;
    int speed = 0;
    std::uint32_t holdMs = 0;
};

struct GesturePlan {
    std::array<MotionTarget, kGestureKeyframeCapacity> targets{};
    std::size_t count = 0;
    std::uint32_t timeoutMs = 0;
};

bool parseCompanionExpression(std::string_view value, CompanionExpression& result);
bool parseCompanionGesture(std::string_view value, CompanionGesture& result);
bool validProjectMotionSafety(const ProjectMotionSafety& safety);
bool buildGesturePlan(CompanionGesture gesture, double intensity,
    const ProjectMotionSafety& safety, GesturePlan& result);

const char* schedulerAckStatusName(SchedulerAckStatus status);
const char* schedulerRejectReasonName(SchedulerRejectReason reason);

class CommandActuator {
public:
    virtual ~CommandActuator() = default;
    virtual bool applyExpression(CompanionExpression expression) = 0;
    virtual bool showText(std::string_view text) = 0;
    virtual bool setIdleEnabled(bool enabled) = 0;
    virtual bool moveHead(const MotionTarget& target) = 0;
    virtual bool motionSettled() = 0;
    virtual void stopAndRelease() = 0;
};

class CommandScheduler {
public:
    explicit CommandScheduler(ProjectMotionSafety safety = {});

    // Replaces the safety profile at a lifecycle boundary. The queue is
    // cancelled first so a profile change cannot affect an in-flight target.
    void setMotionSafety(ProjectMotionSafety safety);

    void submit(ProtocolCommand command, std::uint64_t nowMs, std::uint64_t deadlineMs);
    void tick(std::uint64_t nowMs, CommandActuator& actuator);
    void cancelAll(bool emitTerminalAcknowledgements = false);
    bool popAcknowledgement(SchedulerAck& acknowledgement);

    std::size_t queuedCount() const;
    bool hasActiveCommand() const;
    const ProjectMotionSafety& motionSafety() const { return safety_; }

private:
    enum class CommandKind {
        expression,
        displayText,
        gesture,
        idle,
    };

    struct ScheduledCommand {
        ProtocolCommand command;
        CommandKind kind = CommandKind::expression;
        CompanionExpression expression = CompanionExpression::neutral;
        CompanionGesture gesture = CompanionGesture::still;
        bool idleEnabled = true;
        std::uint64_t deadlineMs = 0;
    };

    struct ActiveCommand {
        ScheduledCommand scheduled;
        GesturePlan plan;
        std::size_t nextTarget = 0;
        std::uint64_t targetDueMs = 0;
        std::uint64_t timeoutAtMs = 0;
        bool targetApplied = false;
        bool targetSettled = false;
    };

    struct Receipt {
        std::string commandId;
        SchedulerAckStatus status = SchedulerAckStatus::accepted;
        SchedulerRejectReason reason = SchedulerRejectReason::none;
    };

    bool classify(const ProtocolCommand& command, ScheduledCommand& result) const;
    bool pushQueued(ScheduledCommand command);
    bool popQueued(ScheduledCommand& command);
    void startNext(std::uint64_t nowMs);
    void finishActive(SchedulerAckStatus status,
        SchedulerRejectReason reason = SchedulerRejectReason::none);
    void cancelAllLocked(bool emitTerminalAcknowledgements);
    bool admitGestureBudget(std::uint64_t nowMs);
    void pushAcknowledgement(SchedulerAck acknowledgement);
    Receipt* findReceipt(std::string_view commandId);
    const Receipt* findReceipt(std::string_view commandId) const;
    void rememberReceipt(std::string_view commandId, SchedulerAckStatus status,
        SchedulerRejectReason reason = SchedulerRejectReason::none);

    ProjectMotionSafety safety_;
    mutable std::mutex mutex_;
    std::array<ScheduledCommand, kSchedulerQueueCapacity> queue_{};
    std::size_t queueHead_ = 0;
    std::size_t queueCount_ = 0;
    bool hasActive_ = false;
    ActiveCommand active_{};
    bool stopRequested_ = false;

    std::array<SchedulerAck, kSchedulerAckCapacity> acknowledgements_{};
    std::size_t acknowledgementHead_ = 0;
    std::size_t acknowledgementCount_ = 0;

    std::array<Receipt, kRecentCommandIdCapacity> receipts_{};
    std::size_t receiptCount_ = 0;
    std::size_t nextReceipt_ = 0;

    std::array<std::uint64_t, kGestureBudgetCapacity> gestureAdmissions_{};
    std::size_t gestureAdmissionHead_ = 0;
    std::size_t gestureAdmissionCount_ = 0;
    std::uint64_t lastGestureAdmissionMs_ = 0;
    bool hasLastGestureAdmission_ = false;
};

}  // namespace stackchan::companion
