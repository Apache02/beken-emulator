#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include "core/log.h"

namespace bk {

// The authenticator side of the WPA2-PSK 4-way handshake with one station:
// CCMP pairwise and group, key descriptor version 2 (HMAC-SHA1-128 MIC, AES key
// wrap). It sends M1, checks M2, sends M3 with the group key, checks M4 and then
// hands out the pairwise temporal key. Every message it waits for an answer to
// is sent up to kTries times, kRetryNs apart; then the handshake has failed.
// EAPOL frames start at the EAPOL version byte: no 802.11 header, no LLC/SNAP.
class Authenticator {
  public:
    enum class State { Idle, SentM1, SentM3, Done, Failed };
    using Mac    = std::array<uint8_t, 6>;
    using Send   = std::function<void(std::vector<uint8_t> eapol)>; // EAPOL frame from the version byte
    using Random = std::function<void(std::span<uint8_t>)>;

    static constexpr uint64_t kRetryNs = 1'000'000'000;
    static constexpr unsigned kTries   = 4;

    // rsn_element is the whole RSN element of the beacons (ID and length included); it goes into M3.
    Authenticator(std::array<uint8_t, 32>  pmk,
                  Mac                      ap,
                  Mac                      sta,
                  std::array<uint8_t, 16>  gtk,
                  std::span<const uint8_t> rsn_element,
                  Random                   random,
                  Send                     send,
                  Logger                  &log);

    void                           start(uint64_t now_ns);                                   // sends M1
    void                           receive(std::span<const uint8_t> eapol, uint64_t now_ns); // M2, M4
    void                           tick(uint64_t now_ns);                                    // retries, then Failed
    std::optional<uint64_t>        next_event_ns() const;
    State                          state() const { return state_; }
    const std::array<uint8_t, 16> &tk() const { return tk_; } // valid in Done

  private:
    // Sends the message of the current state (M1 or M3) with the next replay counter.
    void send_current(uint64_t now_ns);
    void on_m2(std::span<const uint8_t> frame, uint64_t now_ns);
    void on_m4(std::span<const uint8_t> frame);

    std::array<uint8_t, 32> pmk_;
    Mac                     ap_;
    Mac                     sta_;
    std::array<uint8_t, 16> gtk_;
    std::vector<uint8_t>    rsn_;
    Random                  random_;
    Send                    send_;
    Logger                 &log_;

    State                   state_ = State::Idle;
    std::array<uint8_t, 32> anonce_{};
    std::array<uint8_t, 16> kck_{};
    std::array<uint8_t, 16> kek_{};
    std::array<uint8_t, 16> tk_{};
    uint64_t                replay_   = 0;     // of the last message sent; the answer must carry the same
    unsigned                sends_    = 0;     // of the current message
    uint64_t                deadline_ = 0;     // of the current try
    bool                    warned_   = false; // about a bad M2 in this attempt
};

} // namespace bk
