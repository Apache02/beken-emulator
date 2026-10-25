#include <doctest/doctest.h>

#include <initializer_list>
#include <sstream>
#include <string>
#include <vector>

#include "core/log.h"
#include "net/station.h"

using namespace bk;
using Bytes = std::vector<uint8_t>;
using Mac   = Station::Mac;

namespace {

constexpr Mac kSta   = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
constexpr Mac kAp    = {0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0x01};
constexpr Mac kOther = {0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0x02};
constexpr Mac kPeer  = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
constexpr Mac kBcast = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

constexpr uint64_t kMs = 1'000'000;

enum : uint8_t {
    AssocReq  = 0,
    AssocResp = 1,
    ProbeReq  = 4,
    ProbeResp = 5,
    Beacon    = 8,
    Disassoc  = 10,
    Auth      = 11,
    Deauth    = 12
};

Bytes cat(std::initializer_list<Bytes> parts) {
    Bytes r;
    for (const Bytes &p : parts) r.insert(r.end(), p.begin(), p.end());
    return r;
}

Bytes m(const Mac &x) { return Bytes(x.begin(), x.end()); }

Bytes ssid_el(const std::string &s) {
    Bytes r{0, uint8_t(s.size())};
    r.insert(r.end(), s.begin(), s.end());
    return r;
}

const Bytes kRatesEl    = {0x01, 0x08, 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24};
const Bytes kExtRatesEl = {0x32, 0x04, 0x30, 0x48, 0x60, 0x6c};

// A frame as the firmware would send it, header sequence number 0.
Bytes frame(uint8_t fc0, uint8_t fc1, const Mac &a1, const Mac &a2, const Mac &a3, const Bytes &body) {
    return cat({
        {fc0, fc1, 0, 0},
        m(a1), m(a2), m(a3), {0, 0},
        body
    });
}

Bytes mgmt(uint8_t subtype, const Mac &a1, const Mac &a2, const Mac &a3, const Bytes &body) {
    return frame(uint8_t(subtype << 4), 0, a1, a2, a3, body);
}

Bytes beacon_body(const std::string &ssid, uint16_t cap = 0x0401) {
    return cat({
        Bytes(8, 0), {0x64, 0x00, uint8_t(cap), uint8_t(cap >> 8)},
          ssid_el(ssid), kRatesEl
    });
}

Bytes beacon(const std::string &ssid, uint16_t cap = 0x0401, const Mac &bss = kAp) {
    return mgmt(Beacon, kBcast, bss, bss, beacon_body(ssid, cap));
}

Bytes auth_resp(uint16_t seq, uint16_t status) {
    return mgmt(Auth, kSta, kAp, kAp, {0, 0, uint8_t(seq), uint8_t(seq >> 8), uint8_t(status), uint8_t(status >> 8)});
}

Bytes assoc_resp(uint16_t status, uint16_t aid) {
    return mgmt(AssocResp,
                kSta,
                kAp,
                kAp,
                {0x01, 0x04, uint8_t(status), uint8_t(status >> 8), uint8_t(aid), uint8_t(aid >> 8)});
}

struct Fixture {
    std::ostringstream    out;
    std::vector<Bytes>    air; // frames handed to the MAC
    std::vector<Bytes>    eth; // frames handed to the stack
    std::vector<uint64_t> ups;
    Station               st;
    uint64_t              now = 0;

    explicit Fixture(std::optional<std::string> filter = std::nullopt)
        : st(kSta,
             std::move(filter),
             (Logger::reset_for_tests(), Logger::configure(out, false), Logger::get("station"))) {
        st.attach([this](Bytes f) { air.push_back(std::move(f)); });
        st.set_ethernet_in([this](Bytes f) { eth.push_back(std::move(f)); });
        st.set_link_up([this](uint64_t t) { ups.push_back(t); });
    }

    void tick(uint64_t t) {
        now = t;
        st.tick(t);
    }
    void fw(const Bytes &f) { st.transmit(f); }

