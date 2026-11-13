#include "torrent/session/download_manager.h"
#include "torrent/network/peer/extension_protocol.h"
#include "torrent/network/peer/metadata_responder.h"

namespace torrent {

void DownloadManager::configurePeerExtensions(PeerConnection& connection) {
    if (!connection.peerSupportsExtensions()) return;
    auto& extensions = connection.extensions();
    MetadataResponder responder(torrent_.metadata());
    extensions.setMetadataSize(static_cast<int64_t>(responder.metadataSize()));
    extensions.registerExtension(EXT_NAME_METADATA,
        [this, &connection, responder](uint8_t, const std::vector<uint8_t>& request) mutable {
            const auto id = connection.extensions().getPeerExtensionId(EXT_NAME_METADATA);
            if (!id) return;
            const auto response = responder.reply(request);
            if (response.empty()) return;
            if (!upload_limiter_.waitAndConsume(response.size(), [this] { return !running_.load(); },
                    global_limits_ ? &global_limits_->upload : nullptr)) return;
            // Metadata uses the upload budgets but never contributes to payload
            // totals or seeding ratio. The peer worker owns extension state.
            if (!connection.sendExtendedMessage(id, response)) connection.disconnect();
        });
    if (enable_pex_) connection.enablePex();
    connection.sendExtendedHandshake();
}

} // namespace torrent
