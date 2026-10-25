#pragma once

#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "core/log.h"
#include "net/guest_link.h"

namespace bk {

// A publish that cannot be set up: a bad request or a socket that will not bind.
struct ForwardError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// One published port: a host address that is forwarded to a port of the firmware.
struct Publish {
    std::string host_ip    = "127.0.0.1";
    uint16_t    host_port  = 0;
    uint16_t    guest_port = 0;
    bool        udp        = false;
};

// `[HOST_IP:]HOST_PORT:GUEST_PORT[/tcp|/udp]`, ports 1-65535. Throws ForwardError.
Publish parse_publish(const std::string &text);

// `<protocol> <host_ip>:<host_port> -> guest port <guest_port>`
std::string describe(const Publish &p);

// Carries host sockets to the firmware through the host IP stack: every TCP
// client of a published port becomes a flow towards the firmware, every UDP
// client a local stack port. Nothing blocks; `poll` moves whatever is ready.
class Forwarder {
  public:
    static constexpr uint64_t WAIT_FOR_ADDRESS_NS = 30'000'000'000;
    static constexpr uint64_t UDP_IDLE_NS         = 60'000'000'000;
    static constexpr size_t   READ_SIZE           = 16384;

    // Opens a socket per publish; throws ForwardError (after closing what was
    // opened) when one cannot be bound. A host port of 0 takes a free one.
    Forwarder(GuestLink &link, const std::vector<Publish> &publishes);
    ~Forwarder();
    Forwarder(const Forwarder &)            = delete;
    Forwarder &operator=(const Forwarder &) = delete;

    // The publishes as bound, with the real host ports.
    const std::vector<Publish> &addresses() const { return addresses_; }
    bool                        empty() const { return listeners_.empty(); }
    size_t                      connections() const { return connections_.size(); }
    size_t                      udp_clients() const;

    void poll(uint64_t now_ns);
    // Aborts every flow, closes every connection and forgets the UDP clients;
    // the listening sockets stay open. The stack's own reset ends flows without
    // a word, so this has to come first.
    void reset();
    // reset() and the listeners too. Safe to call twice.
    void close();

  private:
    struct Listener {
        int     fd = -1;
        Publish publish;
    };

    struct Connection {
        int                  fd = -1;
        std::string          peer;
        uint16_t             guest_port = 0;
        uint64_t             accepted   = 0;
        GuestLink::TcpHandle flow;
        std::vector<uint8_t> to_host; // from the firmware, not yet sent
        size_t               sent      = 0;
        bool                 reset     = false; // the flow ended with a reset
        bool                 host_eof  = false;
        bool                 shut_down = false;
    };

    struct UdpClient {
        uint16_t local_port = 0;
        uint64_t last_seen  = 0;
    };

    using UdpKey = std::pair<uint32_t, uint16_t>; // address and port, network order

    void accept_clients(const Listener &l, uint64_t now);
    // False when the connection is to be dropped.
    bool serve(Connection &c, uint64_t now);
    void drop(Connection &c);
    void serve_udp(Listener &l, uint64_t now);
    void forget_clients(Listener &l);

    GuestLink                                 &link_;
    Logger                                    &log_;
    std::vector<Publish>                       addresses_;
    std::vector<Listener>                      listeners_;
    std::list<Connection>                      connections_;
    std::map<int, std::map<UdpKey, UdpClient>> clients_; // by listener fd
    uint64_t                                   now_ = 0;
};

} // namespace bk