    // Moves time to the next event and past the reply latency.
    void settle() {
        tick(*st.next_event_ns());
        tick(now + Station::LATENCY_NS);
    }

    // Brings the station to `run` through a beacon, then returns with air cleared.
    void join(const std::string &ssid = "cam") {
        tick(Station::RETRY_NS);
        fw(beacon(ssid));
        tick(now + Station::AUTH_DELAY_NS);
        tick(now + Station::LATENCY_NS);
        fw(auth_resp(2, 0));
        tick(now);
        tick(now + Station::LATENCY_NS);
        fw(assoc_resp(0, 0xc001));
        REQUIRE(st.state() == Station::State::Run);
        air.clear();
    }
};

} // namespace

TEST_CASE("station walks a whole handshake with exact frames") {
    Fixture f("cam");
    CHECK(f.st.state() == Station::State::Scan);
    CHECK_FALSE(f.st.bssid());

    // Nothing until the first retry period.
    f.tick(0);
    CHECK(f.st.next_event_ns() == Station::RETRY_NS);
    f.tick(Station::RETRY_NS);
    CHECK(f.air.empty());
    f.tick(Station::RETRY_NS + Station::LATENCY_NS);
    REQUIRE(f.air.size() == 1);
    CHECK(f.air[0] == cat({
                          {0x40, 0x00, 0x00, 0x00},
                          m(kBcast),
                          m(kSta),
                          m(kBcast),
                          {0x00, 0x00},
                          ssid_el("cam"),
                          kRatesEl,
                          kExtRatesEl
    }));
    CHECK(f.air[0].size() == 24 + 5 + 10 + 6);

    uint64_t t = f.now;
    f.fw(beacon("cam"));
    CHECK(f.st.state() == Station::State::Auth);
    CHECK(f.st.bssid() == kAp);
    CHECK(f.st.next_event_ns() == t + 50 * kMs);

    f.tick(t + 50 * kMs);
    f.tick(t + 50 * kMs + Station::LATENCY_NS);
    REQUIRE(f.air.size() == 2);
    CHECK(f.air[1] ==
          cat({
              {0xb0, 0x00, 0x00, 0x00},
              m(kAp), m(kSta), m(kAp), {0x10, 0x00},
              {0x00, 0x00, 0x01, 0x00, 0x00, 0x00}
    }));

    f.fw(auth_resp(2, 0));
    CHECK(f.st.state() == Station::State::Assoc);
    f.tick(f.now);
    f.tick(f.now + Station::LATENCY_NS);
    REQUIRE(f.air.size() == 3);
    CHECK(f.air[2] == cat({
                          {0x00, 0x00, 0x00, 0x00},
                          m(kAp),
                          m(kSta),
                          m(kAp),
                          {0x20, 0x00},
                          {0x21, 0x04, 0x0a, 0x00},
                          ssid_el("cam"),
                          kRatesEl,
                          kExtRatesEl
    }));

    f.fw(assoc_resp(0, 0xc005));
    CHECK(f.st.state() == Station::State::Run);
    CHECK(f.ups == std::vector<uint64_t>{f.now});
    CHECK_FALSE(f.st.next_event_ns());

    // Data in both directions.
    f.fw(frame(0x08,
               0x02,
               kSta,
               kAp,
               kPeer,
               cat({
                   {0xaa, 0xaa, 0x03, 0, 0, 0, 0x08, 0x00},
                   {1, 2, 3}
    })));
    REQUIRE(f.eth.size() == 1);
    CHECK(f.eth[0] == cat({
                          m(kSta), m(kPeer), {0x08, 0x00, 1, 2, 3}
    }));

    f.st.send_ethernet(cat({
        m(kPeer), m(kSta), {0x08, 0x06, 9, 8}
    }));
    f.tick(f.now + Station::LATENCY_NS);
    REQUIRE(f.air.size() == 4);
    CHECK(f.air[3] == cat({
                          {0x08, 0x01, 0x00, 0x00},
                          m(kAp),
                          m(kSta),
                          m(kPeer),
                          {0x30, 0x00},
                          {0xaa, 0xaa, 0x03, 0, 0, 0, 0x08, 0x06, 9, 8}
    }));
}

