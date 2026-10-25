#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <span>

#include "net/packets.h"

namespace bk {

// What a flow needs from its surroundings: a way out and a clock. The flow
// never sleeps or reads time on its own, so the owner decides what "now" is.
class TcpLink {
  public:
    virtual ~TcpLink() = default;
    // The segment is only valid during the call.
    virtual void     send_segment(const packets::Tcp &segment) = 0;
    virtual uint64_t now_ns() const                            = 0;
};

// One TCP connection, opened by us (start) or by the peer (accept). The
// sequence space, window and retransmission are kept as
// simple as the virtual link allows (no congestion control, no reordering
// buffer).
class TcpFlow {
  public:
    enum class State { SynSent, SynReceived, Open, Ended };

    using Deliver    = std::function<void(std::span<const uint8_t>)>;
    using PeerClosed = std::function<void()>;
    using Closed     = std::function<void(bool reset)>;

    static constexpr uint64_t RTO_NS            = 1'000'000'000;
    static constexpr unsigned MAX_TRIES         = 6;
    static constexpr uint16_t OUR_MSS           = 1400;
    static constexpr uint16_t DEFAULT_PEER_MSS  = 536;
    static constexpr uint16_t RECEIVE_WINDOW    = 16384;
    static constexpr size_t   SEND_BUFFER_LIMIT = 65536;

    TcpFlow(TcpLink   &link,
            uint16_t   local_port,
            uint16_t   peer_port,
            uint32_t   iss,
            Deliver    deliver,
            PeerClosed on_peer_closed,
            Closed     on_closed);

    // Sends the SYN.
    void start();
    // The passive open: takes the peer's ISN, window and MSS from its SYN and
    // answers SYN|ACK, repeated like the SYN until the peer's ACK arrives.
    void accept(const packets::Tcp &syn);
    void receive(const packets::Tcp &segment);
    // Runs the retransmission timer if it is due.
    void tick();

    // Bytes are queued without limit; callers check can_write() first. Ignored
    // after close() or once the flow ended.
    void write(std::span<const uint8_t> data);
    bool can_write() const { return state_ != State::Ended && !close_requested_ && buffer_.size() < SEND_BUFFER_LIMIT; }
    // A FIN follows the queued bytes.
    void close();
    // RST and end, without on_closed.
    void abort();
    // End without a word on the wire or a callback; the owner is going away.
    void discard();

    State                   state() const { return state_; }
    bool                    finished() const { return state_ == State::Ended; }
    bool                    peer_closed() const { return peer_fin_; }
    std::optional<uint64_t> deadline() const { return deadline_; }
    uint16_t                local_port() const { return local_port_; }
    uint16_t                mss() const { return mss_; }
    // Bytes queued or in flight, not yet acknowledged.
    size_t buffered() const { return buffer_.size(); }

  private:
    void
    send(uint8_t flags, uint32_t seq, std::span<const uint8_t> data = {}, std::optional<uint16_t> mss = std::nullopt);
    void send_syn();
    void send_syn_ack();
    void on_syn_received(const packets::Tcp &seg);
    void send_ack() { send(packets::kTcpAck, snd_nxt_); }
    void on_syn_sent(const packets::Tcp &seg);
    void on_open(const packets::Tcp &seg);
    void on_ack(const packets::Tcp &seg);
    void on_data(const packets::Tcp &seg);
    void transmit();
    void on_timeout();
    void rearm();
    void end(bool reset);

    TcpLink   &link_;
    uint16_t   local_port_;
    uint16_t   peer_port_;
    uint32_t   iss_;
    Deliver    deliver_;
    PeerClosed on_peer_closed_;
    Closed     on_closed_;

    State    state_   = State::SynSent;
    uint32_t rcv_nxt_ = 0;
    // snd_una_ is the oldest unacknowledged sequence number; buffer_ holds the
    // data from there on. snd_nxt_ is where transmission goes next and
    // snd_max_ the highest it ever reached; both count the FIN. They differ
    // only after a go-back-N restart.
    uint32_t            snd_una_ = 0;
    uint32_t            snd_nxt_ = 0;
    uint32_t            snd_max_ = 0;
    uint32_t            snd_wnd_ = 0;
    uint16_t            mss_     = DEFAULT_PEER_MSS; // settled when the peer answers
    std::deque<uint8_t> buffer_;
    bool                close_requested_ = false;
    bool                fin_out_         = false; // the FIN is part of this pass
    bool                fin_acked_       = false;
    bool                peer_fin_        = false;

    unsigned                tries_ = 0;
    std::optional<uint64_t> deadline_;
};

} // namespace bk
