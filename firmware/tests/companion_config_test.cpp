#include "companion/companion_config.h"

#include <cassert>
#include <string>

using stackchan::companion::CompanionCredentials;
using stackchan::companion::CompanionStartupState;
using stackchan::companion::isValidDeviceId;
using stackchan::companion::isValidDeviceToken;
using stackchan::companion::maskedTokenStatus;
using stackchan::companion::startupState;

int main()
{
    assert(isValidDeviceId("fixture-device-0001"));
    assert(!isValidDeviceId(""));
    assert(!isValidDeviceId("contains space"));
    assert(!isValidDeviceId(std::string(65, 'a')));

    assert(isValidDeviceToken("example-token_value.1234"));
    assert(!isValidDeviceToken(""));
    assert(!isValidDeviceToken("contains space"));
    assert(!isValidDeviceToken(std::string(129, 'a')));

    const std::string token = "example-token_value.1234";
    const std::string masked = maskedTokenStatus(token);
    assert(masked.find(token) == std::string::npos);
    assert(masked.find("1234") != std::string::npos);
    assert(maskedTokenStatus("") == "NOT SET");

    assert(startupState({}) == CompanionStartupState::unconfigured);
    assert(startupState(CompanionCredentials{"fixture-device-0001", token})
        == CompanionStartupState::offlineSafe);

    return 0;
}
