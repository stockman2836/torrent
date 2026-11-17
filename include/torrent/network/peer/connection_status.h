#pragma once

namespace torrent {

// Observed locally by the owning connection worker; no inferred NAT/firewall cause.
enum class PeerEndReason {
    None, Unknown, Stopped, Paused, RemoteClosed, ConnectFailed, ConnectTimeout,
    HandshakeTimeout, InvalidHandshake, WrongTorrent, SelfConnection,
    ReceiveTimeout, ReceiveFailed, SendTimeout, SendFailed, InvalidMessage,
    IdleTimeout, PeerLimit
};

} // namespace torrent
