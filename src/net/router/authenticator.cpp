#include "net/router/authenticator.h"

#include <algorithm>
#include <format>
#include <string>

#include "net/router/crypto.h"
#include "net/router/mac_text.h"

namespace bk {

namespace {

// EAPOL header, then the EAPOL-Key body of IEEE 802.11 (RSN key descriptor).
constexpr uint8_t kEapolVersion = 2, kTypeKey = 3, kDescriptorRsn = 2;

// Key info bits.
constexpr uint16_t kVersionMask = 0x0007, kVersion2 = 0x0002, kPairwise = 0x0008, kInstall = 0x0040, kAck = 0x0080,
                   kMic = 0x0100, kSecure = 0x0200, kRequest = 0x0800, kEncrypted = 0x1000;

constexpr uint16_t kCcmpKeyLength = 16;

// Offsets in the frame; the key data follows the fixed fields.
constexpr size_t kInfoAt = 5, kKeyLenAt = 7, kReplayAt = 9, kNonceAt = 17, kMicAt = 81, kDataLenAt = 97, kDataAt = 99;

// GTK KDE header: OUI 00:0f:ac, data type 1, key ID 1, Tx 0, reserved.
constexpr uint8_t kGtkKde[] = {0xdd, 0x16, 0x00, 0x0f, 0xac, 0x01, 0x01, 0x00};

uint64_t be(std::span<const uint8_t> b, size_t off, size_t n) {
    uint64_t x = 0;
    for (size_t i = 0; i < n; ++i) x = x << 8 | b[off + i];
    return x;
}

void put_be(std::vector<uint8_t> &b, size_t off, uint64_t x, size_t n) {
    for (size_t i = 0; i < n; ++i) b[off + i] = uint8_t(x >> (8 * (n - 1 - i)));
}

std::array<uint8_t, 16> mic_of(std::span<const uint8_t, 16> kck, std::span<const uint8_t> frame) {
    std::vector<uint8_t> zeroed(frame.begin(), frame.end());
    std::fill_n(zeroed.begin() + kMicAt, 16, 0);
    auto                    h = crypto::hmac_sha1(kck, zeroed);
    std::array<uint8_t, 16> r;
    std::copy_n(h.begin(), 16, r.begin());
    return r;
}

} // namespace

Authenticator::Authenticator(std::array<uint8_t, 32>  pmk,
                             Mac                      ap,
                             Mac                      sta,
                             std::array<uint8_t, 16>  gtk,
                             std::span<const uint8_t> rsn_element,
                             Random                   random,
                             Send                     send,
                             Logger                  &log)
    : pmk_(pmk), ap_(ap), sta_(sta), gtk_(gtk), rsn_(rsn_element.begin(), rsn_element.end()),
      random_(std::move(random)), send_(std::move(send)), log_(log) {}

void Authenticator::start(uint64_t now_ns) {
    random_(anonce_);
    kck_    = {};
    kek_    = {};
    tk_     = {};
    state_  = State::SentM1;
    sends_  = 0;
    warned_ = false;
    send_current(now_ns);
}

void Authenticator::send_current(uint64_t now_ns) {
    // Every send, retries included, takes a new replay counter: a supplicant drops a
    // MIC-protected message whose counter does not increase, so a resent M3 with the
    // old one would never be answered.
    ++replay_;
    ++sends_;
    deadline_ = now_ns + kRetryNs;

    std::vector<uint8_t> key_data;
    uint16_t             info = kVersion2 | kPairwise | kAck;
    if (state_ == State::SentM3) {
        info |= kInstall | kMic | kSecure | kEncrypted;
        std::vector<uint8_t> plain = rsn_;
        plain.insert(plain.end(), std::begin(kGtkKde), std::end(kGtkKde));
        plain.insert(plain.end(), gtk_.begin(), gtk_.end());
        if (plain.size() % 8) {
            plain.push_back(0xdd);
            while (plain.size() % 8) plain.push_back(0);
        }
        key_data = crypto::aes_key_wrap(kek_, plain);
    }

    std::vector<uint8_t> f(kDataAt + key_data.size(), 0);
    f[0] = kEapolVersion;
    f[1] = kTypeKey;
    put_be(f, 2, f.size() - 4, 2);
    f[4] = kDescriptorRsn;
    put_be(f, kInfoAt, info, 2);
    put_be(f, kKeyLenAt, kCcmpKeyLength, 2);
    put_be(f, kReplayAt, replay_, 8);
    std::copy(anonce_.begin(), anonce_.end(), f.begin() + kNonceAt);
    // Key IV and RSC (the GTK's, of a group key never used yet) stay zero.
    put_be(f, kDataLenAt, key_data.size(), 2);
    std::copy(key_data.begin(), key_data.end(), f.begin() + kDataAt);
    if (info & kMic) {
        auto m = mic_of(kck_, f);
        std::copy(m.begin(), m.end(), f.begin() + kMicAt);
    }
    log_.log(
        Level::Debug,
        std::format("message {} to {}, replay counter {}", state_ == State::SentM3 ? 3 : 1, mac_text(sta_), replay_));
    send_(std::move(f));
}

void Authenticator::receive(std::span<const uint8_t> eapol, uint64_t now_ns) {
    if (state_ != State::SentM1 && state_ != State::SentM3) return;
    if (eapol.size() < kDataAt || eapol[1] != kTypeKey || eapol[4] != kDescriptorRsn) return;
    // The EAPOL length bounds the frame; anything after it is link padding.
    size_t end = 4 + be(eapol, 2, 2);
    if (end < kDataAt || end > eapol.size()) return;
    std::span<const uint8_t> f = eapol.first(end);
    if (kDataAt + be(f, kDataLenAt, 2) > f.size()) return;

    uint16_t info = uint16_t(be(f, kInfoAt, 2));
    if ((info & kVersionMask) != kVersion2 || !(info & kPairwise) || !(info & kMic) || (info & (kAck | kRequest))) {
        log_.log(Level::Debug, std::format("EAPOL-Key from {} with key info {:#06x} ignored", mac_text(sta_), info));
        return;
    }
    if (be(f, kReplayAt, 8) != replay_) {
        log_.log(Level::Debug, std::format("EAPOL-Key from {} with a stale replay counter ignored", mac_text(sta_)));
        return;
    }
    // As the 802.11 authenticator tells them apart: M4 carries no key data, M2 its RSN element.
    bool m4 = be(f, kDataLenAt, 2) == 0;
    if (m4 && state_ == State::SentM3) {
        on_m4(f);
    } else if (!m4 && state_ == State::SentM1) {
        on_m2(f, now_ns);
    } else {
        log_.log(Level::Debug, std::format("message {} from {} out of order", m4 ? 4 : 2, mac_text(sta_)));
    }
}

void Authenticator::on_m2(std::span<const uint8_t> f, uint64_t now_ns) {
    std::span<const uint8_t> snonce = f.subspan(kNonceAt, 32);

    // PTK = PRF-384(PMK, "Pairwise key expansion", min(AA,SPA) || max(AA,SPA) || min(ANonce,SNonce) || max(...)).
    std::vector<uint8_t> data;
    const Mac           &lo = std::min(ap_, sta_), &hi = std::max(ap_, sta_);
    data.insert(data.end(), lo.begin(), lo.end());
    data.insert(data.end(), hi.begin(), hi.end());
    bool a_first = std::lexicographical_compare(anonce_.begin(), anonce_.end(), snonce.begin(), snonce.end());
    std::span<const uint8_t> first  = a_first ? std::span<const uint8_t>(anonce_) : snonce;
    std::span<const uint8_t> second = a_first ? snonce : std::span<const uint8_t>(anonce_);
    data.insert(data.end(), first.begin(), first.end());
    data.insert(data.end(), second.begin(), second.end());
    std::vector<uint8_t> ptk = crypto::prf_sha1(pmk_, "Pairwise key expansion", data, 48);

    std::array<uint8_t, 16> kck;
    std::copy_n(ptk.begin(), 16, kck.begin());
    if (!std::equal(f.begin() + kMicAt, f.begin() + kMicAt + 16, mic_of(kck, f).begin())) {
        // A station with the wrong password answers every retry of M1 alike: warn once per attempt.
        log_.log(warned_ ? Level::Debug : Level::Warning,
                 std::format("message 2 from {} has a bad MIC: wrong password?", mac_text(sta_)));
        warned_ = true;
        return;
    }
    // The key data holds the station's RSN element. It is not compared with the one
    // of the beacons: the access point offers a single cipher suite and AKM, so a
    // station that got this far has chosen them.
    kck_ = kck;
    std::copy_n(ptk.begin() + 16, 16, kek_.begin());
    std::copy_n(ptk.begin() + 32, 16, tk_.begin());
    state_ = State::SentM3;
    sends_ = 0;
    send_current(now_ns);
}

void Authenticator::on_m4(std::span<const uint8_t> f) {
    if (!std::equal(f.begin() + kMicAt, f.begin() + kMicAt + 16, mic_of(kck_, f).begin())) {
        log_.log(Level::Debug, std::format("message 4 from {} has a bad MIC", mac_text(sta_)));
        return;
    }
    state_ = State::Done;
    log_.log(Level::Info, std::format("4-way handshake with {} done", mac_text(sta_)));
}

void Authenticator::tick(uint64_t now_ns) {
    if (state_ != State::SentM1 && state_ != State::SentM3) return;
    if (now_ns < deadline_) return;
    if (sends_ < kTries) {
        send_current(now_ns);
        return;
    }
    state_ = State::Failed;
    log_.log(Level::Warning, std::format("4-way handshake with {} failed", mac_text(sta_)));
}

std::optional<uint64_t> Authenticator::next_event_ns() const {
    if (state_ != State::SentM1 && state_ != State::SentM3) return std::nullopt;
    return deadline_;
}

} // namespace bk
