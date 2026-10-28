/*
 * SPDX-License-Identifier: MIT
 */
#include "workers.h"

#include <apps/common/toast/toast.h>
#include <companion/motion_safety_store.h>
#include <mooncake_log.h>
#include <stackchan/stackchan.h>

#include <string>

using namespace smooth_ui_toolkit::lvgl_cpp;
using namespace stackchan::companion;
using namespace stackchan;
using namespace setup_workers;

MotionSafetyWorker::MotionSafetyWorker()
{
    _panel = std::make_unique<Container>(lv_screen_active());
    _panel->setBgColor(lv_color_hex(0xFFFFFF));
    _panel->align(LV_ALIGN_CENTER, 0, 0);
    _panel->setBorderWidth(0);
    _panel->setSize(320, 240);
    _panel->setRadius(0);
    _panel->setFlexFlow(LV_FLEX_FLOW_COLUMN);
    _panel->setFlexAlign(LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    _panel->setPadding(10, 10, 4, 4);
    _panel->setPadRow(4);

    _status = std::make_unique<Label>(*_panel);
    _status->setWidth(292);
    _status->setTextFont(&lv_font_montserrat_14);
    _status->setTextAlign(LV_TEXT_ALIGN_CENTER);

    _btn_yaw_direction = std::make_unique<Button>(*_panel);
    apply_button_common_style(*_btn_yaw_direction);
    _btn_yaw_direction->setSize(292, 32);
    _btn_yaw_direction->onClick().connect([this]() {
        if (_test_running) {
            return;
        }
        _yaw_direction = -_yaw_direction;
        refresh_status();
    });

    _btn_pitch_direction = std::make_unique<Button>(*_panel);
    apply_button_common_style(*_btn_pitch_direction);
    _btn_pitch_direction->setSize(292, 32);
    _btn_pitch_direction->onClick().connect([this]() {
        if (_test_running) {
            return;
        }
        _pitch_direction = -_pitch_direction;
        refresh_status();
    });

    _btn_prepare = std::make_unique<Button>(*_panel);
    apply_button_common_style(*_btn_prepare);
    _btn_prepare->setSize(292, 34);
    _btn_prepare->setBgColor(lv_color_hex(0xFFDF9A));
    _btn_prepare->onClick().connect([this]() {
        if (!_test_running) {
            start_safe_test();
        }
    });

    _btn_confirm = std::make_unique<Button>(*_panel);
    apply_button_common_style(*_btn_confirm);
    _btn_confirm->setSize(292, 34);
    _btn_confirm->setBgColor(lv_color_hex(0xBAE4BA));
    _btn_confirm->onClick().connect([this]() {
        if (_armed) {
            _save_requested = true;
        }
    });

    _btn_clear = std::make_unique<Button>(*_panel);
    apply_button_common_style(*_btn_clear);
    _btn_clear->setSize(142, 30);
    _btn_clear->label().setText("Lock Profile");
    _btn_clear->onClick().connect([this]() { _clear_requested = true; });

    _btn_done = std::make_unique<Button>(*_panel);
    apply_button_common_style(*_btn_done);
    _btn_done->setSize(142, 30);
    _btn_done->label().setText("Done");
    _btn_done->onClick().connect([this]() { _done_requested = true; });

    refresh_status();
}

MotionSafetyWorker::~MotionSafetyWorker()
{
    if (_test_running) {
        GetStackChan().motion().stop();
        GetStackChan().motion().setAutoTorqueReleaseEnabled(true);
    }
}

void MotionSafetyWorker::refresh_status()
{
    const ProjectMotionSafety profile = loadProjectMotionSafety();
    std::string status                = profile.calibrated ? "Profile: ENABLED" : "Profile: LOCKED";
    status += "\nYaw dir: ";
    status += _yaw_direction > 0 ? "+" : "-";
    status += "  Pitch dir: ";
    status += _pitch_direction > 0 ? "+" : "-";
    status += "\nNormal: +Yaw=left, +Pitch=up";
    if (_test_running) {
        status += "\nTest: Home, +/-Yaw, +/-Pitch, Home";
    } else if (_test_completed) {
        status += "\nTest complete; adjust direction if needed, then Confirm";
    } else {
        status += "\nPrepare runs a bounded physical test before saving";
    }
    _status->setText(status);
    _btn_yaw_direction->label().setText(std::string("Yaw Direction: ") + (_yaw_direction > 0 ? "Normal" : "Reversed"));
    _btn_pitch_direction->label().setText(std::string("Pitch Direction: ") +
                                          (_pitch_direction > 0 ? "Normal" : "Reversed"));
    _btn_prepare->label().setText(_test_running ? "Testing..."
                                                : (_test_completed ? "Run Test Again" : "Run Home + 60/40 Test"));
    _btn_confirm->label().setText(_test_completed ? "Confirm And Save" : "Confirm (Run Test First)");
}

void MotionSafetyWorker::start_safe_test()
{
    _test_targets    = makeMotionSafetyTestPlan();
    _test_step       = 0;
    _test_hold_until = 0;
    _test_running    = true;
    _test_completed  = false;
    _armed           = false;
    refresh_status();
}

void MotionSafetyWorker::update_safe_test()
{
    if (!_test_running) {
        return;
    }
    auto& motion       = GetStackChan().motion();
    const uint32_t now = GetHAL().millis();
    if (motion.isMoving() || now < _test_hold_until) {
        return;
    }
    if (_test_step >= _test_targets.size()) {
        _test_running   = false;
        _test_completed = true;
        _armed          = true;
        refresh_status();
        view::pop_a_toast("Safe motion test complete", view::ToastType::Success);
        return;
    }
    const auto& target = _test_targets[_test_step++];
    motion.setTorqueEnabled(true);
    motion.setAutoTorqueReleaseEnabled(true);
    motion.moveWithSpeed(target.yaw, target.pitch, target.speed);
    _test_hold_until = now + kMotionSafetyTestHoldMs;
}

void MotionSafetyWorker::save_profile()
{
    const ProjectMotionSafety candidate = makeConservativeMotionSafetyProfile(_yaw_direction, _pitch_direction);

    if (!saveProjectMotionSafety(candidate)) {
        mclog::tagWarn("Setup-Motion", "profile save or read-back verification failed; remains locked");
        view::pop_a_toast("Save failed; profile remains locked", view::ToastType::Error);
        return;
    }
    mclog::tagInfo("Setup-Motion", "saved conservative motion profile after local confirmation");
    view::pop_a_toast("Conservative profile saved", view::ToastType::Success);
    _armed = false;
    refresh_status();
}

void MotionSafetyWorker::update()
{
    if (_clear_requested) {
        _clear_requested = false;
        if (_test_running) {
            GetStackChan().motion().stop();
            GetStackChan().motion().setAutoTorqueReleaseEnabled(true);
        }
        _test_running   = false;
        _test_completed = false;
        _armed          = false;
        if (clearProjectMotionSafety()) {
            view::pop_a_toast("Motion locked", view::ToastType::Success);
        } else {
            view::pop_a_toast("Lock failed; do not run motion", view::ToastType::Error);
        }
        refresh_status();
    }
    if (_test_running) {
        update_safe_test();
        return;
    }
    if (_save_requested) {
        _save_requested = false;
        save_profile();
    }
    if (_done_requested) {
        _done_requested = false;
        _is_done        = true;
    }
}
