#pragma once

#include "win32.h"
#include "download_queue.h"
#include "smoke_test.h"
#include <filesystem>
#include <utility>

namespace torrent::desktop {

// The window owns UI state and an application queue. Engine work stays in app/session.
class MainWindow {
public:
    explicit MainWindow(SmokeTest smoke) : smoke_(std::move(smoke)) {}
    ~MainWindow();
    bool hidden() const { return smoke_.enabled; }
    static LRESULT CALLBACK procedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

private:
    enum Control { Source = 100, Browse, Read, Folder, ChooseFolder, Files, SelectAll, SelectNone,
        Priority, Apply, Start, Stop, OpenFolder, Jobs, Remove, Limit, DownloadLimit, UploadLimit,
        ApplyLimits, SeedSettings, NetworkDetails, EditNetworkSettings, GlobalSpeedLimits };

    HWND control(const wchar_t* cls, const wchar_t* label, DWORD style, int id = 0);
    void create();
    void createControls();
    void initializeDownloads();
    void layout();
    void browse();
    void chooseFolder();
    void load();
    const app::QueueSnapshot* current() const;
    bool editable() const;
    bool prioritiesEditable() const;
    void saveEdits();
    void persist();
    void applySpeedLimits();
    void start();
    void seedingSettings();
    void networkSettings();
    void globalSpeedLimits();
    void populate(const app::DownloadSnapshot& snapshot, const Config& config);
    void networkDetails();
    void refresh();
    const wchar_t* jobState(const app::QueueSnapshot& entry) const;
    void refreshJobs();
    void close();
    LRESULT dispatch(UINT message, WPARAM wparam, LPARAM lparam);

    // Explicit test hooks, implemented separately from normal rendering/actions.
    void configureSmoke();
    void startSmoke();
    void runSmoke(const app::DownloadSnapshot& state, bool busy);
    void smokeQueue();

    app::DownloadQueue queue_;
    Config config_;
    std::filesystem::path settings_, listPath_;
    HWND handle_ = nullptr;
    HWND title_ = nullptr, subtitle_ = nullptr, source_ = nullptr, browse_ = nullptr, read_ = nullptr,
        folderLabel_ = nullptr, folder_ = nullptr, choose_ = nullptr, files_ = nullptr,
        all_ = nullptr, none_ = nullptr, priority_ = nullptr, apply_ = nullptr, summary_ = nullptr,
        progress_ = nullptr, status_ = nullptr, speeds_ = nullptr, start_ = nullptr, stop_ = nullptr,
        open_ = nullptr, jobs_ = nullptr, jobsLabel_ = nullptr, remove_ = nullptr, limit_ = nullptr,
        limitLabel_ = nullptr, rateLabel_ = nullptr, downloadLabel_ = nullptr, uploadLabel_ = nullptr,
        downloadLimit_ = nullptr, uploadLimit_ = nullptr, applyLimits_ = nullptr, seedSettings_ = nullptr,
        networkStatus_ = nullptr, networkDetails_ = nullptr, networkSettings_ = nullptr, globalSpeedLimits_ = nullptr;
    HFONT font_ = nullptr, heading_ = nullptr;
    std::shared_ptr<const std::vector<FileInfo>> displayed_;
    std::vector<FilePriority> priorities_;
    bool dialogOpen_ = false, filling_ = false, fillingJobs_ = false, closing_ = false, dirty_ = false;
    app::DownloadId selected_ = 0, rendered_ = 0;
    std::vector<app::QueueSnapshot> entries_;
    app::DownloadState previous_ = app::DownloadState::Idle;
    SmokeTest smoke_;
    int exitCode_ = 0;
};

} // namespace torrent::desktop
