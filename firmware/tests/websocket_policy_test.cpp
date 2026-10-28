#include "companion/websocket_policy.h"

#include <cassert>
#include <string>

using stackchan::companion::SecureWebSocketTarget;
using stackchan::companion::WebSocketHandshakeStatus;
using stackchan::companion::parseSecureWebSocketUrl;
using stackchan::companion::makeBearerAuthorization;
using stackchan::companion::validateWebSocketHandshake;

int main()
{
    SecureWebSocketTarget target;
    assert(parseSecureWebSocketUrl("wss://stackchan.example.com/ws/device", target));
    assert(target.host == "stackchan.example.com");
    assert(target.path == "/ws/device");
    assert(target.port == 443);
    assert(parseSecureWebSocketUrl("wss://localhost:8443/device", target));
    assert(target.port == 8443);

    assert(!parseSecureWebSocketUrl("ws://stackchan.example.com/ws/device", target));
    assert(!parseSecureWebSocketUrl("https://stackchan.example.com/ws/device", target));
    assert(!parseSecureWebSocketUrl("wss://stackchan.example.com/ws/device?token=forbidden", target));
    assert(!parseSecureWebSocketUrl("wss://user@stackchan.example.com/ws/device", target));
    assert(!parseSecureWebSocketUrl("wss://stackchan.example.com:0/ws/device", target));
    assert(stackchan::companion::kAuthorizationHeaderName == "Authorization");
    assert(stackchan::companion::kDeviceIdHeaderName == "X-StackChan-Device-ID");
    assert(stackchan::companion::kProtocolHeaderName == "X-StackChan-Protocol");
    assert(stackchan::companion::kProtocolHeaderValue == "2");
    assert(makeBearerAuthorization("placeholder-token") == "Bearer placeholder-token");

    constexpr const char* expectedAccept = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";
    const std::string accepted =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n";
    assert(validateWebSocketHandshake(accepted, expectedAccept) == WebSocketHandshakeStatus::accepted);
    assert(validateWebSocketHandshake("HTTP/1.1 401 Unauthorized\r\n\r\n", expectedAccept)
        == WebSocketHandshakeStatus::unauthorized);
    assert(validateWebSocketHandshake("HTTP/1.1 403 Forbidden\r\n\r\n", expectedAccept)
        == WebSocketHandshakeStatus::forbidden);
    assert(validateWebSocketHandshake("HTTP/1.1 503 Service Unavailable\r\n\r\n", expectedAccept)
        == WebSocketHandshakeStatus::unavailable);
    assert(validateWebSocketHandshake(accepted, "wrong") == WebSocketHandshakeStatus::protocolError);
    const std::string duplicateAccept = accepted.substr(0, accepted.size() - 2)
        + "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n";
    assert(validateWebSocketHandshake(duplicateAccept, expectedAccept)
        == WebSocketHandshakeStatus::protocolError);
    assert(validateWebSocketHandshake(std::string(4097, 'x'), expectedAccept)
        == WebSocketHandshakeStatus::protocolError);

    return 0;
}
