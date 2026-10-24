#include "net/station.h"

#include <algorithm>

namespace bk {

namespace {

constexpr size_t kHeader = 24;

constexpr uint8_t kTypeMgmt = 0, kTypeData = 2;

constexpr uint8_t kAssocReq = 0, kAssocResp = 1, kProbeReq = 4, kProbeResp = 5, kBeacon = 8, kDisassoc = 10, kAuth = 11,
                  kDeauth = 12;

constexpr uint16_t kPrivacy = 0x0010;

constexpr Station::Mac kBroadcast = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

constexpr uint8_t kRates[]    = {0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24};
constexpr uint8_t kExtRates[] = {0x30, 0x48, 0x60, 0x6c};

constexpr uint8_t kLlc[] = {0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00};

uint16_t le16(std::span<const uint8_t> b, size_t off) { return uint16_t(b[off] | b[off + 1] << 8); }

Station::Mac mac_at(std::span<const uint8_t> f, size_t off) {
    Station::Mac m;
    std::copy_n(f.begin() + off, 6, m.begin());
    return m;
}

void put16(std::vector<uint8_t> &v, uint16_t x) {
    v.push_back(uint8_t(x));
    v.push_back(uint8_t(x >> 8));
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

} // namespace

Station::Station(Mac mac, std::optional<std::string> ssid_filter, Logger &log)
    : mac_(mac), filter_(std::move(ssid_filter)), log_(log) {
    reset();
}

void Station::reset() {
    state_ = State::Scan;
    bssid_.reset();
    ssid_.clear();
    next_ = now_ + RETRY_NS;
    outbox_.clear();
    seq_ = 0;
}

std::optional<uint64_t> Station::next_event_ns() const {
    std::optional<uint64_t> t = next_;
    for (const Pending &p : outbox_)
        if (!t || p.due < *t) t = p.due;
    return t;
}

void Station::tick(uint64_t now) {
    now_ = now;
    // Replies built below are due later, never in this very tick.
    while (!outbox_.empty() && outbox_.front().due <= now) {
        std::vector<uint8_t> frame = std::move(outbox_.front().frame);
        outbox_.pop_front();
        if (deliver_) deliver_(std::move(frame));
    }
    if (next_ && *next_ <= now) {
        next_ = now + RETRY_NS;
        switch (state_) {
            case State::Scan: send_probe_request(); break;
            case State::Auth: send_auth_request(); break;
            case State::Assoc: send_assoc_request(); break;
            case State::Run: next_.reset(); break;
        }
    }
}

void Station::queue(std::vector<uint8_t> frame) { outbox_.push_back({now_ + LATENCY_NS, std::move(frame)}); }

std::vector<uint8_t> Station::header(uint8_t fc0, uint8_t fc1, const Mac &a1, const Mac &a2, const Mac &a3) {
    std::vector<uint8_t> h{fc0, fc1, 0, 0};
    h.reserve(24); // the whole header; also keeps GCC 13 from a false array-bounds warning
    h.insert(h.end(), a1.begin(), a1.end());
    h.insert(h.end(), a2.begin(), a2.end());
    h.insert(h.end(), a3.begin(), a3.end());
    put16(h, uint16_t((seq_++ & 0xfff) << 4));
    return h;
}

std::vector<uint8_t> Station::management(uint8_t subtype, const Mac &dest, std::span<const uint8_t> body) {
    // A station always talks to the BSS it picked; only the probe request has
    // no BSS yet and goes out as a broadcast.
    const Mac &bss = bssid_ ? *bssid_ : kBroadcast;
    auto       f   = header(uint8_t(subtype << 4), 0, dest, mac_, bss);
    f.insert(f.end(), body.begin(), body.end());
    return f;
}

void Station::elements(std::vector<uint8_t> &body, const std::string &ssid) {
    element(body, 0, {reinterpret_cast<const uint8_t *>(ssid.data()), ssid.size()});
    element(body, 1, kRates);
    element(body, 50, kExtRates);
}

void Station::send_probe_request() {
    std::vector<uint8_t> body;
    elements(body, filter_.value_or(""));
    queue(management(kProbeReq, kBroadcast, body));
}

void Station::send_auth_request() {
    std::vector<uint8_t> body;
    put16(body, 0); // open system
    put16(body, 1);
    put16(body, 0);
    queue(management(kAuth, *bssid_, body));
}

void Station::send_assoc_request() {
    std::vector<uint8_t> body;
    put16(body, 0x0421);
    put16(body, 10); // listen interval
    elements(body, ssid_);
    queue(management(kAssocReq, *bssid_, body));
}

void Station::transmit(std::span<const uint8_t> f) {
    if (f.size() < kHeader) return;
    switch ((f[0] >> 2) & 3) {
        case kTypeMgmt: on_management(f); break;
        case kTypeData: on_data(f); break;
        default: break;
    }
}

void Station::scan_frame(std::span<const uint8_t> f) {
    auto body = f.subspan(kHeader);
    if (body.size() < 12) return;
    auto ssid = find_ssid(body.subspan(12));
    if (!ssid || (filter_ && *ssid != *filter_)) return;
    if (le16(body, 10) & kPrivacy) {
        log_.warn_once(std::hash<std::string>{}(*ssid),
                       "network '" + *ssid + "' needs a password; only open networks are joined");
        return;
    }
    bssid_ = mac_at(f, 16);
    ssid_  = *ssid;
    state_ = State::Auth;
    next_  = now_ + AUTH_DELAY_NS;
}

void Station::on_management(std::span<const uint8_t> f) {
    uint8_t subtype = f[0] >> 4;
    if (state_ == State::Scan && (subtype == kBeacon || subtype == kProbeResp)) {
        scan_frame(f);
        return;
    }
    if (!bssid_ || mac_at(f, 4) != mac_ || mac_at(f, 16) != *bssid_) return;
    auto body = f.subspan(kHeader);
    switch (subtype) {
        case kAuth:
            if (state_ != State::Auth || body.size() < 6 || le16(body, 2) != 2) return;
            if (le16(body, 4) == 0) {
                state_ = State::Assoc;
                next_  = now_;
            } else {
                next_ = now_ + RETRY_NS;
            }
            break;
        case kAssocResp:
            if (state_ != State::Assoc || body.size() < 6) return;
            if (le16(body, 2) == 0) {
                state_ = State::Run;
                next_.reset();
                log_.log(Level::Info,
                         "associated with '" + ssid_ + "' (AID " + std::to_string(le16(body, 4) & 0x3fff) + ")");
                if (link_up_) link_up_(now_);
            } else {
                state_ = State::Auth;
                next_  = now_ + RETRY_NS;
            }
            break;
        case kDeauth:
        case kDisassoc:
            state_ = State::Auth;
            next_  = now_ + RETRY_NS;
            break;
        default: break;
    }
}

void Station::on_data(std::span<const uint8_t> f) {
    constexpr uint8_t kToDs = 0x01, kFromDs = 0x02;
    if (state_ != State::Run || !(f[1] & kFromDs) || (f[1] & kToDs)) return;
    size_t hdr = kHeader + ((f[0] & 0x80) ? 2 : 0);
    if (f.size() < hdr + 8 || !std::equal(std::begin(kLlc), std::end(kLlc), f.begin() + hdr)) return;
    std::vector<uint8_t> eth;
    eth.reserve(f.size() - hdr + 6);
    eth.insert(eth.end(), f.begin() + 4, f.begin() + 10);  // addr1: destination
    eth.insert(eth.end(), f.begin() + 16, f.begin() + 22); // addr3: source
    eth.insert(eth.end(), f.begin() + hdr + 6, f.end());   // ethertype, payload
    if (ethernet_in_) ethernet_in_(std::move(eth));
}

void Station::send_ethernet(std::span<const uint8_t> eth) {
    if (state_ != State::Run || eth.size() < 14) return;
    auto f = header(0x08, 0x01, *bssid_, mac_, mac_at(eth, 0));
    f.insert(f.end(), std::begin(kLlc), std::end(kLlc));
    f.insert(f.end(), eth.begin() + 12, eth.end());
    queue(std::move(f));
}

} // namespace bk
