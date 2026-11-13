#include "tracker_diagnostics.h"
#include "download_task.h"
#include <ctime>
#include <iomanip>
#include <sstream>

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

std::string trackerTime(int64_t milliseconds) {
    if (milliseconds <= 0) return u8"—";
    const auto seconds = static_cast<std::time_t>(milliseconds / 1000);
    std::tm local{};
#ifdef _WIN32
    if (localtime_s(&local, &seconds)) return u8"—";
#else
    if (!localtime_r(&seconds, &local)) return u8"—";
#endif
    std::ostringstream output;
    output << std::put_time(&local, "%Y-%m-%d %H:%M:%S");
    return output.str();
}

int64_t trackerWaitSeconds(std::chrono::steady_clock::time_point deadline) {
    const auto now = std::chrono::steady_clock::now();
    return deadline <= now ? 0 : std::chrono::ceil<std::chrono::seconds>(deadline - now).count();
}

} // namespace torrent::app
