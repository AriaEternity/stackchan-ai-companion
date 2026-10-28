/*
 * SPDX-License-Identifier: MIT
 */
#include "touch_session.h"

#include <algorithm>
#include <utility>

namespace stackchan::companion {
namespace {

constexpr std::uint64_t kTouchRateWindowMs = 1000;

TouchGestureConfig boundedConfig(TouchGestureConfig config)
{
    config.touchThreshold = std::clamp<std::uint8_t>(config.touchThreshold, 1, 3);
    config.swipeThreshold = std::clamp(config.swipeThreshold, 1, 200);
    config.pressDebounceMs = std::clamp<std::uint64_t>(config.pressDebounceMs, 1, 1000);
    config.releaseDebounceMs = std::clamp<std::uint64_t>(config.releaseDebounceMs, 1, 1000);
    return config;
}

}  // namespace

std::uint8_t TouchSample::maximumIntensity() const
{
    return std::max({channels[0], channels[1], channels[2]});
}

int TouchSample::position() const
{
    const int total = static_cast<int>(channels[0]) + static_cast<int>(channels[1])
        + static_cast<int>(channels[2]);
    if (total == 0) {
        return 0;
    }
    const int weighted = static_cast<int>(channels[0]) * -100
        + static_cast<int>(channels[2]) * 100;
    return weighted / total;
}

TouchGestureRecognizer::TouchGestureRecognizer(TouchGestureConfig config)
    : config_(boundedConfig(config))
{
}

TouchGesture TouchGestureRecognizer::update(const TouchSample& sample)
{
    if (hasTimestamp_ && sample.timestampMs < lastTimestampMs_) {
        reset();
        lastTimestampMs_ = sample.timestampMs;
        hasTimestamp_ = true;
        return TouchGesture::none;
    }
    lastTimestampMs_ = sample.timestampMs;
    hasTimestamp_ = true;
    const bool isTouched = touched(sample);

    switch (state_) {
    case State::idle:
        if (isTouched) {
            state_ = State::pressDebounce;
            candidateSinceMs_ = sample.timestampMs;
            initialPosition_ = sample.position();
        }
        return TouchGesture::none;

    case State::pressDebounce:
        if (!isTouched) {
            state_ = State::idle;
            return TouchGesture::none;
        }
        if (sample.timestampMs - candidateSinceMs_ >= config_.pressDebounceMs) {
            state_ = State::touched;
            return TouchGesture::press;
        }
        return TouchGesture::none;

    case State::touched:
        if (!isTouched) {
            stateBeforeRelease_ = State::touched;
            state_ = State::releaseDebounce;
            candidateSinceMs_ = sample.timestampMs;
            return TouchGesture::none;
        }
        if (const int delta = sample.position() - initialPosition_;
            delta > config_.swipeThreshold) {
            state_ = State::swiped;
            return TouchGesture::swipeLowToHigh;
        } else if (delta < -config_.swipeThreshold) {
            state_ = State::swiped;
            return TouchGesture::swipeHighToLow;
        }
        return TouchGesture::none;

    case State::swiped:
        if (!isTouched) {
            stateBeforeRelease_ = State::swiped;
            state_ = State::releaseDebounce;
            candidateSinceMs_ = sample.timestampMs;
        }
        return TouchGesture::none;

    case State::releaseDebounce:
        if (isTouched) {
            state_ = stateBeforeRelease_;
            return TouchGesture::none;
        }
        if (sample.timestampMs - candidateSinceMs_ >= config_.releaseDebounceMs) {
            state_ = State::idle;
            return TouchGesture::release;
        }
        return TouchGesture::none;
    }
    return TouchGesture::none;
}

void TouchGestureRecognizer::reset()
{
    state_ = State::idle;
    stateBeforeRelease_ = State::touched;
    candidateSinceMs_ = 0;
    lastTimestampMs_ = 0;
    hasTimestamp_ = false;
    initialPosition_ = 0;
}

bool TouchGestureRecognizer::touched(const TouchSample& sample) const
{
    return sample.maximumIntensity() >= config_.touchThreshold;
}

void TouchEventQueue::setOnline(bool online)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (online_ == online) {
        return;
    }
    online_ = online;
    clearLocked();
}

