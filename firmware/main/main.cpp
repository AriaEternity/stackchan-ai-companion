/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include <smooth_ui_toolkit.hpp>
#include <uitk/short_namespace.hpp>
#include <mooncake_log.h>
#include <mooncake.h>
#include <apps/apps.h>
#include <hal/hal.h>

using namespace mooncake;
using namespace smooth_ui_toolkit;

static_assert(sizeof(CONFIG_STACKCHAN_SERVER_URL) == 1,
    "The Companion candidate must not embed the upstream StackChan cloud endpoint");
static_assert(sizeof(CONFIG_OTA_URL) == 1,
    "The Companion candidate must not embed the upstream Xiaozhi OTA endpoint");

extern "C" void app_main(void)
{
    // Setup logger
    mclog::set_level(mclog::level_info);
    mclog::set_time_format(mclog::time_format_unix_milliseconds);

    // HAL init
    GetHAL().init();

    // Setup ui hal
    ui_hal::on_delay([](uint32_t ms) { GetHAL().delay(ms); });
    ui_hal::on_get_tick([]() { return GetHAL().millis(); });

    // Install only local/recovery applications and the private Companion path.
    // The upstream AI.AGENT, AVATAR media WebSocket, App Center, and EzData
    // applications remain in the imported source for reference but are not
    // reachable in the Companion candidate. Persisted upstream NVS flags also
    // cannot bypass this allow-list or start Xiaozhi on boot.
    GetMooncake().installApp(std::make_unique<AppLauncher>());
    GetMooncake().installApp(std::make_unique<AppCompanion>());
    GetMooncake().installApp(std::make_unique<AppEspnowControl>());
    GetMooncake().installApp(std::make_unique<AppDance>());
    GetMooncake().installApp(std::make_unique<AppSetup>());

    while (1) {
        GetHAL().feedTheDog();
        GetHAL().updateHeapStatusLog();
        GetMooncake().update();
    }
}
