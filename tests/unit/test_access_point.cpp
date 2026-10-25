#include <doctest/doctest.h>

#include <initializer_list>
#include <optional>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "core/log.h"
#include "net/router/access_point.h"
#include "net/router/authenticator.h"
#include "net/router/crypto.h"
#include "wpa_supplicant_fake.h"

using namespace bk;
using Bytes = std::vector<uint8_t>;
using Mac   = AccessPoint::Mac;

namespace {

constexpr Mac kBss   = {0x02, 0x00, 0x00, 0x00, 0x00, 0x02};
constexpr Mac kSta   = {0x22, 0xac, 0x01, 0x02, 0x03, 0x04};
constexpr Mac kOther = {0x22, 0xac, 0x09, 0x09, 0x09, 0x09};
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

Bytes le64(uint64_t x) {
    Bytes r;
    for (int i = 0; i < 8; ++i) r.push_back(uint8_t(x >> (8 * i)));
    return r;
}

const Bytes kRatesEl    = {0x01, 0x08, 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24};
const Bytes kExtRatesEl = {0x32, 0x04, 0x30, 0x48, 0x60, 0x6c};
const Bytes kTimEl      = {0x05, 0x04, 0x00, 0x01, 0x00, 0x00};
const Bytes kRsnEl      = {0x30, 0x14, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04, 0x01, 0x00, 0x00,
                           0x0f, 0xac, 0x04, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x02, 0x00, 0x00};

// Everything of a beacon or probe response body after the timestamp.
Bytes open_tail(const std::string &ssid, uint8_t channel) {
    return cat({
        {0x64, 0x00, 0x21, 0x04},
        ssid_el(ssid), kRatesEl, {0x03, 0x01, channel},
        kTimEl, kExtRatesEl
    });
}

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

Bytes probe_req(const std::string &ssid, const Mac &from = kSta) {
    return mgmt(ProbeReq, kBcast, from, kBcast, cat({ssid_el(ssid), kRatesEl, kExtRatesEl}));
}

Bytes auth_req(const Mac &from = kSta, uint16_t alg = 0, uint16_t seq = 1) {
    return mgmt(Auth, kBss, from, kBss, {uint8_t(alg), uint8_t(alg >> 8), uint8_t(seq), uint8_t(seq >> 8), 0, 0});
}

Bytes assoc_req(const Mac &from = kSta) {
    return mgmt(AssocReq,
                kBss,
                from,
                kBss,
                cat({
                    {0x21, 0x04, 0x0a, 0x00},
                    ssid_el("cam"), kRatesEl, kExtRatesEl
    }));
}

// The header of a frame from the access point, with its sequence number.
Bytes ap_header(uint8_t fc0, uint8_t fc1, const Mac &a1, const Mac &a3, unsigned seq) {
    return cat({
        {fc0, fc1, 0, 0},
        m(a1), m(kBss), m(a3), {uint8_t(seq << 4), uint8_t(seq >> 4)}
    });
}

const Bytes kIpPayload = {0xaa, 0xaa, 0x03, 0, 0, 0, 0x08, 0x00, 1, 2, 3};
const Bytes kEapolLlc  = {0xaa, 0xaa, 0x03, 0, 0, 0, 0x88, 0x8e};

const ApConfig kWpa = {.ssid = "cam", .password = "secret123"};

// An EAPOL frame from the firmware, in a data frame to the access point.
Bytes eapol_in(const Bytes &eapol, const Mac &from = kSta) {
    return frame(0x08, 0x01, kBss, from, kBss, cat({kEapolLlc, eapol}));
}

// The EAPOL-Key frame in a data frame from the access point to kSta, if it is one.
std::optional<wpa_test::Key> eapol_out(const Bytes &f) {
    const Bytes head = cat({
        {0x08, 0x02, 0, 0},
        m(kSta), m(kBss), m(kBss)
    });
    if (f.size() < 32 || !std::equal(head.begin(), head.end(), f.begin()) ||
        !std::equal(kEapolLlc.begin(), kEapolLlc.end(), f.begin() + 24))
        return std::nullopt;
    return wpa_test::parse(Bytes(f.begin() + 32, f.end()));
}

struct Fixture {
    std::ostringstream                           out;
    std::vector<Bytes>                           air; // frames handed to the MAC
    std::vector<Bytes>                           eth; // frames handed to the router
    std::vector<std::tuple<bool, Mac, uint64_t>> links;
    std::optional<unsigned>                      tuned;
    uint8_t                                      next_random = 0x10;
    AccessPoint                                  ap;
    uint64_t                                     now = 0;
    // The firmware's side of the handshake, with the password of the network.
    wpa_test::Supplicant sup;

