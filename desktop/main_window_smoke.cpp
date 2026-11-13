#include "main_window.h"
#include "win32_helpers.h"
#include <commctrl.h>
#include <algorithm>
#include <cwchar>
#include <stdexcept>

namespace torrent::desktop {
using namespace app;
namespace fs = std::filesystem;

SmokeTest parseSmokeTest(int argc, wchar_t* argv[]) {
    SmokeTest result;
    if (argv && argc == 5 && (std::wstring(argv[1]) == L"--ui-test" || std::wstring(argv[1]) == L"--ui-test-priorities" ||
                             std::wstring(argv[1]) == L"--ui-test-global-limits" || std::wstring(argv[1]) == L"--ui-test-network")) {
        result.enabled = true; result.source = argv[2]; result.folder = argv[3]; result.config = argv[4];
        result.priorities = std::wstring(argv[1]) == L"--ui-test-priorities";
        result.global_limits = std::wstring(argv[1]) == L"--ui-test-global-limits";
        result.network = std::wstring(argv[1]) == L"--ui-test-network";
    }
    if (argv && argc == 6 && std::wstring(argv[1]) == L"--ui-queue-test") {
        result.enabled = true; result.source = argv[2]; result.second = argv[3];
        result.folder = argv[4]; result.config = argv[5];
    }
    if (argv && argc == 7 && std::wstring(argv[1]) == L"--ui-test-port-mapping") {
        const auto port = [](const wchar_t* value) -> uint16_t {
            wchar_t* end = nullptr; const auto number = std::wcstoul(value, &end, 10);
            return end != value && !*end && number > 0 && number <= 65535 ? static_cast<uint16_t>(number) : 0;
        };
        result.nat_port = port(argv[5]); result.ssdp_port = port(argv[6]);
        if (!result.nat_port || !result.ssdp_port) throw std::invalid_argument("Invalid loopback fixture port");
        result.enabled = result.mapping = true;
        result.source = argv[2]; result.folder = argv[3]; result.config = argv[4];
    }
    return result;
}

void MainWindow::configureSmoke() {
    if (smoke_.enabled) {
        config_.enable_port_mapping = false; // Smoke fixtures never inherit live router opt-in from user preferences.
        config_.loadFromFile(utf8(smoke_.config));
        if (smoke_.mapping) {
            config_.port_mapping_options.gateway = "127.0.0.1";
            config_.port_mapping_options.discovery_address = "127.0.0.1";
            config_.port_mapping_options.natpmp_port = smoke_.nat_port;
            config_.port_mapping_options.discovery_port = smoke_.ssdp_port;
            config_.port_mapping_options.lease_seconds = 2;
            config_.port_mapping_options.request_timeout_ms = 250;
            config_.port_mapping_options.discovery_timeout_ms = 500;
            config_.port_mapping_options.retry_delay_ms = 1000;
        }
        settings_.clear();
        if (smoke_.global_limits || smoke_.mapping) settings_ = fs::path(smoke_.config);
        listPath_.clear();
        config_.download_dir = utf8(smoke_.folder);
        SetWindowTextW(source_, smoke_.source.c_str());
    }
}

void MainWindow::startSmoke() {
    if (smoke_.enabled) {
        smoke_.deadline = GetTickCount64() + 45000;
        if (smoke_.second.empty()) load();
        else {
            selected_ = queue_.add(utf8(smoke_.source), config_);
            queue_.add(utf8(smoke_.second), config_);
            SetWindowTextW(source_, L"");
        }
    }
}

void MainWindow::runSmoke(const DownloadSnapshot& state, bool busy) {
    if (!smoke_.enabled) return;
    if (!smoke_.second.empty()) { smokeQueue(); return; }
    if (!busy && state.state == DownloadState::Ready && !smoke_.started) {
        if (ListView_GetItemCount(files_) == 0) { exitCode_ = 1; close(); }
        else {
            smoke_.started = true;
            smoke_.started_at = GetTickCount64();
            networkSettings();
            SendMessageW(handle_, WM_COMMAND, Start, 0);
        }
    } else if (smoke_.mapping && !smoke_.mapping_checked && busy && state.network.port_mapping.state == MappingState::Active) {
        smoke_.mapping_checked = true;
        networkDetails();
    } else if (smoke_.network && !smoke_.network_checked && busy &&
               state.network.listener.state == ListenerState::Listening && state.state == DownloadState::Downloading) {
        smoke_.network_checked = true;
        networkDetails();
    } else if (smoke_.global_limits && !smoke_.global_limits_applied && busy &&
               state.state == DownloadState::Downloading && GetTickCount64() >= smoke_.started_at + 1500) {
        smoke_.global_limits_applied = true;
        if (state.progress != 0) throw std::runtime_error("Saved global limit did not apply at startup");
        SendMessageW(handle_, WM_COMMAND, GlobalSpeedLimits, 0);
    } else if (smoke_.priorities && !smoke_.priorities_applied && busy && state.progress > 0 &&
               state.state == DownloadState::Downloading) {
        smoke_.priorities_applied = true;
        const auto count = ListView_GetItemCount(files_);
        if (count < 3 || !IsWindowEnabled(apply_)) { exitCode_ = 1; close(); return; }
        ListView_SetCheckState(files_, 1, FALSE);
        ListView_SetItemState(files_, count - 1, LVIS_SELECTED, LVIS_SELECTED);
        SendMessageW(priority_, CB_SETCURSEL, 1, 0);
        SendMessageW(handle_, WM_COMMAND, Apply, 0);
    } else if (!busy && state.state == DownloadState::Complete) {
        if (smoke_.priorities && !smoke_.priorities_applied) exitCode_ = 1;
        if (smoke_.global_limits && !smoke_.global_limits_applied) exitCode_ = 1;
        if (smoke_.network && !smoke_.network_checked) exitCode_ = 1;
        if (smoke_.mapping && !smoke_.mapping_checked) exitCode_ = 1;
        networkDetails(); close();
    }
    else if (state.state == DownloadState::Failed || GetTickCount64() > smoke_.deadline) { exitCode_ = 1; close(); }
}

void MainWindow::smokeQueue() {
    if (GetTickCount64() > smoke_.deadline || std::any_of(entries_.begin(), entries_.end(), [](const auto& entry) {
        return entry.download.state == DownloadState::Failed;
    })) { exitCode_ = 1; close(); return; }
    if (!smoke_.started) {
        if (entries_.size() != 2 || std::any_of(entries_.begin(), entries_.end(), [](const auto& entry) { return entry.busy; })) return;
        smoke_.started = true; // Command handlers refresh the window recursively.
        networkSettings();
        SendMessageW(handle_, WM_COMMAND, SeedSettings, 0);
        SetWindowTextW(downloadLimit_, L"32"); SetWindowTextW(uploadLimit_, L"16");
        SendMessageW(handle_, WM_COMMAND, ApplyLimits, 0);
        ListView_SetItemState(files_, 0, LVIS_SELECTED, LVIS_SELECTED);
        SendMessageW(priority_, CB_SETCURSEL, 1, 0);
        SendMessageW(handle_, WM_COMMAND, Apply, 0);
        SendMessageW(handle_, WM_COMMAND, Start, 0);
        ListView_SetItemState(jobs_, 1, LVIS_SELECTED, LVIS_SELECTED);
        SetWindowTextW(downloadLimit_, L"64"); SetWindowTextW(uploadLimit_, L"0");
        SendMessageW(handle_, WM_COMMAND, ApplyLimits, 0);
        ListView_SetItemState(files_, 0, LVIS_SELECTED, LVIS_SELECTED);
        SendMessageW(priority_, CB_SETCURSEL, 2, 0);
        SendMessageW(handle_, WM_COMMAND, Apply, 0);
        SendMessageW(handle_, WM_COMMAND, Start, 0);
        const auto entries = queue_.snapshots();
        if (entries[0].config.file_priorities.at(0) != FilePriority::High ||
            entries[1].config.file_priorities.at(0) != FilePriority::Low || !entries[1].queued ||
            entries[0].config.max_download_speed != 32768 || entries[0].config.max_upload_speed != 16384 ||
            entries[1].config.max_download_speed != 65536 || entries[1].config.max_upload_speed != 0) {
            exitCode_ = 1; close();
        }
    } else if (std::all_of(entries_.begin(), entries_.end(), [](const auto& entry) {
        return !entry.busy && entry.download.state == DownloadState::Complete;
    })) { networkDetails(); close(); }
}

} // namespace torrent::desktop
