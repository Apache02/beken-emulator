#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include "core/log.h"
#include "net/router/authenticator.h"
#include "net/router/crypto.h"
#include "wpa_supplicant_fake.h"

using namespace bk;

namespace {

using wpa_test::Bytes;
using Mac = Authenticator::Mac;

constexpr Mac kAp  = {0x02, 0x00, 0x00, 0x00, 0x00, 0x02};
constexpr Mac kSta = {0x22, 0xac, 0x01, 0x02, 0x03, 0x04};

const Bytes kRsnEl = {0x30, 0x14, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04, 0x01, 0x00, 0x00,
                      0x0f, 0xac, 0x04, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x02, 0x00, 0x00};

constexpr std::array<uint8_t, 16> kGtk = {
    0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf};

const std::array<uint8_t, 32> &pmk() {
    static const auto p = crypto::wpa_psk("34974033A", "NAXCLOW");
    return p;
}

struct Fixture {
    std::ostringstream   out;
    std::vector<Bytes>   sent;
    uint8_t              next_random = 0x10;
    Authenticator        auth;
    wpa_test::Supplicant sup;

    explicit Fixture(const std::array<uint8_t, 32> &sta_pmk = pmk())
        : auth(
              pmk(),
              kAp,
              kSta,
              kGtk,
              kRsnEl,
              [this](std::span<uint8_t> b) {
                  for (uint8_t &x : b) x = next_random++;
              },
              [this](Bytes f) { sent.push_back(std::move(f)); },
              (Logger::reset_for_tests(), Logger::configure(out, false), Logger::get("net.ap"))),
          sup{.pmk = sta_pmk, .aa = kAp, .spa = kSta, .snonce = {}, .rsn = kRsnEl, .ptk = {}} {
        for (size_t i = 0; i < 32; ++i) sup.snonce[i] = uint8_t(0xc0 + i);
    }

    wpa_test::Key last() {
        REQUIRE(!sent.empty());
        auto k = wpa_test::parse(sent.back());
        REQUIRE(k);
        return *k;
    }
};

std::array<uint8_t, 32> expected_anonce() {
    std::array<uint8_t, 32> a;
    for (size_t i = 0; i < 32; ++i) a[i] = uint8_t(0x10 + i);
    return a;
}

} // namespace

TEST_CASE("authenticator: a full handshake with the right password") {
    using namespace wpa_test;
    Fixture f;
    CHECK(f.auth.state() == Authenticator::State::Idle);
    CHECK(f.auth.next_event_ns() == std::nullopt);

    f.auth.start(1000);
    CHECK(f.auth.state() == Authenticator::State::SentM1);
    CHECK(f.auth.next_event_ns() == 1000 + Authenticator::kRetryNs);
    REQUIRE(f.sent.size() == 1);
    Key m1 = f.last();
    CHECK(f.sent[0].size() == kDataAt);
    CHECK(m1.version == 2);
    CHECK(m1.type == 3);
    CHECK(m1.length == kDataAt - 4);
    CHECK(m1.descriptor == 2);
    CHECK(m1.info == (kVersion2 | kPairwise | kAck));
    CHECK(m1.key_length == 16);
    CHECK(m1.replay == 1);
    CHECK(m1.nonce == expected_anonce());
    CHECK(m1.mic == std::array<uint8_t, 16>{});
    CHECK(m1.data.empty());

    f.auth.receive(f.sup.m2(m1), 2000);
    CHECK(f.auth.state() == Authenticator::State::SentM3);
    CHECK(f.auth.next_event_ns() == 2000 + Authenticator::kRetryNs);
    REQUIRE(f.sent.size() == 2);
    Key m3 = f.last();
    CHECK(m3.version == 2);
    CHECK(m3.type == 3);
    CHECK(m3.length == f.sent[1].size() - 4);
    CHECK(m3.descriptor == 2);
    CHECK(m3.info == (kVersion2 | kPairwise | kInstall | kAck | kMic | kSecure | kEncrypted));
    CHECK(m3.key_length == 16);
    CHECK(m3.replay == 2);
    CHECK(m3.nonce == expected_anonce());
    CHECK(m3.iv == std::array<uint8_t, 16>{});
    CHECK(m3.rsc == std::array<uint8_t, 8>{});
    CHECK(m3.mic == mic(f.sup.kck(), f.sent[1]));

    // RSN element, GTK KDE (key ID 1, Tx 0), then dd 00 padding to a multiple of 8: 22 + 24 + 2 = 48.
    auto plain = crypto::aes_key_unwrap(f.sup.kek(), m3.data);
    REQUIRE(plain);
    Bytes expect = kRsnEl;
    Bytes kde    = {0xdd, 0x16, 0x00, 0x0f, 0xac, 0x01, 0x01, 0x00};
    expect.insert(expect.end(), kde.begin(), kde.end());
    expect.insert(expect.end(), kGtk.begin(), kGtk.end());
    expect.insert(expect.end(), {0xdd, 0x00});
    CHECK(*plain == expect);

    f.auth.receive(f.sup.m4(m3), 3000);
    CHECK(f.auth.state() == Authenticator::State::Done);
    CHECK(f.auth.next_event_ns() == std::nullopt);
    CHECK(f.sent.size() == 2);
    CHECK(std::equal(f.auth.tk().begin(), f.auth.tk().end(), f.sup.tk().begin()));

    // Nothing more happens once done.
    f.auth.tick(10 * Authenticator::kRetryNs);
    f.auth.receive(f.sup.m4(m3), 10 * Authenticator::kRetryNs);
    CHECK(f.sent.size() == 2);
    CHECK(f.auth.state() == Authenticator::State::Done);
}

