#include "companion/touch_session.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

using namespace stackchan::companion;

namespace {

TouchSample sample(std::uint64_t timestampMs, std::uint8_t low = 0,
    std::uint8_t middle = 0, std::uint8_t high = 0)
{
    return {{{low, middle, high}}, timestampMs};
}

std::vector<TouchEvent> drain(TouchEventQueue& queue)
{
    std::vector<TouchEvent> result;
    TouchEvent event;
    while (queue.pop(event)) {
        result.push_back(event);
    }
    return result;
}

}  // namespace

int main()
{
    const auto dual = sample(0, 3, 3, 0);
    assert(dual.maximumIntensity() == 3);
    assert(dual.position() == -50);
    assert(sample(0).position() == 0);

    TouchGestureRecognizer click;
    assert(click.update(sample(0, 3)) == TouchGesture::none);
    assert(click.update(sample(50, 3)) == TouchGesture::press);
    assert(click.update(sample(100, 3)) == TouchGesture::none);
    assert(click.update(sample(150)) == TouchGesture::none);
    assert(click.update(sample(200)) == TouchGesture::release);

    TouchGestureRecognizer bounce;
    for (std::uint64_t nowMs = 0; nowMs < 500; nowMs += 25) {
        const bool touched = (nowMs / 25) % 2 == 0;
        assert(bounce.update(touched ? sample(nowMs, 3) : sample(nowMs))
            == TouchGesture::none);
    }

    TouchGestureRecognizer longHold;
    std::size_t pressCount = 0;
    std::size_t releaseCount = 0;
    for (std::uint64_t nowMs = 0; nowMs <= 500000; nowMs += 50) {
        const auto gesture = longHold.update(sample(nowMs, 0, 3));
        if (gesture == TouchGesture::press) ++pressCount;
        if (gesture == TouchGesture::release) ++releaseCount;
    }
    assert(pressCount == 1);
    assert(releaseCount == 0);
    assert(longHold.update(sample(500050)) == TouchGesture::none);
    assert(longHold.update(sample(500100)) == TouchGesture::release);

    TouchGestureRecognizer leftToRight;
    assert(leftToRight.update(sample(0, 3, 0, 0)) == TouchGesture::none);
    assert(leftToRight.update(sample(50, 3, 0, 0)) == TouchGesture::press);
    assert(leftToRight.update(sample(100, 0, 0, 3)) == TouchGesture::swipeLowToHigh);
    assert(leftToRight.update(sample(150, 0, 0, 3)) == TouchGesture::none);
    assert(leftToRight.update(sample(200)) == TouchGesture::none);
    assert(leftToRight.update(sample(250)) == TouchGesture::release);

    TouchGestureRecognizer rightToLeft;
    assert(rightToLeft.update(sample(0, 0, 0, 3)) == TouchGesture::none);
    assert(rightToLeft.update(sample(50, 0, 0, 3)) == TouchGesture::press);
    assert(rightToLeft.update(sample(100, 3, 0, 0)) == TouchGesture::swipeHighToLow);
    assert(rightToLeft.update(sample(150)) == TouchGesture::none);
    assert(rightToLeft.update(sample(200)) == TouchGesture::release);

    TouchGestureRecognizer simultaneous;
    assert(simultaneous.update(sample(0, 3, 3, 0)) == TouchGesture::none);
    assert(simultaneous.update(sample(50, 3, 3, 0)) == TouchGesture::press);
    assert(simultaneous.update(sample(100, 3, 3, 0)) == TouchGesture::none);
    assert(simultaneous.update(sample(150)) == TouchGesture::none);
    assert(simultaneous.update(sample(200)) == TouchGesture::release);

    TouchGestureRecognizer regression;
    regression.update(sample(100, 3));
    assert(regression.update(sample(50, 3)) == TouchGesture::none);
    assert(regression.update(sample(100, 3)) == TouchGesture::none);
    assert(regression.update(sample(150, 3)) == TouchGesture::press);

    TouchEventQueue events;
    assert(!events.online());
    assert(!events.submitGesture(TouchGesture::press, 0));
    events.setOnline(true);
    assert(events.online());
    assert(!events.submitGesture(TouchGesture::release, 0));
    assert(events.submitGesture(TouchGesture::press, 10));
    assert(!events.submitGesture(TouchGesture::press, 11));
    assert(events.submitGesture(TouchGesture::swipeLowToHigh, 60));
    assert(!events.submitGesture(TouchGesture::swipeHighToLow, 61));
    assert(events.submitGesture(TouchGesture::release, 110));
    const auto ordered = drain(events);
    assert(ordered.size() == 3);
    assert(ordered[0].type == TouchEventType::start);
    assert(ordered[1].type == TouchEventType::strokeLeftToRight);
    assert(ordered[2].type == TouchEventType::end);
    assert(serializeTouchEvent(ordered[0])
        == "{\"protocol\":2,\"type\":\"touch_start\",\"zone\":\"top\"}");
    assert(serializeTouchEvent(ordered[1]).find("\"direction\":\"left_to_right\"")
        != std::string::npos);
    assert(serializeTouchEvent({TouchEventType::strokeRightToLeft, 0})
        .find("\"direction\":\"right_to_left\"") != std::string::npos);

    TouchEventQueue disconnect;
    disconnect.setOnline(true);
    assert(disconnect.submitGesture(TouchGesture::press, 0));
    disconnect.setOnline(false);
    assert(disconnect.size() == 0);
    disconnect.setOnline(true);
    assert(!disconnect.submitGesture(TouchGesture::release, 100));
    assert(disconnect.submitGesture(TouchGesture::press, 200));
    assert(disconnect.submitGesture(TouchGesture::release, 250));

    TouchEventQueue full;
    full.setOnline(true);
    for (std::size_t index = 0; index < kTouchEventQueueCapacity / 2; ++index) {
        assert(full.submitGesture(TouchGesture::press, index * 100));
        assert(full.submitGesture(TouchGesture::release, index * 100 + 50));
    }
    assert(full.size() == kTouchEventQueueCapacity);
    assert(!full.submitGesture(TouchGesture::press, 500));
    const auto fullEvents = drain(full);
    for (std::size_t index = 0; index < fullEvents.size(); ++index) {
        assert(fullEvents[index].type
            == (index % 2 == 0 ? TouchEventType::start : TouchEventType::end));
    }

    TouchEventQueue rate;
    rate.setOnline(true);
    std::size_t admitted = 0;
    for (std::uint64_t nowMs = 0; nowMs < 20; nowMs += 2) {
        if (rate.submitGesture(TouchGesture::press, nowMs)) ++admitted;
        if (rate.submitGesture(TouchGesture::release, nowMs + 1)) ++admitted;
        drain(rate);
    }
    assert(admitted == kTouchEventRateLimit);
    assert(rate.submitGesture(TouchGesture::press, 1000));

    TouchGestureRecognizer randomizedRecognizer;
    TouchEventQueue randomizedEvents;
    randomizedEvents.setOnline(true);
    std::mt19937 random(0xD8001234U);
    std::uniform_int_distribution<int> channel(0, 3);
    bool networkSession = false;
    std::size_t emittedEvents = 0;
    for (std::uint64_t index = 0; index < 10000; ++index) {
        const auto gesture = randomizedRecognizer.update(sample(index * 50,
            static_cast<std::uint8_t>(channel(random)),
            static_cast<std::uint8_t>(channel(random)),
            static_cast<std::uint8_t>(channel(random))));
        randomizedEvents.submitGesture(gesture, index * 50);
        for (const auto& event : drain(randomizedEvents)) {
            ++emittedEvents;
            if (event.type == TouchEventType::start) {
                assert(!networkSession);
                networkSession = true;
            } else if (event.type == TouchEventType::end) {
                assert(networkSession);
                networkSession = false;
            } else {
                assert(networkSession);
            }
        }
    }
    for (std::uint64_t nowMs = 500000; nowMs <= 500150; nowMs += 50) {
        randomizedEvents.submitGesture(randomizedRecognizer.update(sample(nowMs)), nowMs);
        for (const auto& event : drain(randomizedEvents)) {
            ++emittedEvents;
            if (event.type == TouchEventType::start) {
                assert(!networkSession);
                networkSession = true;
            } else if (event.type == TouchEventType::end) {
                assert(networkSession);
                networkSession = false;
            } else {
                assert(networkSession);
            }
        }
    }
    assert(emittedEvents > 0);
    assert(!networkSession);
    assert(randomizedEvents.size() <= kTouchEventQueueCapacity);

    return 0;
}