TEST_CASE("station without a filter probes with an empty SSID and accepts any network") {
    Fixture f;
    f.tick(Station::RETRY_NS);
    f.tick(f.now + Station::LATENCY_NS);
    REQUIRE(f.air.size() == 1);
    CHECK(f.air[0] == cat({
                          {0x40, 0x00, 0x00, 0x00},
                          m(kBcast),
                          m(kSta),
                          m(kBcast),
                          {0x00, 0x00},
                          {0x00, 0x00},
                          kRatesEl,
                          kExtRatesEl
    }));
    f.fw(mgmt(ProbeResp, kSta, kAp, kAp, beacon_body("whatever")));
    CHECK(f.st.state() == Station::State::Auth);
}

TEST_CASE("station ignores a network with another SSID") {
    Fixture f("cam");
    f.tick(1);
    f.fw(beacon("other"));
    CHECK(f.st.state() == Station::State::Scan);
    CHECK_FALSE(f.st.bssid());
}

TEST_CASE("station does not join a network that needs a password") {
    Fixture f;
    f.fw(beacon("secret", 0x0411));
    f.fw(beacon("secret", 0x0411));
    CHECK(f.st.state() == Station::State::Scan);
}

TEST_CASE("station skips beacons that are too short or have no SSID") {
    Fixture f;
    f.fw(mgmt(Beacon, kBcast, kAp, kAp, Bytes(11, 0)));
    CHECK(f.st.state() == Station::State::Scan);
    f.fw(mgmt(Beacon,
              kBcast,
              kAp,
              kAp,
              cat({
                  Bytes(10, 0), {0x01, 0x04},
                    kRatesEl
    })));
    CHECK(f.st.state() == Station::State::Scan);
    // A truncated element does not run past the end.
    f.fw(mgmt(Beacon,
              kBcast,
              kAp,
              kAp,
              cat({
                  Bytes(10, 0), {0x01, 0x04},
                    {0x00, 0x09, 'a'}
    })));
    CHECK(f.st.state() == Station::State::Scan);
}

TEST_CASE("station finds the SSID after other elements") {
    Fixture f;
    f.fw(mgmt(Beacon,
              kBcast,
              kAp,
              kAp,
              cat({
                  Bytes(8, 0), {0x64, 0, 0x01, 0x04},
                    {0x03, 0x01, 0x06},
                    ssid_el("late")
    })));
    CHECK(f.st.state() == Station::State::Auth);
}

TEST_CASE("station ignores frames shorter than a header") {
    Fixture f;
    f.fw(Bytes(23, 0));
    f.fw(Bytes{});
    CHECK(f.st.state() == Station::State::Scan);
}

TEST_CASE("station ignores a beacon outside scan") {
    Fixture f;
    f.tick(Station::RETRY_NS);
    f.fw(beacon("cam", 0x0401, kAp));
    f.fw(beacon("cam", 0x0401, kOther));
    CHECK(f.st.state() == Station::State::Auth);
    CHECK(f.st.bssid() == kAp);
}

TEST_CASE("station honours a management frame only when addressed to it from the BSS") {
    Fixture f;
    f.fw(beacon("cam"));
    f.fw(mgmt(Auth, kOther, kAp, kAp, {0, 0, 2, 0, 0, 0})); // wrong addressee
    CHECK(f.st.state() == Station::State::Auth);
    f.fw(mgmt(Auth, kSta, kOther, kOther, {0, 0, 2, 0, 0, 0})); // wrong BSS
    CHECK(f.st.state() == Station::State::Auth);
    f.fw(auth_resp(1, 0)); // wrong sequence
    CHECK(f.st.state() == Station::State::Auth);
    f.fw(auth_resp(2, 0));
    CHECK(f.st.state() == Station::State::Assoc);
}

