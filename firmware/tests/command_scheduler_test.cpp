#include "companion/command_scheduler.h"
#include "companion/motion_safety_plan.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace stackchan::companion;

namespace {

class FakeActuator final : public CommandActuator {
public:
    bool applyExpression(CompanionExpression expression) override
    {
        expressions.push_back(expression);
        return allowOperations;
    }

    bool showText(std::string_view text) override
    {
        displayedText.emplace_back(text);
        return allowOperations;
    }

    bool setIdleEnabled(bool enabled) override
    {
        idleStates.push_back(enabled);
        return allowOperations;
    }

    bool moveHead(const MotionTarget& target) override
    {
        targets.push_back(target);
        return allowOperations;
    }

    bool motionSettled() override
    {
        return settled;
    }

    void stopAndRelease() override
    {
        ++stopCount;
    }

    bool allowOperations = true;
    bool settled = true;
    int stopCount = 0;
    std::vector<CompanionExpression> expressions;
    std::vector<std::string> displayedText;
    std::vector<bool> idleStates;
    std::vector<MotionTarget> targets;
};

std::string commandId(unsigned value)
{
    char suffix[13]{};
    std::snprintf(suffix, sizeof(suffix), "%012x", value);
    return std::string("10000000-0000-4000-8000-") + suffix;
}

ProtocolCommand makeCommand(unsigned id, std::string name, std::string value,
    double intensity = 0.0)
{
    ProtocolCommand command;
    command.id = commandId(id);
    command.name = std::move(name);
    command.value = std::move(value);
    command.intensity = intensity;
    return command;
}

std::vector<SchedulerAck> drain(CommandScheduler& scheduler)
{
    std::vector<SchedulerAck> result;
    SchedulerAck acknowledgement;
    while (scheduler.popAcknowledgement(acknowledgement)) {
        result.push_back(std::move(acknowledgement));
    }
    return result;
}

bool hasStatus(const std::vector<SchedulerAck>& acknowledgements,
    SchedulerAckStatus status, std::string_view id = {})
{
    for (const auto& acknowledgement : acknowledgements) {
        if (acknowledgement.status == status
            && (id.empty() || acknowledgement.commandId == id)) {
            return true;
        }
    }
    return false;
}

ProjectMotionSafety enabledSafety()
{
    ProjectMotionSafety safety;
    safety.calibrated = true;
    safety.yawCenter = 20;
    safety.pitchCenter = 450;
    safety.yawAmplitude = 120;
    safety.pitchAmplitude = 80;
    safety.yawLimitMinimum = -100;
    safety.yawLimitMaximum = 140;
    safety.pitchLimitMinimum = 370;
    safety.pitchLimitMaximum = 530;
    safety.maximumSpeed = 350;
    safety.minimumCooldownMs = 0;
    safety.maximumGesturesPerMinute = 12;
    return safety;
}

void finishScheduler(CommandScheduler& scheduler, FakeActuator& actuator,
    std::uint64_t startMs, std::uint64_t endMs)
{
    for (std::uint64_t nowMs = startMs; nowMs <= endMs; nowMs += 20) {
        scheduler.tick(nowMs, actuator);
        if (!scheduler.hasActiveCommand() && scheduler.queuedCount() == 0) {
            return;
        }
    }
}

}  // namespace

