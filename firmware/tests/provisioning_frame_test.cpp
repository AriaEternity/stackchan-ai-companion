#include "companion/provisioning_frame.h"

#include <cassert>
#include <string>

using stackchan::companion::ProvisioningFeedResult;
using stackchan::companion::ProvisioningFrameParser;

ProvisioningFeedResult feed(ProvisioningFrameParser& parser, const std::string& frame)
{
    ProvisioningFeedResult result = ProvisioningFeedResult::waiting;
    for (char value : frame) {
        result = parser.feed(value);
    }
    return result;
}

int main()
{
    ProvisioningFrameParser parser;
    assert(feed(parser, "SC-PROVISION-1 fixture-device-0001 token_value.1234\r\n")
        == ProvisioningFeedResult::accepted);
    const auto credentials = parser.takeCredentials();
    assert(credentials.deviceId == "fixture-device-0001");
    assert(credentials.deviceToken == "token_value.1234");

    assert(feed(parser, "SC-PROVISION-1 fixture-device-0001 ")
        == ProvisioningFeedResult::waiting);
    assert(feed(parser, "token_value.5678\n") == ProvisioningFeedResult::accepted);
    assert(parser.takeCredentials().deviceToken == "token_value.5678");

    assert(feed(parser, "SC-PROVISION-2 fixture-device-0001 token\n")
        == ProvisioningFeedResult::rejected);
    assert(feed(parser, "SC-PROVISION-1 contains space token\n")
        == ProvisioningFeedResult::rejected);
    assert(feed(parser, "SC-PROVISION-1 fixture-device-0001 bad:token\n")
        == ProvisioningFeedResult::rejected);
    assert(feed(parser, "SC-PROVISION-1 stackchan-\r-k151 token\n")
        == ProvisioningFeedResult::rejected);
    assert(feed(parser, std::string(300, 'x') + "\n") == ProvisioningFeedResult::rejected);

    assert(feed(parser, "ignored log line\nSC-PROVISION-1 id token\n")
        == ProvisioningFeedResult::accepted);
    assert(parser.takeCredentials().deviceId == "id");
    parser.reset();
    return 0;
}