TEST_CASE("authenticator: a wrong password fails after the tries") {
    using namespace wpa_test;
    Fixture f(crypto::wpa_psk("not the password", "NAXCLOW"));

    uint64_t t = 0;
    f.auth.start(t);
    for (unsigned n = 1; n <= Authenticator::kTries; ++n) {
        REQUIRE(f.sent.size() == n);
        Key m1 = f.last();
        CHECK(m1.info == (kVersion2 | kPairwise | kAck));
        // Each retry carries a new replay counter, as a supplicant drops one that does not increase.
        CHECK(m1.replay == n);
        CHECK(m1.nonce == expected_anonce());

        f.auth.receive(f.sup.m2(m1), t + 1000);
        CHECK(f.auth.state() == Authenticator::State::SentM1);
        CHECK(f.sent.size() == n); // a bad M2 is not answered

        CHECK(f.auth.next_event_ns() == t + Authenticator::kRetryNs);
        f.auth.tick(t + Authenticator::kRetryNs - 1);
        CHECK(f.sent.size() == n);
        t += Authenticator::kRetryNs;
        f.auth.tick(t);
    }
    CHECK(f.sent.size() == Authenticator::kTries);
    CHECK(f.auth.state() == Authenticator::State::Failed);
    CHECK(f.auth.next_event_ns() == std::nullopt);

    // A late M2 changes nothing.
    f.auth.receive(f.sup.m2(*parse(f.sent.back())), t + 1);
    f.auth.tick(t + 10 * Authenticator::kRetryNs);
    CHECK(f.sent.size() == Authenticator::kTries);
    CHECK(f.auth.state() == Authenticator::State::Failed);

    // A new start begins a fresh attempt; a right password then completes it.
    f.auth.start(t + 20 * Authenticator::kRetryNs);
    CHECK(f.auth.state() == Authenticator::State::SentM1);
    Key m1 = f.last();
    f.auth.receive(f.sup.m2(m1), t + 20 * Authenticator::kRetryNs);
    f.auth.receive(f.sup.m2(m1), t + 20 * Authenticator::kRetryNs);
    f.sup.pmk = pmk();
    f.auth.receive(f.sup.m2(m1), t + 20 * Authenticator::kRetryNs);
    REQUIRE(f.auth.state() == Authenticator::State::SentM3);
    f.auth.receive(f.sup.m4(f.last()), t + 20 * Authenticator::kRetryNs);
    CHECK(f.auth.state() == Authenticator::State::Done);
    CHECK(std::equal(f.auth.tk().begin(), f.auth.tk().end(), f.sup.tk().begin()));
}