TEST_CASE("a refused authentication retries after a second") {
    Fixture f;
    f.tick(10 * kMs);
    f.fw(beacon("cam"));
    f.tick(f.now + 50 * kMs);
    f.tick(f.now + Station::LATENCY_NS);
    f.air.clear();
    uint64_t t = f.now;
    f.fw(auth_resp(2, 1));
    CHECK(f.st.state() == Station::State::Auth);
    CHECK(f.st.next_event_ns() == t + Station::RETRY_NS);
    f.tick(t + Station::RETRY_NS);
    f.tick(f.now + Station::LATENCY_NS);
    REQUIRE(f.air.size() == 1);
    CHECK(f.air[0][0] == 0xb0);
}

TEST_CASE("a refused association goes back to authentication") {
    Fixture f;
    f.tick(Station::RETRY_NS);
    f.tick(f.now + Station::LATENCY_NS); // the probe request is out
    f.fw(beacon("cam"));
    f.fw(auth_resp(2, 0));
    uint64_t t = f.now;
    f.fw(assoc_resp(17, 1));
    CHECK(f.st.state() == Station::State::Auth);
    CHECK(f.st.next_event_ns() == t + Station::RETRY_NS);
    CHECK(f.ups.empty());
}

TEST_CASE("association response outside assoc is ignored") {
    Fixture f;
    f.tick(Station::RETRY_NS);
    f.fw(beacon("cam"));
    f.fw(assoc_resp(0, 1));
    CHECK(f.st.state() == Station::State::Auth);
    CHECK(f.ups.empty());
}

TEST_CASE("deauthentication and disassociation return to auth with a retry") {
    for (uint8_t subtype : {Deauth, Disassoc}) {
        Fixture f;
        f.join();
        f.fw(mgmt(subtype, kSta, kAp, kAp, {0x03, 0x00}));
        CHECK(f.st.state() == Station::State::Auth);
        CHECK(f.st.next_event_ns() == f.now + Station::RETRY_NS);
    }
}

TEST_CASE("data frames are used only in run, FromDS set and ToDS clear") {
    Fixture     f;
    const Bytes body = cat({
        {0xaa, 0xaa, 0x03, 0, 0, 0, 0x08, 0x00},
        {7}
    });
    f.tick(Station::RETRY_NS);
    f.fw(beacon("cam"));
    f.fw(frame(0x08, 0x02, kSta, kAp, kPeer, body)); // not yet in run
    CHECK(f.eth.empty());
    f.join("cam");
    f.fw(frame(0x08, 0x01, kSta, kAp, kPeer, body)); // ToDS only
    f.fw(frame(0x08, 0x03, kSta, kAp, kPeer, body)); // both
    f.fw(frame(0x08, 0x00, kSta, kAp, kPeer, body)); // neither
    CHECK(f.eth.empty());
    f.fw(frame(0x08, 0x02, kSta, kAp, kPeer, body));
    CHECK(f.eth.size() == 1);
}

TEST_CASE("QoS data frames have a 26-byte header") {
    Fixture f;
    f.join();
    Bytes qos = frame(0x88, 0x02, kSta, kAp, kPeer, {});
    qos.insert(qos.end(), {0x00, 0x00}); // QoS control
    qos.insert(qos.end(), {0xaa, 0xaa, 0x03, 0, 0, 0, 0x86, 0xdd, 0x42});
    f.fw(qos);
    REQUIRE(f.eth.size() == 1);
    CHECK(f.eth[0] == cat({
                          m(kSta), m(kPeer), {0x86, 0xdd, 0x42}
    }));
}

