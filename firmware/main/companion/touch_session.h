/*
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

namespace stackchan::companion {

constexpr std::size_t kTouchEventQueueCapacity = 8;
constexpr std::size_t kTouchEventRateLimit = 12;
constexpr std::size_t kTouchEventRateWindowCapacity = 20;

enum class TouchGesture {
    none,
    press,
    release,
    swipeLowToHigh,
    swipeHighToLow,
};

enum class TouchEventType {
    start,
    end,
    strokeLeftToRight,
    strokeRightToLeft,
};

struct TouchSample {
    std::array<std::uint8_t, 3> channels{};
    std::uint64_t timestampMs = 0;

    std::uint8_t maximumIntensity() const;
    int position() const;
};

struct TouchGestureConfig {
    std::uint8_t touchThreshold = 1;
    int swipeThreshold = 40;
    std::uint64_t pressDebounceMs = 50;
    std::uint64_t releaseDebounceMs = 50;
};

class TouchGestureRecognizer {
public:
    explicit TouchGestureRecognizer(TouchGestureConfig config = {});

    TouchGesture update(const TouchSample& sample);
    void reset();

private:
    enum class State {
        idle,
        pressDebounce,
        touched,
        swiped,
        releaseDebounce,
    };

    bool touched(const TouchSample& sample) const;

    TouchGestureConfig config_;
    State state_ = State::idle;
    State stateBeforeRelease_ = State::touched;
    std::uint64_t candidateSinceMs_ = 0;
    std::uint64_t lastTimestampMs_ = 0;
    bool hasTimestamp_ = false;
    int initialPosition_ = 0;
};

struct TouchEvent {
    TouchEventType type = TouchEventType::start;
    std::uint64_t timestampMs = 0;
};

class TouchEventQueue {
public:
    void setOnline(bool online);
    bool submitGesture(TouchGesture gesture, std::uint64_t nowMs);
    bool pop(TouchEvent& event);
    void clear();

    std::size_t size() const;
    bool online() const;

private:
    bool rateLimited(std::uint64_t nowMs);
    void rememberAdmission(std::uint64_t nowMs);
    bool push(TouchEvent event);
    void clearLocked();

    mutable std::mutex mutex_;
    bool online_ = false;
    bool sessionActive_ = false;
    bool strokeQueued_ = false;

    std::array<TouchEvent, kTouchEventQueueCapacity> queue_{};
    std::size_t queueHead_ = 0;
    std::size_t queueCount_ = 0;

    std::array<std::uint64_t, kTouchEventRateWindowCapacity> admissions_{};
    std::size_t admissionHead_ = 0;
    std::size_t admissionCount_ = 0;
    std::uint64_t lastAdmissionMs_ = 0;
    bool hasLastAdmission_ = false;
};

std::string serializeTouchEvent(const TouchEvent& event);

}  // namespace stackchan::companion
