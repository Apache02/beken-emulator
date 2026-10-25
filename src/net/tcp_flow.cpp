#include "net/tcp_flow.h"

#include <algorithm>
#include <vector>

namespace bk {

using namespace packets;

namespace {

// Sequence numbers wrap: the signed distance from b to a.
int32_t distance(uint32_t a, uint32_t b) { return int32_t(a - b); }

} // namespace

TcpFlow::TcpFlow(TcpLink   &link,
                 uint16_t   local_port,
                 uint16_t   peer_port,
                 uint32_t   iss,
                 Deliver    deliver,
                 PeerClosed on_peer_closed,
                 Closed     on_closed)
    : link_(link), local_port_(local_port), peer_port_(peer_port), iss_(iss), deliver_(std::move(deliver)),
      on_peer_closed_(std::move(on_peer_closed)), on_closed_(std::move(on_closed)) {}

void TcpFlow::start() {
    snd_una_ = iss_;
    snd_nxt_ = snd_max_ = iss_ + 1;
    tries_              = 1;
    deadline_           = link_.now_ns() + RTO_NS;
    send_syn();
}

void TcpFlow::accept(const Tcp &syn) {
    state_   = State::SynReceived;
    rcv_nxt_ = syn.seq + 1;
    snd_wnd_ = syn.window;
    mss_     = std::min<uint16_t>(OUR_MSS, syn.mss && *syn.mss ? *syn.mss : DEFAULT_PEER_MSS);
    snd_una_ = iss_;
    snd_nxt_ = snd_max_ = iss_ + 1;
    tries_              = 1;
    deadline_           = link_.now_ns() + RTO_NS;
    send_syn_ack();
}

void TcpFlow::send(uint8_t flags, uint32_t seq, std::span<const uint8_t> data, std::optional<uint16_t> mss) {
    link_.send_segment(
        Tcp{local_port_, peer_port_, seq, (flags & kTcpAck) ? rcv_nxt_ : 0, flags, RECEIVE_WINDOW, mss, data});
}

void TcpFlow::send_syn() { send(kTcpSyn, iss_, {}, OUR_MSS); }

void TcpFlow::send_syn_ack() { send(kTcpSyn | kTcpAck, iss_, {}, OUR_MSS); }

void TcpFlow::receive(const Tcp &seg) {
    if (state_ == State::SynSent)
        on_syn_sent(seg);
    else if (state_ == State::SynReceived)
        on_syn_received(seg);
    else if (state_ == State::Open)
        on_open(seg);
}

void TcpFlow::on_syn_received(const Tcp &seg) {
    if (seg.flags & kTcpRst) {
        end(true);
        return;
    }
    // The peer did not hear our SYN|ACK.
    if (seg.flags & kTcpSyn) {
        send_syn_ack();
        return;
    }
    if (!(seg.flags & kTcpAck)) return;
    if (seg.ack != iss_ + 1) {
        send(kTcpRst, seg.ack);
        return;
    }
    state_   = State::Open;
    snd_una_ = snd_nxt_;
    tries_   = 1;
    deadline_.reset();
    // The ACK may already carry data; bytes queued meanwhile go out now.
    on_open(seg);
}

void TcpFlow::on_syn_sent(const Tcp &seg) {
    const bool ack = seg.flags & kTcpAck;
    if (seg.flags & kTcpRst) {
        // Only an RST that answers our SYN counts; a blind one does not.
        if (ack && seg.ack == iss_ + 1) end(true);
        return;
    }
    if (!(seg.flags & kTcpSyn) || !ack) return;
    if (seg.ack != iss_ + 1) {
        send(kTcpRst, seg.ack);
        end(true);
        return;
    }
    state_   = State::Open;
    rcv_nxt_ = seg.seq + 1;
    snd_wnd_ = seg.window;
    mss_     = std::min<uint16_t>(OUR_MSS, seg.mss && *seg.mss ? *seg.mss : DEFAULT_PEER_MSS);
    snd_una_ = snd_nxt_;
    tries_   = 1;
    deadline_.reset();
    send_ack();
    transmit();
}

void TcpFlow::on_open(const Tcp &seg) {
    if (seg.flags & kTcpRst) {
        end(true);
        return;
    }
    // A repeated SYN+ACK means our handshake ACK got lost.
    if (seg.flags & kTcpSyn) {
        send_ack();
        return;
    }
    if (seg.flags & kTcpAck) on_ack(seg);
    on_data(seg);
    if (state_ != State::Open) return;
    transmit();
    if (fin_acked_ && peer_fin_) end(false);
}

void TcpFlow::on_ack(const Tcp &seg) {
    // Always the newest window, also on a duplicate ACK.
    snd_wnd_ = seg.window;

    uint32_t acked = seg.ack - snd_una_;
    if (acked == 0 || acked > snd_max_ - snd_una_) return;
    const bool fin = acked > buffer_.size();
    buffer_.erase(buffer_.begin(), buffer_.begin() + (acked - fin));
    snd_una_ = seg.ack;
    if (distance(snd_nxt_, snd_una_) < 0) snd_nxt_ = snd_una_;
    if (fin) fin_acked_ = fin_out_ = true;
    rearm();
}

void TcpFlow::rearm() {
    tries_ = 1;
    if (snd_max_ != snd_una_)
        deadline_ = link_.now_ns() + RTO_NS;
    else
        deadline_.reset();
}

void TcpFlow::on_data(const Tcp &seg) {
    const size_t len = seg.payload.size();
    const bool   fin = seg.flags & kTcpFin;
    if (len == 0 && !fin) return;

    // How much of the segment lies before what we expect.
    const int32_t skip = distance(rcv_nxt_, seg.seq);
    if (skip < 0) {
        // A gap: no reordering buffer, the peer sends it again.
        send_ack();
        return;
    }
    if (size_t(skip) > len) {
        // Nothing new at all.
        send_ack();
        return;
    }
    if (size_t(skip) < len) {
        auto fresh = seg.payload.subspan(skip);
        rcv_nxt_ += uint32_t(fresh.size());
        if (deliver_) deliver_(fresh);
        if (state_ != State::Open) return;
    }
    // The segment now ends exactly at rcv_nxt_, so its FIN is the next byte.
    const bool new_fin = fin && !peer_fin_;
    if (new_fin) {
        peer_fin_ = true;
        ++rcv_nxt_;
    }
    send_ack();
    if (new_fin && on_peer_closed_) on_peer_closed_();
}

void TcpFlow::write(std::span<const uint8_t> data) {
    if (state_ == State::Ended || close_requested_) return;
    buffer_.insert(buffer_.end(), data.begin(), data.end());
    if (state_ == State::Open) transmit();
}

void TcpFlow::close() {
    if (state_ == State::Ended || close_requested_) return;
    close_requested_ = true;
    if (state_ == State::Open) transmit();
}

void TcpFlow::abort() {
    if (state_ == State::Ended) return;
    if (state_ == State::Open || state_ == State::SynReceived) send(kTcpRst | kTcpAck, snd_nxt_);
    on_closed_ = nullptr;
    end(false);
}

void TcpFlow::discard() {
    on_closed_ = nullptr;
    end(false);
}

void TcpFlow::transmit() {
    if (state_ != State::Open || fin_acked_) return;
    bool sent = false;
    if (!fin_out_) {
        for (;;) {
            const size_t  off   = snd_nxt_ - snd_una_;
            const size_t  avail = buffer_.size() - off;
            const int32_t room  = distance(snd_una_ + snd_wnd_, snd_nxt_);
            if (avail == 0 || room <= 0) break;
            const size_t               n = std::min({avail, size_t(mss_), size_t(room)});
            const std::vector<uint8_t> chunk(buffer_.begin() + off, buffer_.begin() + off + n);
            send(kTcpAck | (off + n == buffer_.size() ? kTcpPsh : 0), snd_nxt_, chunk);
            snd_nxt_ += uint32_t(n);
            sent = true;
        }
    }
    if (close_requested_ && !fin_out_ && snd_nxt_ - snd_una_ == buffer_.size()) {
        send(kTcpFin | kTcpAck, snd_nxt_);
        ++snd_nxt_;
        fin_out_ = true;
        sent     = true;
    }
    if (distance(snd_nxt_, snd_max_) > 0) snd_max_ = snd_nxt_;
    if (sent && !deadline_) deadline_ = link_.now_ns() + RTO_NS;
}

void TcpFlow::tick() {
    if (deadline_ && link_.now_ns() >= *deadline_) on_timeout();
}

void TcpFlow::on_timeout() {
    if (tries_ >= MAX_TRIES) {
        end(true);
        return;
    }
    ++tries_;
    deadline_ = link_.now_ns() + (RTO_NS << std::min(tries_ - 1, 5u));
    if (state_ == State::SynSent) {
        send_syn();
        return;
    }
    if (state_ == State::SynReceived) {
        send_syn_ack();
        return;
    }
    // Go back to the oldest unacknowledged byte.
    snd_nxt_ = snd_una_;
    fin_out_ = false;
    transmit();
}

void TcpFlow::end(bool reset) {
    if (state_ == State::Ended) return;
    state_ = State::Ended;
    deadline_.reset();
    buffer_.clear();
    if (auto closed = std::move(on_closed_)) closed(reset);
}

} // namespace bk