int main()
{
    CommandScheduler display;
    FakeActuator displayActuator;
    const auto displayId = commandId(800);
    display.submit(makeCommand(800, "show_text", "neko is here."), 0, 1000);
    assert(hasStatus(drain(display), SchedulerAckStatus::accepted, displayId));
    display.tick(0, displayActuator);
    assert(hasStatus(drain(display), SchedulerAckStatus::completed, displayId));
    assert(displayActuator.displayedText == std::vector<std::string>{"neko is here."});

    constexpr std::array<std::string_view, 7> expressions = {
        "neutral", "happy", "shy", "sad", "angry", "sleepy", "surprised"};
    for (std::size_t index = 0; index < expressions.size(); ++index) {
        CompanionExpression parsed;
        assert(parseCompanionExpression(expressions[index], parsed));
        CommandScheduler scheduler;
        FakeActuator actuator;
        const auto id = commandId(static_cast<unsigned>(index + 1));
        scheduler.submit(makeCommand(static_cast<unsigned>(index + 1),
                             "set_expression", std::string(expressions[index])),
            100, 1000);
        assert(hasStatus(drain(scheduler), SchedulerAckStatus::accepted, id));
        scheduler.tick(100, actuator);
        assert(hasStatus(drain(scheduler), SchedulerAckStatus::completed, id));
        assert(actuator.expressions.size() == 1);
    }
    CompanionExpression invalidExpression;
    assert(!parseCompanionExpression("unknown", invalidExpression));

    constexpr std::array<std::string_view, 8> gestures = {
        "still", "nod", "shake", "look_left", "look_right", "look_up", "look_down", "nuzzle"};
    const ProjectMotionSafety safety = enabledSafety();
    assert(validProjectMotionSafety(safety));

    const ProjectMotionSafety conservative = makeConservativeMotionSafetyProfile(1, 1);
    assert(conservative.maximumSpeed == kMotionSafetyTestSpeed);
    GesturePlan visibleMinimum;
    assert(buildGesturePlan(CompanionGesture::lookLeft, 0.2, conservative, visibleMinimum));
    assert(visibleMinimum.count == 2);
    assert(visibleMinimum.targets[0].yaw == 60);
    assert(visibleMinimum.targets[0].pitch == kProjectHomePitch);
    assert(visibleMinimum.targets[0].speed == kMotionSafetyTestSpeed);
    assert(visibleMinimum.targets[0].holdMs == 900);
    assert(visibleMinimum.targets[1].yaw == kProjectHomeYaw);
    assert(visibleMinimum.targets[1].pitch == kProjectHomePitch);

    GesturePlan strictButtonPlan;
    assert(buildGesturePlan(CompanionGesture::lookRight, 1.0, conservative, strictButtonPlan));
    assert(strictButtonPlan.count == 2);
    assert(strictButtonPlan.targets[0].yaw == -60);
    assert(strictButtonPlan.targets[0].pitch == 30);
    assert(buildGesturePlan(CompanionGesture::lookUp, 1.0, conservative, strictButtonPlan));
    assert(strictButtonPlan.count == 2);
    assert(strictButtonPlan.targets[0].yaw == 0);
    assert(strictButtonPlan.targets[0].pitch == 110);
    assert(buildGesturePlan(CompanionGesture::lookDown, 1.0, conservative, strictButtonPlan));
    assert(strictButtonPlan.count == 3);
    assert(strictButtonPlan.targets[0].pitch == 70);
    assert(strictButtonPlan.targets[0].holdMs == 100);
    assert(strictButtonPlan.targets[1].pitch == 30);
    assert(strictButtonPlan.targets[1].holdMs == 900);
    assert(buildGesturePlan(CompanionGesture::shake, 1.0, conservative, strictButtonPlan));
    assert(strictButtonPlan.count == 3);
    assert(strictButtonPlan.targets[0].yaw == 60);
    assert(strictButtonPlan.targets[1].yaw == -60);
    assert(buildGesturePlan(CompanionGesture::nod, 1.0, conservative, strictButtonPlan));
    assert(strictButtonPlan.count == 3);
    assert(strictButtonPlan.targets[0].pitch == 110);
    assert(strictButtonPlan.targets[1].pitch == 30);

    ProjectMotionSafety excessive = safety;
    excessive.yawAmplitude = kProjectMaximumYawAmplitude + 1;
    assert(!validProjectMotionSafety(excessive));
    ProjectMotionSafety reversed = safety;
    reversed.yawDirection = -1;
    reversed.pitchDirection = -1;
    assert(validProjectMotionSafety(reversed));
    GesturePlan reversedPlan;
    assert(buildGesturePlan(CompanionGesture::lookRight, 1.0, reversed, reversedPlan));
    assert(reversedPlan.targets.front().yaw > reversed.yawCenter);
    ProjectMotionSafety invalidDirection = safety;
    invalidDirection.pitchDirection = 0;
    assert(!validProjectMotionSafety(invalidDirection));
    ProjectMotionSafety narrowLimits = safety;
    narrowLimits.yawLimitMinimum = -40;
    narrowLimits.yawLimitMaximum = 40;
    narrowLimits.yawAmplitude = 41;
    assert(!validProjectMotionSafety(narrowLimits));
    ProjectMotionSafety outsideHardLimits = safety;
    outsideHardLimits.pitchLimitMinimum = kProjectPitchLimitMinimum - 1;
    assert(!validProjectMotionSafety(outsideHardLimits));

    std::mt19937 random(0xD7001234U);
    std::uniform_real_distribution<double> intensityDistribution(0.0, 1.0);
    for (std::size_t gestureIndex = 0; gestureIndex < gestures.size(); ++gestureIndex) {
        const auto gestureName = gestures[gestureIndex];
        CompanionGesture gesture;
        assert(parseCompanionGesture(gestureName, gesture));
        for (int iteration = 0; iteration < 1000; ++iteration) {
            const double intensity = iteration == 0 ? 0.0 : intensityDistribution(random);
            GesturePlan plan;
            assert(buildGesturePlan(gesture, intensity, safety, plan));
            CommandScheduler scheduler(safety);
            FakeActuator fakeServo;
            const auto idNumber = static_cast<unsigned>(10000 + gestureIndex * 1000 + iteration);
            const auto id = commandId(idNumber);
            scheduler.submit(makeCommand(idNumber, "play_gesture",
                                 std::string(gestureName), intensity),
                0, 10000);
            assert(hasStatus(drain(scheduler), SchedulerAckStatus::accepted, id));
            finishScheduler(scheduler, fakeServo, 0, 5000);
            assert(hasStatus(drain(scheduler), SchedulerAckStatus::completed, id));
            if (gesture == CompanionGesture::still || intensity == 0.0) {
                assert(plan.count == 0);
                assert(fakeServo.targets.empty());
                continue;
            }
            assert(plan.count > 0 && plan.count <= kGestureKeyframeCapacity);
            assert(fakeServo.targets.size() == plan.count);
            for (const auto& target : fakeServo.targets) {
                assert(target.yaw >= safety.yawCenter - safety.yawAmplitude);
                assert(target.yaw <= safety.yawCenter + safety.yawAmplitude);
                assert(target.pitch >= safety.pitchCenter - safety.pitchAmplitude);
                assert(target.pitch <= safety.pitchCenter + safety.pitchAmplitude);
                assert(target.speed > 0 && target.speed <= safety.maximumSpeed);
                assert(target.holdMs > 0);
            }
            const auto& home = fakeServo.targets.back();
            assert(home.yaw == std::clamp(kProjectHomeYaw,
                safety.yawLimitMinimum, safety.yawLimitMaximum));
            assert(home.pitch == std::clamp(kProjectHomePitch,
                safety.pitchLimitMinimum, safety.pitchLimitMaximum));
        }
    }
    CompanionGesture invalidGesture;
    assert(!parseCompanionGesture("spin", invalidGesture));

    for (std::size_t index = 0; index < gestures.size(); ++index) {
        CommandScheduler scheduler(safety);
        FakeActuator actuator;
        const auto id = commandId(static_cast<unsigned>(100 + index));
        scheduler.submit(makeCommand(static_cast<unsigned>(100 + index), "play_gesture",
                             std::string(gestures[index]), 0.7),
            0, 10000);
        assert(hasStatus(drain(scheduler), SchedulerAckStatus::accepted, id));
        finishScheduler(scheduler, actuator, 0, 5000);
        assert(hasStatus(drain(scheduler), SchedulerAckStatus::completed, id));
        if (gestures[index] == "still") {
            assert(actuator.targets.empty());
            assert(actuator.stopCount > 0);
        } else {
            assert(!actuator.targets.empty());
        }
    }

    CommandScheduler deviceLocked;
    FakeActuator lockedActuator;
    const auto lockedId = commandId(200);
    deviceLocked.submit(makeCommand(200, "play_gesture", "nod", 1.0), 0, 10000);
    const auto lockedAck = drain(deviceLocked);
    assert(hasStatus(lockedAck, SchedulerAckStatus::rejected, lockedId));
    assert(lockedAck.front().reason == SchedulerRejectReason::unsafe);
    deviceLocked.tick(0, lockedActuator);
    assert(lockedActuator.targets.empty());

    const auto zeroId = commandId(201);
    deviceLocked.submit(makeCommand(201, "play_gesture", "nod", 0.0), 20, 10000);
    assert(hasStatus(drain(deviceLocked), SchedulerAckStatus::accepted, zeroId));
    deviceLocked.tick(20, lockedActuator);
    assert(hasStatus(drain(deviceLocked), SchedulerAckStatus::completed, zeroId));
    assert(lockedActuator.targets.empty());

    CommandScheduler bounded;
    for (unsigned index = 0; index < kSchedulerQueueCapacity; ++index) {
        bounded.submit(makeCommand(300 + index, "set_expression", "happy"), 0, 10000);
        assert(hasStatus(drain(bounded), SchedulerAckStatus::accepted));
    }
    assert(bounded.queuedCount() == kSchedulerQueueCapacity);
    const auto busyId = commandId(399);
    bounded.submit(makeCommand(399, "set_expression", "happy"), 0, 10000);
    assert(hasStatus(drain(bounded), SchedulerAckStatus::busy, busyId));
    const auto duplicateId = commandId(300);
    bounded.submit(makeCommand(300, "set_expression", "happy"), 0, 10000);
    assert(hasStatus(drain(bounded), SchedulerAckStatus::accepted, duplicateId));
    assert(bounded.queuedCount() == kSchedulerQueueCapacity);

    CommandScheduler activeBounded(safety);
    FakeActuator waitingActuator;
    activeBounded.submit(makeCommand(320, "play_gesture", "nuzzle", 1.0), 0, 10000);
    drain(activeBounded);
    activeBounded.tick(0, waitingActuator);
    assert(activeBounded.hasActiveCommand());
    for (unsigned index = 0; index < kSchedulerQueueCapacity - 1; ++index) {
        activeBounded.submit(makeCommand(321 + index, "set_expression", "happy"), 1, 10000);
    }
    drain(activeBounded);
    assert(activeBounded.hasActiveCommand());
    assert(activeBounded.queuedCount() == kSchedulerQueueCapacity - 1);
    const auto activeBusyId = commandId(3990);
    activeBounded.submit(makeCommand(3990, "set_expression", "happy"), 2, 10000);
    assert(hasStatus(drain(activeBounded), SchedulerAckStatus::busy, activeBusyId));
    assert(activeBounded.queuedCount() == kSchedulerQueueCapacity - 1);

    CommandScheduler expiry;
    const auto expiredId = commandId(400);
    expiry.submit(makeCommand(400, "set_expression", "happy"), 100, 99);
    assert(hasStatus(drain(expiry), SchedulerAckStatus::expired, expiredId));
    const auto unknownId = commandId(401);
    expiry.submit(makeCommand(401, "unknown", "value"), 100, 1000);
    const auto unknownAck = drain(expiry);
    assert(hasStatus(unknownAck, SchedulerAckStatus::rejected, unknownId));
    assert(unknownAck.front().reason == SchedulerRejectReason::unsupported);

    ProjectMotionSafety cooldownSafety = safety;
    cooldownSafety.minimumCooldownMs = 1000;
    CommandScheduler cooldown(cooldownSafety);
    cooldown.submit(makeCommand(500, "play_gesture", "nod", 0.5), 0, 10000);
    assert(hasStatus(drain(cooldown), SchedulerAckStatus::accepted));
    cooldown.submit(makeCommand(501, "play_gesture", "shake", 0.5), 500, 10000);
    assert(hasStatus(drain(cooldown), SchedulerAckStatus::busy, commandId(501)));

    ProjectMotionSafety budgetSafety = safety;
    budgetSafety.maximumGesturesPerMinute = 2;
    CommandScheduler budget(budgetSafety);
    budget.submit(makeCommand(510, "play_gesture", "nod", 0.5), 0, 10000);
    budget.submit(makeCommand(511, "play_gesture", "shake", 0.5), 1000, 10000);
    budget.submit(makeCommand(512, "play_gesture", "look_up", 0.5), 2000, 10000);
    const auto budgetAcks = drain(budget);
    assert(hasStatus(budgetAcks, SchedulerAckStatus::accepted, commandId(510)));
    assert(hasStatus(budgetAcks, SchedulerAckStatus::accepted, commandId(511)));
    assert(hasStatus(budgetAcks, SchedulerAckStatus::busy, commandId(512)));

    CommandScheduler interrupted(safety);
    FakeActuator interruptedActuator;
    const auto interruptedId = commandId(600);
    interrupted.submit(makeCommand(600, "play_gesture", "nuzzle", 1.0), 0, 10000);
    drain(interrupted);
    interrupted.tick(0, interruptedActuator);
    assert(!interruptedActuator.targets.empty());
    interrupted.cancelAll(false);
    interrupted.tick(1, interruptedActuator);
    assert(interruptedActuator.stopCount > 0);
    assert(!interrupted.hasActiveCommand() && interrupted.queuedCount() == 0);
    assert(drain(interrupted).empty());
    interrupted.submit(makeCommand(600, "play_gesture", "nuzzle", 1.0), 2, 10000);
    assert(hasStatus(drain(interrupted), SchedulerAckStatus::completed, interruptedId));
    assert(interrupted.queuedCount() == 0);

    CommandScheduler timedOut(safety);
    FakeActuator timedOutActuator;
    timedOutActuator.settled = false;
    const auto timedOutId = commandId(610);
    timedOut.submit(makeCommand(610, "play_gesture", "nod", 1.0), 0, 30000);
    drain(timedOut);
    timedOut.tick(0, timedOutActuator);
    timedOut.tick(12701, timedOutActuator);
    const auto timeoutAcks = drain(timedOut);
    assert(hasStatus(timeoutAcks, SchedulerAckStatus::rejected, timedOutId));
    assert(timedOutActuator.stopCount > 0);

    CommandScheduler delayedSettling(conservative);
    FakeActuator delayedSettlingActuator;
    delayedSettlingActuator.settled = false;
    const auto delayedSettlingId = commandId(611);
    delayedSettling.submit(makeCommand(611, "play_gesture", "look_left", 0.2), 0, 10000);
    assert(hasStatus(drain(delayedSettling), SchedulerAckStatus::accepted, delayedSettlingId));
    delayedSettling.tick(0, delayedSettlingActuator);
    assert(delayedSettlingActuator.targets.size() == 1);
    assert(delayedSettlingActuator.targets.front().yaw == 60);
    assert(delayedSettlingActuator.targets.front().pitch == kProjectHomePitch);
    delayedSettling.tick(699, delayedSettlingActuator);
    assert(delayedSettlingActuator.targets.size() == 1);
    delayedSettling.tick(3000, delayedSettlingActuator);
    assert(delayedSettlingActuator.targets.size() == 1);
    delayedSettlingActuator.settled = true;
    delayedSettling.tick(3001, delayedSettlingActuator);
    delayedSettling.tick(3900, delayedSettlingActuator);
    assert(delayedSettlingActuator.targets.size() == 1);
    delayedSettling.tick(3901, delayedSettlingActuator);
    assert(delayedSettlingActuator.targets.size() == 1);
    delayedSettling.tick(3902, delayedSettlingActuator);
    assert(delayedSettlingActuator.targets.size() == 2);
    assert(delayedSettlingActuator.targets.back().yaw == kProjectHomeYaw);
    assert(delayedSettlingActuator.targets.back().pitch == kProjectHomePitch);
    delayedSettlingActuator.settled = false;
    delayedSettling.tick(6000, delayedSettlingActuator);
    assert(delayedSettling.hasActiveCommand());
    assert(drain(delayedSettling).empty());
    delayedSettlingActuator.settled = true;
    delayedSettling.tick(6001, delayedSettlingActuator);
    delayedSettling.tick(6900, delayedSettlingActuator);
    assert(delayedSettling.hasActiveCommand());
    delayedSettling.tick(6901, delayedSettlingActuator);
    assert(hasStatus(drain(delayedSettling), SchedulerAckStatus::completed, delayedSettlingId));
    assert(delayedSettlingActuator.targets.size() == 2);

    CommandScheduler stillCancels(safety);
    FakeActuator stillActuator;
    const auto movingId = commandId(620);
    const auto stillId = commandId(621);
    stillCancels.submit(makeCommand(620, "play_gesture", "nod", 1.0), 0, 10000);
    drain(stillCancels);
    stillCancels.tick(0, stillActuator);
    stillCancels.submit(makeCommand(621, "play_gesture", "still", 1.0), 1, 10000);
    const auto cancellationAcks = drain(stillCancels);
    assert(hasStatus(cancellationAcks, SchedulerAckStatus::completed, movingId));
    assert(hasStatus(cancellationAcks, SchedulerAckStatus::accepted, stillId));
    stillCancels.tick(1, stillActuator);
    assert(hasStatus(drain(stillCancels), SchedulerAckStatus::completed, stillId));

    CommandScheduler idle;
    FakeActuator idleActuator;
    idle.submit(makeCommand(700, "set_idle_mode", "off"), 0, 1000);
    idle.submit(makeCommand(701, "set_idle_mode", "on"), 0, 1000);
    drain(idle);
    idle.tick(0, idleActuator);
    assert(idleActuator.idleStates.size() == 2);
    assert(!idleActuator.idleStates[0] && idleActuator.idleStates[1]);
    assert(hasStatus(drain(idle), SchedulerAckStatus::completed, commandId(700)));

    return 0;
}
