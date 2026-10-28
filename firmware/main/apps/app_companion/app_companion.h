/*
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <mooncake.h>

class AppCompanion : public mooncake::AppAbility {
public:
    AppCompanion();

    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;
};