bool TouchEventQueue::submitGesture(TouchGesture gesture, std::uint64_t nowMs)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!online_ || gesture == TouchGesture::none) {
        return false;
    }

    if (gesture == TouchGesture::press) {
        if (sessionActive_ || queue_.size() - queueCount_ < 2 || rateLimited(nowMs)) {
            return false;
        }
        if (!push({TouchEventType::start, nowMs})) {
            return false;
        }
        sessionActive_ = true;
        strokeQueued_ = false;
        rememberAdmission(nowMs);
        return true;
    }

    if (gesture == TouchGesture::release) {
        if (!sessionActive_ || queueCount_ >= queue_.size()) {
            return false;
        }
        const bool queued = push({TouchEventType::end, nowMs});
        sessionActive_ = false;
        strokeQueued_ = false;
        if (queued) {
            rememberAdmission(nowMs);
        }
        return queued;
    }

    if (!sessionActive_ || strokeQueued_ || queue_.size() - queueCount_ < 2
        || rateLimited(nowMs)) {
        return false;
    }
    const TouchEventType type = gesture == TouchGesture::swipeLowToHigh
        ? TouchEventType::strokeLeftToRight
        : TouchEventType::strokeRightToLeft;
    if (!push({type, nowMs})) {
        return false;
    }
    strokeQueued_ = true;
    rememberAdmission(nowMs);
    return true;
}

bool TouchEventQueue::pop(TouchEvent& event)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (queueCount_ == 0) {
        return false;
    }
    event = queue_[queueHead_];
    queue_[queueHead_] = {};
    queueHead_ = (queueHead_ + 1) % queue_.size();
    --queueCount_;
    return true;
}

void TouchEventQueue::clear()
{
    std::lock_guard<std::mutex> lock(mutex_);
    clearLocked();
}

std::size_t TouchEventQueue::size() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return queueCount_;
}

bool TouchEventQueue::online() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return online_;
}

bool TouchEventQueue::rateLimited(std::uint64_t nowMs)
{
    if (hasLastAdmission_ && nowMs < lastAdmissionMs_) {
        admissionHead_ = 0;
        admissionCount_ = 0;
        hasLastAdmission_ = false;
    }
    while (admissionCount_ > 0) {
        const std::uint64_t oldest = admissions_[admissionHead_];
        if (nowMs >= oldest && nowMs - oldest >= kTouchRateWindowMs) {
            admissionHead_ = (admissionHead_ + 1) % admissions_.size();
            --admissionCount_;
        } else {
            break;
        }
    }
    return admissionCount_ >= kTouchEventRateLimit;
}

void TouchEventQueue::rememberAdmission(std::uint64_t nowMs)
{
    rateLimited(nowMs);
    if (admissionCount_ == admissions_.size()) {
        admissionHead_ = (admissionHead_ + 1) % admissions_.size();
        --admissionCount_;
    }
    const std::size_t tail = (admissionHead_ + admissionCount_) % admissions_.size();
    admissions_[tail] = nowMs;
    ++admissionCount_;
    lastAdmissionMs_ = nowMs;
    hasLastAdmission_ = true;
}

bool TouchEventQueue::push(TouchEvent event)
{
    if (queueCount_ >= queue_.size()) {
        return false;
    }
    const std::size_t tail = (queueHead_ + queueCount_) % queue_.size();
    queue_[tail] = std::move(event);
    ++queueCount_;
    return true;
}

void TouchEventQueue::clearLocked()
{
    queue_ = {};
    queueHead_ = 0;
    queueCount_ = 0;
    sessionActive_ = false;
    strokeQueued_ = false;
    admissions_ = {};
    admissionHead_ = 0;
    admissionCount_ = 0;
    lastAdmissionMs_ = 0;
    hasLastAdmission_ = false;
}

std::string serializeTouchEvent(const TouchEvent& event)
{
    switch (event.type) {
    case TouchEventType::start:
        return "{\"protocol\":2,\"type\":\"touch_start\",\"zone\":\"top\"}";
    case TouchEventType::end:
        return "{\"protocol\":2,\"type\":\"touch_end\",\"zone\":\"top\"}";
    case TouchEventType::strokeLeftToRight:
        return "{\"protocol\":2,\"type\":\"stroke\",\"zone\":\"top\",\"direction\":\"left_to_right\"}";
    case TouchEventType::strokeRightToLeft:
        return "{\"protocol\":2,\"type\":\"stroke\",\"zone\":\"top\",\"direction\":\"right_to_left\"}";
    }
    return {};
}

}  // namespace stackchan::companion