    explicit Fixture(ApConfig config = {.ssid = "cam", .password = std::nullopt})
        : ap(
              config,
              [this] { return tuned; },
              (Logger::reset_for_tests(), Logger::configure(out, false), Logger::get("net.ap")),
              [this](std::span<uint8_t> b) {
                  for (uint8_t &x : b) x = next_random++;
              }),
          sup{.pmk    = crypto::wpa_psk(config.password.value_or("unused password"), config.ssid),
              .aa     = kBss,
              .spa    = kSta,
              .snonce = {},
              .rsn    = kRsnEl,
              .ptk    = {}} {
        for (size_t i = 0; i < 32; ++i) sup.snonce[i] = uint8_t(0xc0 + i);
        ap.attach([this](Bytes f) { air.push_back(std::move(f)); });
        ap.set_ethernet_in([this](Bytes f) { eth.push_back(std::move(f)); });
        ap.set_link_change([this](bool up, const Mac &sta, uint64_t t) { links.emplace_back(up, sta, t); });
    }

    void tick(uint64_t t) {
        now = t;
        ap.tick(t);
    }
    void fw(const Bytes &f) { ap.transmit(f); }
    // Lets the replies out, then drops the beacons among the delivered frames.
    void settle() {
        tick(now + AccessPoint::kLatencyNs);
        std::erase_if(air, [](const Bytes &f) { return f[0] == 0x80; });
    }

    // Authenticates and associates kSta, then returns with the management frames
    // cleared from the air; with WPA2 message 1 of the handshake stays.
    void join(const Mac &sta = kSta) {
        fw(auth_req(sta));
        settle();
        fw(assoc_req(sta));
        settle();
        REQUIRE(ap.station() == sta);
        std::erase_if(air, [](const Bytes &f) { return (f[0] & 0x0c) == 0; });
    }

    // Joins and completes the 4-way handshake, then returns with the air cleared.
    void handshake() {
        join();
        REQUIRE(air.size() == 1);
        auto m1 = eapol_out(air[0]);
        REQUIRE(m1);
        fw(eapol_in(sup.m2(*m1)));
        settle();
        REQUIRE(air.size() == 2);
        auto m3 = eapol_out(air[1]);
        REQUIRE(m3);
        fw(eapol_in(sup.m4(*m3)));
        air.clear();
    }
};

} // namespace

