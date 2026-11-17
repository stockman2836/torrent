#include "peer_diagnostics.h"

namespace torrent::app {

std::string peerAddress(const PeerStatus& peer) {
    return (peer.ip.find(':') == std::string::npos ? peer.ip : "[" + peer.ip + "]") + ":" + std::to_string(peer.port);
}

std::string peerEndText(PeerEndReason reason) {
    switch (reason) {
    case PeerEndReason::Stopped: return u8"Остановлено приложением";
    case PeerEndReason::Paused: return u8"Пауза";
    case PeerEndReason::RemoteClosed: return u8"Пир закрыл соединение";
    case PeerEndReason::ConnectFailed: return u8"Не удалось установить TCP-соединение";
    case PeerEndReason::ConnectTimeout: return u8"Истекло время TCP-подключения";
    case PeerEndReason::HandshakeTimeout: return u8"Истекло время приветствия BitTorrent";
    case PeerEndReason::InvalidHandshake: return u8"Некорректное приветствие BitTorrent";
    case PeerEndReason::WrongTorrent: return u8"В приветствии указан другой торрент";
    case PeerEndReason::SelfConnection: return u8"Обнаружено подключение к самому себе";
    case PeerEndReason::ReceiveTimeout: return u8"Истекло время получения сообщения";
    case PeerEndReason::ReceiveFailed: return u8"Ошибка получения данных";
    case PeerEndReason::SendTimeout: return u8"Истекло время отправки данных";
    case PeerEndReason::SendFailed: return u8"Ошибка отправки данных";
    case PeerEndReason::InvalidMessage: return u8"Некорректное сообщение BitTorrent";
    case PeerEndReason::IdleTimeout: return u8"Нет сообщений от пира более пяти минут";
    case PeerEndReason::PeerLimit: return u8"Достигнут лимит подключений";
    case PeerEndReason::None: return "";
    default: return u8"Причина завершения не определена";
    }
}

std::string peerStateText(const PeerStatus& peer, bool seeding) {
    switch (peer.state) {
    case PeerState::Connecting: return u8"TCP-подключение";
    case PeerState::Handshaking: return u8"Обмен приветствиями";
    case PeerState::Disconnected: return peerEndText(peer.end_reason);
    default: return peer.pending_requests ? u8"Ожидание блоков" :
        peer.choked && !seeding ? u8"Ожидание разрешения" : u8"Подключён";
    }
}

std::string peerResultText(const PeerStatus& peer) {
    auto result = peerEndText(peer.end_reason);
    if (peer.error_code) result += u8" · системный код " + std::to_string(peer.error_code);
    return result;
}

} // namespace torrent::app