TEST_CASE("data frames with a bad or missing LLC/SNAP header are dropped") {
    Fixture f;
    f.join();
    f.fw(frame(0x08, 0x02, kSta, kAp, kPeer, {0xaa, 0xaa, 0x03, 0, 0, 1, 0x08, 0x00}));
    f.fw(frame(0x08, 0x02, kSta, kAp, kPeer, {0xaa, 0xaa, 0x03, 0, 0, 0, 0x08})); // no ethertype
    f.fw(frame(0x08, 0x02, kSta, kAp, kPeer, {}));
    CHECK(f.eth.empty());
}

TEST_CASE("send_ethernet needs run and a full Ethernet header") {
    Fixture     f;
    const Bytes eth = cat({
        m(kPeer), m(kSta), {0x08, 0x00, 1}
    });
    f.tick(1);
    f.st.send_ethernet(eth);
    f.tick(1 + Station::LATENCY_NS);
    CHECK(f.air.empty());
    f.join();
    f.st.send_ethernet(Bytes(13, 0));
    f.tick(f.now + Station::LATENCY_NS);
    CHECK(f.air.empty());
    f.st.send_ethernet(Bytes(14, 0));
    f.tick(f.now + Station::LATENCY_NS);
    REQUIRE(f.air.size() == 1);
    CHECK(f.air[0].size() == 24 + 6 + 2);
}

TEST_CASE("sequence counter wraps at 12 bits") {
    Fixture f;
    f.join();
    for (int i = 0; i < 4100; ++i) f.st.send_ethernet(Bytes(14, 0));
    f.tick(f.now + Station::LATENCY_NS);
    REQUIRE(f.air.size() == 4100);
    // join() sent 3 frames, so the next number is 3.
    auto seq = [&](int i) { return (f.air[i][22] | f.air[i][23] << 8) >> 4; };
    CHECK(seq(0) == 3);
    CHECK(seq(4092) == 4095);
    CHECK(seq(4093) == 0);
}

TEST_CASE("reset returns to scan with an empty outbox") {
    Fixture f;
    f.join();
    f.st.send_ethernet(Bytes(14, 0));
    f.st.reset();
    CHECK(f.st.state() == Station::State::Scan);
    CHECK_FALSE(f.st.bssid());
    CHECK(f.st.next_event_ns() == f.now + Station::RETRY_NS);
    f.tick(f.now + Station::LATENCY_NS);
    CHECK(f.air.empty());
}

TEST_CASE("next_event_ns is the earlier of the next action and the outbox") {
    Fixture f;
    f.tick(Station::RETRY_NS); // queues a probe request due in 200 us
    CHECK(f.st.next_event_ns() == Station::RETRY_NS + Station::LATENCY_NS);
    f.tick(Station::RETRY_NS + Station::LATENCY_NS);
    CHECK(f.st.next_event_ns() == 2 * Station::RETRY_NS);
}

TEST_CASE("the authentication request waits 50 ms after the beacon") {
    Fixture f;
    f.tick(Station::RETRY_NS);
    f.fw(beacon("cam"));
    uint64_t t = f.now;
    f.tick(t + 50 * kMs - 1);
    CHECK(f.st.next_event_ns() == t + 50 * kMs);
    CHECK(f.air.size() == 1); // only the probe request
    f.tick(t + 50 * kMs);
    f.tick(t + 50 * kMs + Station::LATENCY_NS);
    REQUIRE(f.air.size() == 2);
    CHECK(f.air[1][0] == 0xb0);
}

TEST_CASE("a reply is never delivered inside the firmware's own call") {
    Fixture f;
    f.tick(Station::RETRY_NS);
    f.tick(f.now + Station::LATENCY_NS);
    f.air.clear();
    f.fw(beacon("cam"));
    f.fw(auth_resp(2, 0)); // would be answered at once if it were synchronous
    f.tick(f.now);
    CHECK(f.air.empty());
    f.tick(f.now + Station::LATENCY_NS - 1);
    CHECK(f.air.empty());
    f.tick(f.now + 1);
    CHECK(f.air.size() == 1);
}
