// WASM stub: UPnP not available in browser sandbox
#include <common/upnp.h>

namespace UPnP {
    void TryPortmapping(std::uint16_t port, bool is_udp) {}
    void StopPortmapping(std::uint16_t port, bool is_udp) {}
}