TEST_CASE("authenticator: an unanswered M3 is retried, then the handshake fails") {
    using namespace wpa_test;
    Fixture f;
    f.auth.start(0);
    f.auth.receive(f.sup.m2(f.last()), 0);
    REQUIRE(f.auth.state() == Authenticator::State::SentM3);
    for (unsigned n = 1; n < Authenticator::kTries; ++n) {
        f.auth.tick(n * Authenticator::kRetryNs);
        REQUIRE(f.sent.size() == 2 + n);
        Key m3 = f.last();
        CHECK(m3.info == (kVersion2 | kPairwise | kInstall | kAck | kMic | kSecure | kEncrypted));
        CHECK(m3.replay == 2 + n);
        CHECK(m3.mic == mic(f.sup.kck(), f.sent.back()));
    }
    // An M4 answering an earlier M3 does not count.
    f.auth.receive(f.sup.m4(*parse(f.sent[2])), Authenticator::kTries * Authenticator::kRetryNs - 1);
    CHECK(f.auth.state() == Authenticator::State::SentM3);
    f.auth.tick(Authenticator::kTries * Authenticator::kRetryNs);
    CHECK(f.sent.size() == 1 + Authenticator::kTries);
    CHECK(f.auth.state() == Authenticator::State::Failed);
}

TEST_CASE("authenticator: replayed or out-of-order messages are ignored") {
    using namespace wpa_test;
    Fixture f;
    f.auth.start(0);
    Key m1 = f.last();

    // M4 before M3: valid MIC under the PTK the station would hold, but M2 has not been seen.
    Bytes m2 = f.sup.m2(m1);
    f.auth.receive(f.sup.m4(m1), 10);
    CHECK(f.auth.state() == Authenticator::State::SentM1);
    CHECK(f.sent.size() == 1);

    // M2 with replay counter 0 (M1 had 1), correctly signed.
    Key stale    = m1;
    stale.replay = 0;
    f.auth.receive(f.sup.m2(stale), 20);
    CHECK(f.auth.state() == Authenticator::State::SentM1);
    CHECK(f.sent.size() == 1);

    // Junk: too short, not a key frame, an M2 without the MIC bit.
    f.auth.receive(Bytes(10, 0), 30);
    Bytes not_key = m2;
    not_key[1]    = 0; // EAP packet
    f.auth.receive(not_key, 30);
    Bytes no_mic = build(kVersion2 | kPairwise, 1, f.sup.snonce, kRsnEl);
    f.auth.receive(no_mic, 30);
    Bytes cut = m2;
    cut.resize(cut.size() - 1); // key data runs past the end
    f.auth.receive(cut, 30);
    CHECK(f.auth.state() == Authenticator::State::SentM1);
    CHECK(f.sent.size() == 1);

    // The real M2 still works; a second copy of it after M3 changes nothing.
    f.auth.receive(m2, 40);
    REQUIRE(f.auth.state() == Authenticator::State::SentM3);
    f.auth.receive(m2, 50);
    CHECK(f.auth.state() == Authenticator::State::SentM3);
    CHECK(f.sent.size() == 2);
    Key m3 = f.last();

    // M4 with M1's replay counter.
    f.auth.receive(f.sup.m4(m1), 60);
    CHECK(f.auth.state() == Authenticator::State::SentM3);
    // M4 with a bad MIC.
    Bytes bad = f.sup.m4(m3);
    bad[kMicAt] ^= 1;
    f.auth.receive(bad, 70);
    CHECK(f.auth.state() == Authenticator::State::SentM3);

    f.auth.receive(f.sup.m4(m3), 80);
    CHECK(f.auth.state() == Authenticator::State::Done);
}
