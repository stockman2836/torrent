#include "tracker_diagnostics.h"
#include "download_task.h"

namespace torrent::app {

bool canUpdateTrackers(const DownloadSnapshot& snapshot) {
    const auto& tracker = snapshot.network.tracker;
    return (snapshot.state == DownloadState::Downloading || snapshot.state == DownloadState::Seeding) &&
        tracker.total_trackers && !tracker.retry_pending &&
        (tracker.state == TrackerState::Working || tracker.state == TrackerState::Error) &&
        std::chrono::steady_clock::now() >= tracker.retry_after;
}

std::string trackerStateText(TrackerState state) {
    switch (state) {
    case TrackerState::Waiting: return u8"Ожидает запроса";
    case TrackerState::Announcing: return u8"Ожидание ответа";
    case TrackerState::Working: return u8"Ответ получен";
    case TrackerState::Error: return u8"Ошибка";
    default: return u8"Трекер не указан";
    }
}

std::string trackerEventText(const TrackerEvent& event) {
    std::string name = event.event == "started" ? u8"Запуск" : event.event == "completed" ? u8"Завершение" :
        event.event == "stopped" ? u8"Остановка" : u8"Обновление";
    return name + u8" · " + (event.result == TrackerEventResult::Cancelled ? u8"отменено" :
        event.result == TrackerEventResult::Success ? u8"ответ получен" : u8"ошибка");
}

int64_t trackerWaitSeconds(std::chrono::steady_clock::time_point deadline) {
    const auto now = std::chrono::steady_clock::now();
    return deadline <= now ? 0 : std::chrono::ceil<std::chrono::seconds>(deadline - now).count();
}

} // namespace torrent::app
