#include "net/router/access_point.h"

#include <algorithm>
#include <format>
#include <random>

#include "net/router/crypto.h"
#include "net/router/mac_text.h"

namespace bk {

namespace {

constexpr size_t kHeader = 24;

constexpr uint8_t kTypeMgmt = 0, kTypeData = 2;

constexpr uint8_t kAssocReq = 0, kAssocResp = 1, kReassocReq = 2, kReassocResp = 3, kProbeReq = 4, kProbeResp = 5,
                  kBeacon = 8, kDisassoc = 10, kAuth = 11, kDeauth = 12;

constexpr uint16_t kEss = 0x0001, kPrivacy = 0x0010, kShortPreamble = 0x0020, kShortSlot = 0x0400;

constexpr uint16_t kStatusOk = 0, kStatusBadAlgorithm = 13;

constexpr uint16_t kReasonHandshakeTimeout = 15;

constexpr AccessPoint::Mac kBroadcast = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

constexpr uint8_t kRates[]    = {0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24};
constexpr uint8_t kExtRates[] = {0x30, 0x48, 0x60, 0x6c};
// DTIM count 0, period 1, no bitmap control, an empty virtual bitmap.
constexpr uint8_t kTim[] = {0x00, 0x01, 0x00, 0x00};
// Version 1, group CCMP, one pairwise CCMP, one AKM PSK, no capabilities.
constexpr uint8_t kRsn[] = {0x01, 0x00, 0x00, 0x0f, 0xac, 0x04, 0x01, 0x00, 0x00, 0x0f,
                            0xac, 0x04, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x02, 0x00, 0x00};

constexpr uint8_t kRsnId = 48;

constexpr uint8_t  kLlc[]      = {0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00};
constexpr uint16_t kEtherEapol = 0x888e;
constexpr size_t   kCcmpHeader = 8;

constexpr uint16_t kAid = 1;

uint16_t le16(std::span<const uint8_t> b, size_t off) { return uint16_t(b[off] | b[off + 1] << 8); }

AccessPoint::Mac mac_at(std::span<const uint8_t> f, size_t off) {
    AccessPoint::Mac m;
    std::copy_n(f.begin() + off, 6, m.begin());
    return m;
}

void put16(std::vector<uint8_t> &v, uint16_t x) {
    v.push_back(uint8_t(x));
    v.push_back(uint8_t(x >> 8));
}

void put64(std::vector<uint8_t> &v, uint64_t x) {
    for (int i = 0; i < 8; ++i) v.push_back(uint8_t(x >> (8 * i)));
}

void element(std::vector<uint8_t> &v, uint8_t id, std::span<const uint8_t> data) {
    v.push_back(id);
    v.push_back(uint8_t(data.size()));
    v.insert(v.end(), data.begin(), data.end());
}

// The SSID element among the tagged parameters, if there is one.
std::optional<std::string> find_ssid(std::span<const uint8_t> els) {
    for (size_t i = 0; i + 2 <= els.size();) {
        size_t len = els[i + 1];
        if (i + 2 + len > els.size()) break;
        if (els[i] == 0) return std::string(els.begin() + i + 2, els.begin() + i + 2 + len);
        i += 2 + len;
    }
    return std::nullopt;
}

void random_device_fill(std::span<uint8_t> out) {
    static std::random_device rd;
    for (uint8_t &b : out) b = uint8_t(rd());
}

} // namespace

AccessPoint::AccessPoint(ApConfig config, TunedChannel tuned, Logger &log, Authenticator::Random random)
    : config_(std::move(config)), tuned_(std::move(tuned)), log_(log),
      random_(random ? std::move(random) : Authenticator::Random(random_device_fill)) {
    if (config_.password) pmk_ = crypto::wpa_psk(*config_.password, config_.ssid);
    reset();
    log_.log(Level::Info,
             std::format("access point '{}' on channel {} ({})",
                         config_.ssid,
                         config_.channel,
                         config_.password ? "WPA2-PSK" : "open"));
}

void AccessPoint::reset() {
    // A rebooted firmware has lost its association; the router hears so.
    if (station_) forget("reset");
    authenticated_.reset();
    next_beacon_ = now_ + kBeaconIntervalNs;
    outbox_.clear();
    seq_ = 0;
}

std::optional<uint64_t> AccessPoint::next_event_ns() const {
    uint64_t t = next_beacon_;
    for (const Pending &p : outbox_) t = std::min(t, p.due);
    if (std::optional<uint64_t> retry = auth_ ? auth_->next_event_ns() : std::nullopt) t = std::min(t, *retry);
    return t;
}

bool AccessPoint::heard() const {
    std::optional<unsigned> ch = tuned_ ? tuned_() : std::nullopt;
    return !ch || *ch == config_.channel;
}

void AccessPoint::tick(uint64_t now) {
    now_ = now;
    if (auth_) {
        auth_->tick(now);
        if (auth_->state() == Authenticator::State::Failed) {
            std::vector<uint8_t> f = header(kDeauth << 4, 0, *station_, config_.bssid);
            put16(f, kReasonHandshakeTimeout);
            queue(std::move(f));
            forget("4-way handshake failed");
        }
    }
    while (!outbox_.empty() && outbox_.front().due <= now) {
        Pending p = std::move(outbox_.front());
        outbox_.pop_front();
        if (p.on_channel && !heard()) continue;
        if (deliver_) deliver_(std::move(p.frame));
    }
    if (next_beacon_ <= now) {
        // One beacon however late the tick; the next stays on the 100 TU grid.
        next_beacon_ += (now - next_beacon_) / kBeaconIntervalNs * kBeaconIntervalNs + kBeaconIntervalNs;
        if (heard() && deliver_) deliver_(beacon_like(kBeacon, kBroadcast, now));
    }
}

void AccessPoint::queue(std::vector<uint8_t> frame, bool on_channel) {
    outbox_.push_back({now_ + kLatencyNs, std::move(frame), on_channel});
}

std::vector<uint8_t> AccessPoint::header(uint8_t fc0, uint8_t fc1, const Mac &a1, const Mac &a3) {
    std::vector<uint8_t> h{fc0, fc1, 0, 0};
    h.reserve(kHeader); // the whole header; also keeps GCC 13 from a false array-bounds warning
    h.insert(h.end(), a1.begin(), a1.end());
    h.insert(h.end(), config_.bssid.begin(), config_.bssid.end());
    h.insert(h.end(), a3.begin(), a3.end());
    put16(h, uint16_t((seq_++ & 0xfff) << 4));
    return h;
}

uint16_t AccessPoint::capabilities() const {
    return uint16_t(kEss | kShortPreamble | kShortSlot | (config_.password ? kPrivacy : 0));
}

std::vector<uint8_t> AccessPoint::beacon_like(uint8_t subtype, const Mac &dest, uint64_t at_ns) {
    auto f = header(uint8_t(subtype << 4), 0, dest, config_.bssid);
    put64(f, at_ns / 1000);
    put16(f, 100); // beacon interval, TU
    put16(f, capabilities());
    element(f, 0, {reinterpret_cast<const uint8_t *>(config_.ssid.data()), config_.ssid.size()});
    element(f, 1, kRates);
    const uint8_t channel = uint8_t(config_.channel);
    element(f, 3, {&channel, 1});
    element(f, 5, kTim);
    element(f, 50, kExtRates);
    if (config_.password) element(f, kRsnId, kRsn);
    return f;
}

void AccessPoint::transmit(std::span<const uint8_t> f) {
    if (f.size() < kHeader) return;
    switch ((f[0] >> 2) & 3) {
        case kTypeMgmt: on_management(f); break;
        case kTypeData: on_data(f); break;
        default: break;
    }
}

void AccessPoint::on_management(std::span<const uint8_t> f) {
    uint8_t subtype = f[0] >> 4;
    if (subtype == kProbeReq) {
        on_probe_request(f);
        return;
    }
    if (mac_at(f, 4) != config_.bssid || mac_at(f, 16) != config_.bssid) return;
    const Mac sta  = mac_at(f, 10);
    auto      body = f.subspan(kHeader);
    switch (subtype) {
        case kAuth: on_auth(sta, body); break;
        case kAssocReq:
        case kReassocReq: on_assoc(sta, subtype); break;
        case kDeauth:
        case kDisassoc:
            if (authenticated_ == sta) authenticated_.reset();
            if (station_ == sta) forget(subtype == kDeauth ? "deauthenticated" : "disassociated");
            break;
        default: break;
    }
}

void AccessPoint::on_probe_request(std::span<const uint8_t> f) {
    const Mac a1 = mac_at(f, 4);
    if (a1 != kBroadcast && a1 != config_.bssid) return;
    auto ssid = find_ssid(f.subspan(kHeader));
    if (!ssid || (!ssid->empty() && *ssid != config_.ssid)) return;
    queue(beacon_like(kProbeResp, mac_at(f, 10), now_ + kLatencyNs), true);
}

void AccessPoint::on_auth(const Mac &sta, std::span<const uint8_t> body) {
    if (body.size() < 6 || le16(body, 2) != 1) return;
    const uint16_t alg = le16(body, 0);
    // Authenticating again means the station starts over.
    if (station_ == sta) forget("authenticating again");
    std::vector<uint8_t> f = header(kAuth << 4, 0, sta, config_.bssid);
    put16(f, alg);
    put16(f, 2);
    if (alg != 0) {
        put16(f, kStatusBadAlgorithm);
        queue(std::move(f));
        return;
    }
    put16(f, kStatusOk);
    queue(std::move(f));
    authenticated_ = sta;
    log_.log(Level::Info, std::format("station {} authenticated", mac_text(sta)));
}

void AccessPoint::on_assoc(const Mac &sta, uint8_t subtype) {
    if (authenticated_ != sta && station_ != sta) {
        log_.log(Level::Debug, std::format("association request from unauthenticated {} ignored", mac_text(sta)));
        return;
    }
    std::vector<uint8_t> f =
        header(uint8_t((subtype == kReassocReq ? kReassocResp : kAssocResp) << 4), 0, sta, config_.bssid);
    put16(f, capabilities());
    put16(f, kStatusOk);
    put16(f, kAid | 0xc000);
    element(f, 1, kRates);
    element(f, 50, kExtRates);
    queue(std::move(f));
    if (station_ == sta) return; // a repeated request, its response was lost
    if (station_) forget("replaced");
    authenticated_.reset();
    associated(sta);
}

void AccessPoint::associated(const Mac &sta) {
    station_ = sta;
    log_.log(Level::Info, std::format("station {} associated with '{}'", mac_text(sta), config_.ssid));
    if (pmk_) {
        // The port opens, and the link is reported, once the 4-way handshake is done.
        std::array<uint8_t, 16> gtk;
        random_(gtk);
        // The authenticator puts into M3 exactly the RSN element the beacons carry.
        std::vector<uint8_t> rsn;
        element(rsn, kRsnId, kRsn);
        auth_.emplace(
            *pmk_, config_.bssid, sta, gtk, rsn, random_, [this](std::vector<uint8_t> e) { send_eapol(e); }, log_);
        auth_->start(now_);
        return;
    }
    port_open_ = true;
    if (link_change_) link_change_(true, sta, now_);
}

void AccessPoint::forget(const char *why) {
    const Mac  sta    = *station_;
    const bool was_up = port_open_;
    station_.reset();
    auth_.reset();
    port_open_ = false;
    log_.log(Level::Info, std::format("station {} left ({})", mac_text(sta), why));
    if (was_up && link_change_) link_change_(false, sta, now_);
}

void AccessPoint::on_data(std::span<const uint8_t> f) {
    constexpr uint8_t kToDs = 0x01, kFromDs = 0x02, kProtected = 0x40;
    constexpr uint8_t kNoData = 0x40, kQos = 0x80; // subtype bits in the first FC byte
    if (!station_ || !(f[1] & kToDs) || (f[1] & kFromDs) || (f[0] & kNoData)) return;
    if (mac_at(f, 4) != config_.bssid || mac_at(f, 10) != *station_) return;
    size_t hdr = kHeader + ((f[0] & kQos) ? 2 : 0);
    if (f[1] & kProtected) {
        // The emulated MAC ciphers nothing: behind the CCMP header the firmware
        // wrote comes the plaintext, then whatever room the MAC was left for the
        // MIC. Those trailing bytes do no harm: the IP layer cuts link padding.
        if (!pmk_) return;
        hdr += kCcmpHeader;
    }
    if (f.size() < hdr + 8 || !std::equal(std::begin(kLlc), std::end(kLlc), f.begin() + hdr)) return;
    if ((f[hdr + 6] << 8 | f[hdr + 7]) == kEtherEapol) {
        on_eapol(f.subspan(hdr + 8));
        return;
    }
    if (!port_open_) return;
    std::vector<uint8_t> eth;
    eth.reserve(f.size() - hdr + 6);
    eth.insert(eth.end(), f.begin() + 16, f.begin() + 22); // addr3: destination
    eth.insert(eth.end(), f.begin() + 10, f.begin() + 16); // addr2: source
    eth.insert(eth.end(), f.begin() + hdr + 6, f.end());   // ethertype, payload
    if (ethernet_in_) ethernet_in_(std::move(eth));
}

void AccessPoint::on_eapol(std::span<const uint8_t> eapol) {
    if (!auth_) return;
    auth_->receive(eapol, now_);
    if (auth_->state() == Authenticator::State::Done && !port_open_) {
        port_open_ = true;
        if (link_change_) link_change_(true, *station_, now_);
    }
}

void AccessPoint::send_eapol(std::span<const uint8_t> eapol) {
    auto f = header(0x08, 0x02, *station_, config_.bssid);
    f.insert(f.end(), std::begin(kLlc), std::end(kLlc));
    f.push_back(uint8_t(kEtherEapol >> 8));
    f.push_back(uint8_t(kEtherEapol));
    f.insert(f.end(), eapol.begin(), eapol.end());
    queue(std::move(f));
}

void AccessPoint::send_ethernet(std::span<const uint8_t> eth) {
    if (!port_open_ || eth.size() < 14) return;
    // With WPA2 too the frame goes out plain: no Protected bit, no CCMP header.
    // The MAC model reports the pairwise key installed for the transmitter in
    // the receive status (key valid, key index), and both the project firmware
    // and the stock firmware accept such a frame and hand it to lwIP (DHCP and
    // ping work), so neither a Protected bit with a CCMP header before the
    // plaintext nor a decryption status in the receive descriptor is needed.
    auto f = header(0x08, 0x02, *station_, mac_at(eth, 6));
    f.insert(f.end(), std::begin(kLlc), std::end(kLlc));
    f.insert(f.end(), eth.begin() + 12, eth.end());
    queue(std::move(f));
}

std::optional<unsigned> channel_from_rc(uint32_t pll_ldo_chan) {
    const unsigned mhz = 2400 + (pll_ldo_chan >> 25);
    if (mhz == 2484) return 14;
    if (mhz < 2412 || mhz > 2472 || (mhz - 2407) % 5 != 0) return std::nullopt;
    return (mhz - 2407) / 5;
}

} // namespace bk
