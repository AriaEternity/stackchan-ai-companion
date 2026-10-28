/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include "board/hal_bridge.h"
#include "drivers/Si12T/Si12T.h"

#include <companion/touch_session.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mooncake_log.h>

#include <atomic>
#include <cstdint>
#include <string_view>

namespace {

constexpr std::string_view kTag = "HAL-HeadTouch";
constexpr std::uint64_t kCalibrationLogIntervalMs = 200;

std::atomic<bool> calibrationEnabled{false};

HeadPetGesture mapGesture(stackchan::companion::TouchGesture gesture)
{
    using stackchan::companion::TouchGesture;
    switch (gesture) {
    case TouchGesture::press: return HeadPetGesture::Press;
    case TouchGesture::release: return HeadPetGesture::Release;
    case TouchGesture::swipeLowToHigh: return HeadPetGesture::SwipeForward;
    case TouchGesture::swipeHighToLow: return HeadPetGesture::SwipeBackward;
    case TouchGesture::none: return HeadPetGesture::None;
    }
    return HeadPetGesture::None;
}

void headTouchUpdateTask(void* parameter)
{
    mclog::tagInfo(kTag, "start update task");

    const auto si12t = static_cast<si12t_handle_t>(parameter);
    stackchan::companion::TouchGestureRecognizer recognizer;
    std::uint64_t lastCalibrationLogMs = 0;
    bool hasCalibrationLog = false;

    vTaskDelay(pdMS_TO_TICKS(200));

    while (true) {
        std::uint8_t touchResult = 0;
        if (si12t_read_touch_result(si12t, &touchResult) == ESP_OK) {
            std::uint8_t parsed[3]{};
            si12t_parse_touch_result_to(touchResult, parsed);
            stackchan::companion::TouchSample sample;
            sample.channels = {parsed[0], parsed[1], parsed[2]};
            sample.timestampMs = static_cast<std::uint64_t>(xTaskGetTickCount())
                * static_cast<std::uint64_t>(portTICK_PERIOD_MS);

            if (calibrationEnabled.load(std::memory_order_relaxed)
                && (!hasCalibrationLog || sample.timestampMs < lastCalibrationLogMs
                    || sample.timestampMs - lastCalibrationLogMs >= kCalibrationLogIntervalMs)) {
                // Bounded diagnostic output only: uptime and the three 0..3 channel classes.
                // This module does not persist samples, identifiers, or touch trajectories.
                mclog::tagInfo(kTag, "calibration t_ms={} channels=[{},{},{}]",
                    sample.timestampMs, static_cast<unsigned>(sample.channels[0]),
                    static_cast<unsigned>(sample.channels[1]),
                    static_cast<unsigned>(sample.channels[2]));
                lastCalibrationLogMs = sample.timestampMs;
                hasCalibrationLog = true;
            }

            const HeadPetGesture gesture = mapGesture(recognizer.update(sample));
            if (gesture != HeadPetGesture::None) {
                GetHAL().onHeadPetGesture.emit(gesture);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

}  // namespace

void Hal::head_touch_init()
{
    mclog::tagInfo(kTag, "init");

    auto i2cBus = hal_bridge::board_get_i2c_bus();
    si12t_config_t si12tConfig = {
        .i2c_bus = i2cBus,
        .dev_addr = SI12T_GND_ADDRESS,
    };
    static si12t_handle_t si12t;
    if (si12t_init(&si12tConfig, &si12t) != ESP_OK) {
        mclog::tagError(kTag, "Si12T initialization failed; touch task disabled");
        return;
    }
    if (si12t_setup(si12t, SI12T_TYPE_LOW, SI12T_SENSITIVITY_LEVEL_3) != ESP_OK) {
        mclog::tagError(kTag, "Si12T setup failed; touch task disabled");
        si12t_delete(si12t);
        si12t = nullptr;
        return;
    }

    if (xTaskCreatePinnedToCoreWithCaps(headTouchUpdateTask, "headtouch", 1024 * 6, si12t, 2,
            nullptr, 1, MALLOC_CAP_SPIRAM) != pdPASS) {
        mclog::tagError(kTag, "touch task creation failed; touch input disabled");
        si12t_delete(si12t);
        si12t = nullptr;
    }
}

void Hal::setHeadTouchCalibrationEnabled(bool enabled)
{
    calibrationEnabled.store(enabled, std::memory_order_relaxed);
    mclog::tagInfo(kTag, "calibration output {}", enabled ? "enabled" : "disabled");
}

bool Hal::isHeadTouchCalibrationEnabled() const
{
    return calibrationEnabled.load(std::memory_order_relaxed);
}
