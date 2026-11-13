#include "network_diagnostics.h"
#include "speed_limits.h"
#include <algorithm>
#include <sstream>

namespace torrent::app {
namespace {
std::string rate(int64_t value) {
    return value > 0 ? formatSpeedLimit(value) + u8" КиБ/с" : u8"без ограничения";
}
const char* enabled(bool value) { return value ? u8"включён" : u8"выключен"; }
std::string trackerHost(const std::string& url) {
    const auto scheme = url.find("://");
    if (scheme == std::string::npos) return url.empty() ? u8"не указан" : u8"адрес скрыт";
    const auto start = scheme + 3;
    auto authority = url.substr(start, url.find_first_of("/?#", start) - start);
    const auto credentials = authority.rfind('@');
    if (credentials != std::string::npos) authority.erase(0, credentials + 1);
    return url.substr(0, start) + authority; // Report hosts only; paths/query strings can contain passkeys.
}
const char* trackerState(TrackerState value) {
    switch (value) {
    case TrackerState::Waiting: return u8"ожидает запроса";
    case TrackerState::Announcing: return u8"ожидает ответа";
    case TrackerState::Working: return u8"ответ получен";
    case TrackerState::Error: return u8"ошибка";
    default: return u8"не указан";
    }
}
}

NetworkDiagnostics diagnoseNetwork(const DownloadSnapshot& snapshot) {
    const auto& network = snapshot.network;
    NetworkDiagnostics result;
    const auto& mapping = network.port_mapping;
    const char* mapping_state = u8"выключено";
    switch (mapping.state) {
    case MappingState::Discovering: mapping_state = u8"поиск роутера…"; break;
    case MappingState::Mapping: mapping_state = u8"настройка или продление правил…"; break;
    case MappingState::Active: mapping_state = u8"роутер подтвердил правила"; break;
    case MappingState::Partial: mapping_state = u8"подтверждена часть правил"; break;
    case MappingState::Error: mapping_state = u8"настройка не подтверждена"; break;
    case MappingState::Removing: mapping_state = u8"удаление правил…"; break;
    case MappingState::Stopped:
        mapping_state = mapping.error.empty() ? u8"сеанс остановлен, своих правил нет" : u8"удаление правил не подтверждено"; break;
    default:
        if (!mapping.error.empty()) mapping_state = u8"выключено, удаление правил не подтверждено";
        else if (mapping.enabled) mapping_state = u8"включится при запуске загрузки";
        break;
    }
    result.mapping = std::string(u8"Автооткрытие портов: ") + mapping_state;
    if (mapping.protocol != MappingProtocol::None)
        result.mapping += mapping.protocol == MappingProtocol::NatPmp ? " (NAT-PMP)" : " (UPnP)";
    for (const auto& port : mapping.ports) if (port.active)
        result.mapping += std::string(" · ") + (port.transport == PortTransport::Tcp ? "TCP " : "UDP ") +
            std::to_string(port.internal_port) + " → " + std::to_string(port.external_port);
    if (!mapping.error.empty()) result.mapping += ". " + mapping.error;
    const auto port = std::to_string(network.listener.port);
    auto listener = network.listener.state;
    // Failed tasks retain their last snapshot after the session has unwound.
    if (listener == ListenerState::Listening && (snapshot.state == DownloadState::Failed ||
        snapshot.state == DownloadState::Stopped || snapshot.state == DownloadState::Complete)) listener = ListenerState::Stopped;
    switch (listener) {
    case ListenerState::Listening:
        result.port = u8"TCP-порт " + port + u8": локально слушается (" +
            (network.listener.ipv6 ? u8"IPv4 + IPv6" : "IPv4") + u8")."; break;
    case ListenerState::Stopped: result.port = u8"TCP-порт " + port + u8": закрыт после остановки сеанса."; break;
    case ListenerState::Error: result.port = u8"TCP-порт " + port + u8": ошибка локального прослушивания."; break;
    default: result.port = u8"Входящий TCP-порт будет проверен при запуске загрузки."; break;
    }
    result.port += u8"\nДоступность из интернета: не проверена.";
    const auto local = std::min(network.incoming_handshakes, network.loopback_handshakes);
    result.incoming = u8"Входящих BitTorrent за сеанс: " + std::to_string(network.incoming_handshakes) +
        u8" · с этого ПК: " + std::to_string(local) + u8" · с других адресов: " +
        std::to_string(network.incoming_handshakes - local);
    result.limits = u8"Лимиты торрента: ↓ " + rate(network.download_limit) + u8" · ↑ " + rate(network.upload_limit) +
        u8". Общие: ↓ " + rate(network.global_download_limit) + u8" · ↑ " + rate(network.global_upload_limit) + ".";

    switch (snapshot.state) {
    case DownloadState::Loading:
        result.summary = u8"Получение метаданных…";
        result.advice = u8"Дождитесь чтения .torrent или ответа пиров с метаданными magnet-ссылки."; return result;
    case DownloadState::Stopping:
        result.summary = u8"Закрытие сетевых подключений…";
        result.advice = u8"Дождитесь сохранения проверенных частей и завершения сетевых запросов."; return result;
    case DownloadState::Stopped: case DownloadState::Complete:
        result.summary = snapshot.state == DownloadState::Complete ? u8"Загрузка завершена · сеть остановлена" : u8"Сеть остановлена";
        result.advice = u8"Показаны результаты последнего сеанса. Для новой проверки запустите загрузку или раздачу."; return result;
    case DownloadState::Failed:
        result.summary = network.listener.state == ListenerState::Error ? u8"Не удалось открыть входящий TCP-порт" : u8"Загрузка завершилась с ошибкой";
        result.advice = network.listener.state == ListenerState::Error
            ? u8"Проверьте, не занят ли порт другим приложением и разрешён ли запуск системой. Можно изменить начальный порт в настройках сети и запустить торрент снова."
            : u8"Проверьте сообщение об ошибке и параметры загрузки.";
        return result;
    case DownloadState::Downloading: case DownloadState::Seeding: break;
    default:
        result.summary = u8"Сеть будет запущена после начала загрузки";
        result.advice = u8"Нажмите «Скачать», чтобы проверить локальный порт и начать поиск пиров."; return result;
    }

    const auto connected = std::count_if(network.peers.begin(), network.peers.end(),
        [](const auto& peer) { return peer.state == PeerState::Connected; });
    if (snapshot.state == DownloadState::Seeding) {
        result.summary = u8"Раздача · подключений: " + std::to_string(connected);
        result.advice = u8"Отдача начинается, когда другие пиры запрашивают проверенные части.";
    } else if (snapshot.download_speed > 0) {
        result.summary = u8"Скачивание · пиров: " + std::to_string(connected);
        result.advice = u8"Данные поступают. При низкой скорости проверьте общий лимит и лимит этого торрента.";
    } else if (connected) {
        if (std::any_of(network.peers.begin(), network.peers.end(), [](const auto& peer) { return peer.pending_requests > 0; })) {
            result.summary = u8"Ожидание запрошенных блоков · пиров: " + std::to_string(connected);
            result.advice = u8"Запросы уже отправлены. Дождитесь ответа пиров; при долгом ожидании проверьте соединение и лимиты скорости.";
        } else if (std::all_of(network.peers.begin(), network.peers.end(), [](const auto& peer) {
            return peer.state != PeerState::Connected || peer.choked;
        })) {
            result.summary = u8"Пиры пока не разрешают скачивание";
            result.advice = u8"Ожидайте разрешения от пиров. Другие найденные пиры будут подключаться автоматически.";
        } else {
            result.summary = u8"Пиры подключены · ожидание данных";
            result.advice = u8"У подключённых пиров могут отсутствовать нужные части. Дождитесь других источников.";
        }
    } else if (!network.peers.empty() || network.queued_peers) {
        result.summary = u8"Установка подключения к найденным пирам…";
        result.advice = u8"Идёт TCP-подключение или обмен приветствиями. Неудачные попытки отображаются ниже.";
    } else if (network.tracker.state == TrackerState::Announcing) {
        result.summary = network.tracker.attempt > 1 ? u8"Запрос к резервному трекеру…" : u8"Запрос пиров у трекера…";
        result.advice = u8"Дождитесь ответа. При ошибке или таймауте приложение попробует резервные адреса из торрента.";
    } else if (network.failed_connections > network.failed_incoming_handshakes && network.tracker.peers_returned) {
        result.summary = u8"Не удалось подключиться к найденным пирам";
        result.advice = u8"Проверьте интернет-соединение и последнюю ошибку подключения. Адреса пиров могут быть устаревшими; поиск продолжится.";
    } else if (network.tracker.state == TrackerState::Error) {
        result.summary = network.dht_enabled ? u8"Ошибка трекера · DHT включён" : u8"Трекеры не ответили";
        result.advice = u8"Приложение повторит запрос. Проверьте адрес трекера, соединение и текст последней ошибки.";
    } else if (network.dht_enabled && network.dht_nodes == 0) {
        result.summary = u8"Поиск узлов DHT…";
        result.advice = u8"Ответов от узлов DHT пока нет. Дождитесь начального поиска; при долгом ожидании проверьте доступность UDP в вашей сети.";
    } else if (network.web_seeds) {
        result.summary = u8"Ожидание HTTP-источников…";
        result.advice = u8"В торренте есть HTTP-источники. Дождитесь ответа сервера и проверьте лимиты скорости.";
    } else if (network.tracker.state == TrackerState::None && !network.dht_enabled) {
        result.summary = network.lsd_enabled ? u8"Поиск пиров в локальной сети…" : u8"Нет источника адресов пиров";
        result.advice = u8"Трекер не указан, DHT выключен. PEX получает адреса только от уже подключённых пиров. Проверьте настройки поиска или используйте торрент с рабочим трекером.";
    } else {
        result.summary = u8"Доступные пиры пока не найдены";
        result.advice = network.tracker.state == TrackerState::Working && network.tracker.peers_returned == 0
            ? u8"Трекер ответил без адресов пиров. Дождитесь следующего поиска; в раздаче может не быть доступных участников."
            : u8"Поиск работает, но адресов пиров пока нет. Дождитесь ответа трекера или DHT.";
    }
    if (network.max_peers && network.peers.size() >= network.max_peers)
        result.advice += u8" Достигнут лимит пиров: " + std::to_string(network.max_peers) +
            u8". Изменения лимита в настройках сети применяются при следующем запуске торрента.";
    return result;
}

std::string networkReport(const DownloadSnapshot& snapshot) {
    const auto diagnosis = diagnoseNetwork(snapshot);
    const auto& network = snapshot.network;
    std::ostringstream report;
    report << u8"Диагностика TorrentClient\nТоррент: " << snapshot.name << '\n' << diagnosis.summary << '\n'
           << diagnosis.port << '\n' << diagnosis.incoming << '\n' << diagnosis.mapping << '\n' << diagnosis.advice << '\n' << diagnosis.limits << '\n'
           << u8"Роутер: " << network.port_mapping.gateway << u8" · внешний адрес: " << network.port_mapping.external_address << '\n'
           << u8"Исходящих BitTorrent за сеанс: " << network.outgoing_handshakes
           << u8"\nОтклонено по лимиту пиров: " << network.rejected_incoming
           << u8"\nОшибок входящего приветствия: " << network.failed_incoming_handshakes
           << u8"\nDHT: " << enabled(network.dht_enabled) << u8" · узлов: " << network.dht_nodes
           << u8"\nPEX: " << enabled(network.pex_enabled) << u8" · LSD: " << enabled(network.lsd_enabled)
           << u8"\nHTTP-источников: " << network.web_seeds << u8"\nАдресов в очереди: " << network.queued_peers
           << u8"\nТрекер: " << trackerHost(network.tracker.url) << u8" · " << trackerState(network.tracker.state)
           << u8"\nАдресов в ответе трекера: " << network.tracker.peers_returned;
    if (!network.listener.error.empty()) report << u8"\nОшибка локального порта: " << network.listener.error;
    if (!network.tracker.error.empty()) report << u8"\nОшибка трекера: " << network.tracker.error;
    if (!network.last_peer_error.empty()) report << u8"\nОшибка подключения: " << network.last_peer_error;
    if (!snapshot.error.empty()) report << u8"\nОшибка загрузки: " << snapshot.error;
    report << u8"\nВходящие подключения с других адресов могут приходить из локальной сети. Проверку из интернета нужно выполнять из другой сети.\n";
    return report.str();
}

} // namespace torrent::app