TEST_CASE("access point beacons every 100 TU with the open elements") {
    Fixture f;
    CHECK(f.ap.next_event_ns() == AccessPoint::kBeaconIntervalNs);
    f.tick(AccessPoint::kBeaconIntervalNs - 1);
    CHECK(f.air.empty());
    f.tick(AccessPoint::kBeaconIntervalNs);
    REQUIRE(f.air.size() == 1);
    CHECK(f.air[0] == cat({ap_header(0x80, 0x00, kBcast, kBss, 0), le64(102'400), open_tail("cam", 1)}));
    CHECK(f.ap.next_event_ns() == 2 * AccessPoint::kBeaconIntervalNs);

    // A late tick sends one beacon and keeps the 100 TU grid.
    f.tick(3 * AccessPoint::kBeaconIntervalNs + 5);
    REQUIRE(f.air.size() == 2);
    CHECK(Bytes(f.air[1].begin() + 24, f.air[1].begin() + 32) == le64((3 * AccessPoint::kBeaconIntervalNs + 5) / 1000));
    CHECK(Bytes(f.air[1].begin() + 32, f.air[1].end()) == open_tail("cam", 1));
    CHECK(f.ap.next_event_ns() == 4 * AccessPoint::kBeaconIntervalNs);
}

TEST_CASE("access point with a password advertises RSN and runs the handshake after association") {
    using namespace wpa_test;
    ApConfig config = kWpa;
    config.channel  = 11;
    Fixture f(config);
    f.tick(AccessPoint::kBeaconIntervalNs);
    REQUIRE(f.air.size() == 1);
    CHECK(Bytes(f.air[0].begin() + 32, f.air[0].end()) ==
          cat({
              {0x64, 0x00, 0x31, 0x04},
              ssid_el("cam"), kRatesEl, {0x03, 0x01, 11},
              kTimEl, kExtRatesEl, kRsnEl
    }));
    f.air.clear();

    // The association is answered, then message 1 follows; the link is not up yet.
    f.join();
    CHECK(f.links.empty());
    REQUIRE(f.air.size() == 1);
    auto m1 = eapol_out(f.air[0]);
    REQUIRE(m1);
    CHECK(m1->info == (kVersion2 | kPairwise | kAck));
    CHECK(m1->replay == 1);
    // The GTK takes the first 16 random bytes, the ANonce the next 32.
    std::array<uint8_t, 32> anonce;
    for (size_t i = 0; i < 32; ++i) anonce[i] = uint8_t(0x20 + i);
    CHECK(m1->nonce == anonce);

    f.fw(eapol_in(f.sup.m2(*m1)));
    f.settle();
    REQUIRE(f.air.size() == 2);
    auto m3 = eapol_out(f.air[1]);
    REQUIRE(m3);
    CHECK(m3->info == (kVersion2 | kPairwise | kInstall | kAck | kMic | kSecure | kEncrypted));
    auto plain = crypto::aes_key_unwrap(f.sup.kek(), m3->data);
    REQUIRE(plain);
    Bytes gtk;
    for (uint8_t i = 0; i < 16; ++i) gtk.push_back(uint8_t(0x10 + i));
    CHECK(*plain == cat({
                        kRsnEl, {0xdd, 0x16, 0x00, 0x0f, 0xac, 0x01, 0x01, 0x00},
                         gtk, {0xdd, 0x00}
    }));
    CHECK(f.links.empty());

    f.fw(eapol_in(f.sup.m4(*m3)));
    CHECK(f.links == std::vector<std::tuple<bool, Mac, uint64_t>>{
                         {true, kSta, f.now}
    });
    // EAPOL never reaches the router.
    CHECK(f.eth.empty());
    f.settle();
    CHECK(f.air.size() == 2);

    // Now data flows.
    f.fw(frame(0x08, 0x01, kBss, kSta, kPeer, kIpPayload));
    CHECK(f.eth.size() == 1);
}

TEST_CASE("access point drops data before the handshake is done") {
    Fixture f(kWpa);
    f.tick(1 * kMs);
    const Bytes eth = cat({
        m(kSta), m(kPeer), {0x08, 0x00, 1}
    });

    f.join();
    REQUIRE(f.air.size() == 1);
    auto m1 = eapol_out(f.air[0]);
    REQUIRE(m1);
    f.air.clear();
    f.fw(frame(0x08, 0x01, kBss, kSta, kPeer, kIpPayload));
    f.ap.send_ethernet(eth);
    f.settle();
    CHECK(f.eth.empty());
    CHECK(f.air.empty());

    // Halfway, after message 3, still nothing.
    f.fw(eapol_in(f.sup.m2(*m1)));
    f.settle();
    REQUIRE(f.air.size() == 1);
    auto m3 = eapol_out(f.air[0]);
    REQUIRE(m3);
    f.air.clear();
    f.fw(frame(0x08, 0x01, kBss, kSta, kPeer, kIpPayload));
    f.ap.send_ethernet(eth);
    f.settle();
    CHECK(f.eth.empty());
    CHECK(f.air.empty());
    CHECK(f.links.empty());

    // EAPOL from another station or from the right one before an association goes nowhere.
    f.fw(eapol_in(f.sup.m4(*m3), kOther));
    CHECK(f.links.empty());

    f.fw(eapol_in(f.sup.m4(*m3)));
    REQUIRE(f.links.size() == 1);
    f.fw(frame(0x08, 0x01, kBss, kSta, kPeer, kIpPayload));
    f.ap.send_ethernet(eth);
    f.settle();
    CHECK(f.eth.size() == 1);
    CHECK(f.air.size() == 1);
}

TEST_CASE("access point strips the CCMP header of protected frames") {
    const Bytes ccmp = {0x01, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00}; // PN 1, ExtIV, key id 0
    const Bytes mic  = {0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8};
    {
        Fixture f(kWpa);
        f.tick(1 * kMs);
        f.handshake();
        f.fw(frame(0x08, 0x41, kBss, kSta, kPeer, cat({ccmp, kIpPayload, mic})));
        REQUIRE(f.eth.size() == 1);
        // What follows the IP packet stays; the IP layer cuts it as link padding.
        CHECK(f.eth[0] == cat({
                              m(kPeer), m(kSta), {0x08, 0x00, 1, 2, 3},
                                mic
        }));

        Bytes qos = frame(0x88, 0x41, kBss, kSta, kBcast, {0x00, 0x00});
        qos.insert(qos.end(), ccmp.begin(), ccmp.end());
        qos.insert(qos.end(), {0xaa, 0xaa, 0x03, 0, 0, 0, 0x08, 0x06, 0x42});
        f.fw(qos);
        REQUIRE(f.eth.size() == 2);
        CHECK(f.eth[1] == cat({
                              m(kBcast), m(kSta), {0x08, 0x06, 0x42}
        }));

        // Too short for a CCMP header and an LLC/SNAP header.
        f.fw(frame(0x08,
                   0x41,
                   kBss,
                   kSta,
                   kPeer,
                   cat({
                       ccmp, {0xaa, 0xaa, 0x03}
        })));
        CHECK(f.eth.size() == 2);
    }
    {
        // A protected EAPOL frame reaches the authenticator too.
        Fixture f(kWpa);
        f.tick(1 * kMs);
        f.join();
        auto m1 = eapol_out(f.air[0]);
        REQUIRE(m1);
        f.fw(eapol_in(f.sup.m2(*m1)));
        f.settle();
        REQUIRE(f.air.size() == 2);
        auto m3 = eapol_out(f.air[1]);
        REQUIRE(m3);
        f.fw(frame(0x08, 0x41, kBss, kSta, kBss, cat({ccmp, kEapolLlc, f.sup.m4(*m3), mic})));
        CHECK(f.links.size() == 1);
    }
    {
        // On an open network a protected frame makes no sense.
        Fixture f;
        f.tick(1 * kMs);
        f.join();
        f.fw(frame(0x08, 0x41, kBss, kSta, kPeer, cat({ccmp, kIpPayload})));
        CHECK(f.eth.empty());
    }
}

TEST_CASE("access point sends data to the station plain after the handshake") {
    // Both firmwares take it so; see AccessPoint::send_ethernet.
    Fixture f(kWpa);
    f.tick(1 * kMs);
    f.handshake();
    f.ap.send_ethernet(cat({
        m(kSta), m(kPeer), {0x08, 0x00, 9, 8}
    }));
    f.settle();
    REQUIRE(f.air.size() == 1);
    CHECK(f.air[0] == cat({
                          ap_header(0x08, 0x02, kSta, kPeer, 4), {0xaa, 0xaa, 0x03, 0, 0, 0, 0x08, 0x00, 9, 8}
    }));
}

TEST_CASE("access point deauthenticates when the handshake fails") {
    Fixture f(kWpa);
    f.sup.pmk = crypto::wpa_psk("wrong password", "cam");
    f.tick(1 * kMs);

    for (int attempt = 1; attempt <= 2; ++attempt) {
        f.join();
        const uint64_t t0 = f.now - AccessPoint::kLatencyNs; // when the association request came
        for (unsigned n = 1; n <= Authenticator::kTries; ++n) {
            REQUIRE(f.air.size() == 1);
            auto m1 = eapol_out(f.air[0]);
            REQUIRE(m1);
            CHECK(m1->replay == n);
            f.fw(eapol_in(f.sup.m2(*m1)));
            f.air.clear();
            f.tick(t0 + n * Authenticator::kRetryNs);
            f.settle();
        }
        // Reason 15: 4-way handshake timeout.
        REQUIRE(f.air.size() == 1);
        Bytes head = ap_header(0xc0, 0x00, kSta, kBss, 0);
        head.resize(22); // without the sequence number
        CHECK(Bytes(f.air[0].begin(), f.air[0].begin() + 22) == head);
        CHECK(Bytes(f.air[0].begin() + 24, f.air[0].end()) == Bytes{0x0f, 0x00});
        f.air.clear();
        CHECK_FALSE(f.ap.station());
        CHECK(f.links.empty());
        CHECK(f.ap.next_event_ns() > f.now);
        CHECK(*f.ap.next_event_ns() <= f.now + AccessPoint::kBeaconIntervalNs);

        // Nothing of the old association is left: no data, no late handshake message.
        f.fw(frame(0x08, 0x01, kBss, kSta, kPeer, kIpPayload));
        CHECK(f.eth.empty());
    }
}

TEST_CASE("access point is heard only on its channel") {
    Fixture f;
    f.tuned = 6;
    f.tick(AccessPoint::kBeaconIntervalNs);
    CHECK(f.air.empty());
    f.fw(probe_req(""));
    f.settle();
    f.tick(f.now + AccessPoint::kLatencyNs);
    CHECK(f.air.empty());

    f.tuned = 1;
    f.tick(2 * AccessPoint::kBeaconIntervalNs);
    CHECK(f.air.size() == 1);
    f.tuned.reset();
    f.tick(3 * AccessPoint::kBeaconIntervalNs);
    CHECK(f.air.size() == 2);
}

TEST_CASE("access point answers a wildcard or matching probe request only") {
    Fixture f;
    f.tick(1 * kMs);
    f.fw(probe_req("other"));
    f.fw(probe_req("ca"));
    f.tick(f.now + AccessPoint::kLatencyNs);
    CHECK(f.air.empty());

    f.fw(probe_req(""));
    f.tick(f.now + AccessPoint::kLatencyNs - 1);
    CHECK(f.air.empty()); // never inside transmit, and only after the latency
    f.tick(f.now + 1);
    REQUIRE(f.air.size() == 1);
    CHECK(f.air[0] == cat({ap_header(0x50, 0x00, kSta, kBss, 0), le64(f.now / 1000), open_tail("cam", 1)}));

    f.fw(probe_req("cam", kOther));
    f.tick(f.now + AccessPoint::kLatencyNs);
    REQUIRE(f.air.size() == 2);
    CHECK(f.air[1] == cat({ap_header(0x50, 0x00, kOther, kBss, 1), le64(f.now / 1000), open_tail("cam", 1)}));

    // No SSID element at all, or one that runs past the end.
    f.fw(mgmt(ProbeReq, kBcast, kSta, kBcast, kRatesEl));
    f.fw(mgmt(ProbeReq, kBcast, kSta, kBcast, {0x00, 0x09, 'c'}));
    f.tick(f.now + AccessPoint::kLatencyNs);
    CHECK(f.air.size() == 2);
}

TEST_CASE("access point walks auth and association and reports the link") {
    Fixture f;
    f.tick(1 * kMs);
    CHECK_FALSE(f.ap.station());

    f.fw(auth_req(kSta, 1)); // shared key is not offered
    f.tick(f.now + AccessPoint::kLatencyNs);
    REQUIRE(f.air.size() == 1);
    CHECK(f.air[0] == cat({
                          ap_header(0xb0, 0x00, kSta, kBss, 0), {0x01, 0x00, 0x02, 0x00, 0x0d, 0x00}
    }));
    f.air.clear();

    // Association before authentication is not answered.
    f.fw(assoc_req());
    f.tick(f.now + AccessPoint::kLatencyNs);
    CHECK(f.air.empty());
    CHECK_FALSE(f.ap.station());

    f.fw(auth_req());
    f.tick(f.now);
    CHECK(f.air.empty());
    f.tick(f.now + AccessPoint::kLatencyNs);
    REQUIRE(f.air.size() == 1);
    CHECK(f.air[0] == cat({
                          ap_header(0xb0, 0x00, kSta, kBss, 1), {0x00, 0x00, 0x02, 0x00, 0x00, 0x00}
    }));
    CHECK_FALSE(f.ap.station());

    uint64_t t = f.now;
    f.fw(assoc_req());
    CHECK(f.ap.station() == kSta);
    CHECK(f.links == std::vector<std::tuple<bool, Mac, uint64_t>>{
                         {true, kSta, t}
    });
    CHECK(f.air.size() == 1);
    f.tick(f.now + AccessPoint::kLatencyNs);
    REQUIRE(f.air.size() == 2);
    CHECK(f.air[1] ==
          cat({
              ap_header(0x10, 0x00, kSta, kBss, 2), {0x21, 0x04, 0x00, 0x00, 0x01, 0xc0},
                   kRatesEl, kExtRatesEl
    }));
}

TEST_CASE("access point bridges data both ways, QoS header too") {
    Fixture f;
    f.tick(1 * kMs);

    // Nothing is bridged before an association.
    f.fw(frame(0x08, 0x01, kBss, kSta, kPeer, kIpPayload));
    f.ap.send_ethernet(cat({
        m(kSta), m(kPeer), {0x08, 0x00, 1}
    }));
    f.tick(f.now + AccessPoint::kLatencyNs);
    CHECK(f.eth.empty());
    CHECK(f.air.empty());

    f.join();
    f.fw(frame(0x08, 0x01, kBss, kSta, kPeer, kIpPayload));
    REQUIRE(f.eth.size() == 1);
    CHECK(f.eth[0] == cat({
                          m(kPeer), m(kSta), {0x08, 0x00, 1, 2, 3}
    }));

    Bytes qos = frame(0x88, 0x01, kBss, kSta, kBcast, {0x00, 0x00}); // QoS control
    qos.insert(qos.end(), {0xaa, 0xaa, 0x03, 0, 0, 0, 0x08, 0x06, 0x42});
    f.fw(qos);
    REQUIRE(f.eth.size() == 2);
    CHECK(f.eth[1] == cat({
                          m(kBcast), m(kSta), {0x08, 0x06, 0x42}
    }));

    // Dropped: FromDS, another BSS, another station, a null frame, no LLC/SNAP.
    f.fw(frame(0x08, 0x02, kBss, kSta, kPeer, kIpPayload));
    f.fw(frame(0x08, 0x01, kOther, kSta, kPeer, kIpPayload));
    f.fw(frame(0x08, 0x01, kBss, kOther, kPeer, kIpPayload));
    f.fw(frame(0x48, 0x01, kBss, kSta, kPeer, {}));
    f.fw(frame(0x08, 0x01, kBss, kSta, kPeer, {0xaa, 0xaa, 0x03, 0, 0, 1, 0x08, 0x00}));
    f.fw(frame(0x08, 0x01, kBss, kSta, kPeer, {0xaa, 0xaa, 0x03, 0, 0, 0, 0x08}));
    CHECK(f.eth.size() == 2);

    f.ap.send_ethernet(Bytes(13, 0));
    f.ap.send_ethernet(cat({
        m(kSta), m(kPeer), {0x08, 0x00, 9, 8}
    }));
    CHECK(f.air.empty());
    f.tick(f.now + AccessPoint::kLatencyNs);
    REQUIRE(f.air.size() == 1);
    CHECK(f.air[0] == cat({
                          ap_header(0x08, 0x02, kSta, kPeer, 2), {0xaa, 0xaa, 0x03, 0, 0, 0, 0x08, 0x00, 9, 8}
    }));
}

TEST_CASE("access point forgets the station on deauth, disassoc or a new auth") {
    const Bytes eth = cat({
        m(kSta), m(kPeer), {0x08, 0x00, 1}
    });
    for (const Bytes &bye :
         {mgmt(Deauth, kBss, kSta, kBss, {0x03, 0x00}), mgmt(Disassoc, kBss, kSta, kBss, {0x08, 0x00}), auth_req()}) {
        Fixture f;
        f.tick(1 * kMs);
        f.join();
        f.links.clear();

        // From someone else it changes nothing.
        Bytes foreign = bye;
        std::copy(kOther.begin(), kOther.end(), foreign.begin() + 10);
        f.fw(foreign);
        CHECK(f.ap.station() == kSta);
        CHECK(f.links.empty());

        f.fw(bye);
        CHECK_FALSE(f.ap.station());
        CHECK(f.links == std::vector<std::tuple<bool, Mac, uint64_t>>{
                             {false, kSta, f.now}
        });
        f.fw(frame(0x08, 0x01, kBss, kSta, kPeer, kIpPayload));
        CHECK(f.eth.empty());
        f.settle();
        f.air.clear();
        f.ap.send_ethernet(eth);
        f.settle();
        CHECK(f.air.empty());

        // The station joins again and the link comes back.
        f.join();
        CHECK(f.links.size() == 2);
        CHECK(std::get<0>(f.links[1]));
    }
}

TEST_CASE("access point reset drops the association and the outbox") {
    Fixture f;
    f.tick(1 * kMs);
    f.join();
    f.ap.send_ethernet(cat({
        m(kSta), m(kPeer), {0x08, 0x00, 1}
    }));
    f.ap.reset();
    CHECK_FALSE(f.ap.station());
    CHECK(f.links.size() == 2);
    CHECK_FALSE(std::get<0>(f.links[1]));
    CHECK(f.ap.next_event_ns() == f.now + AccessPoint::kBeaconIntervalNs);
    f.tick(f.now + AccessPoint::kLatencyNs);
    CHECK(f.air.empty());
}

TEST_CASE("channel_from_rc maps 2.4 GHz channels") {
    CHECK(channel_from_rc(12u << 25) == 1u);
    CHECK(channel_from_rc(37u << 25) == 6u);
    CHECK(channel_from_rc(72u << 25) == 13u);
    CHECK(channel_from_rc(84u << 25) == 14u);
    CHECK(channel_from_rc((12u << 25) | 0x01ff'ffff) == 1u); // the low bits are other fields
    CHECK_FALSE(channel_from_rc(0));
    CHECK_FALSE(channel_from_rc(13u << 25));
    CHECK_FALSE(channel_from_rc(89u << 25));
}
