#pragma once

#include "config/model_access.hpp"
#include "config/model_catalog.hpp"
#include "sessions/session_info.hpp"
#include "ui/terminal_panel.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ui
{
// Modal for starting a fresh `claude` in any directory: recent project
// folders, a native folder picker (kdialog/zenity), permission mode, model,
// --continue and an optional first message.
class NewSessionDialog
{
public:
    NewSessionDialog();
    ~NewSessionDialog();

    void Open(const std::string& cwd = {});
    // Returns the launch spec on the frame the user confirms.
    std::optional<LaunchSpec> Draw(const std::vector<sessions::SessionInfo>& sessions,
                                   const config::ModelCatalog& models, const config::ModelAccess& access,
                                   bool showLegacyModels);

private:
    struct PickerState; // shared with the background picker thread

    void StartPicker();
    void RefreshRecent(const std::vector<sessions::SessionInfo>& sessions);

    bool        openRequested_ = false;
    std::string cwd_;
    int         permissionIndex_ = 0;
    std::string model_;
    std::string prompt_;
    bool        continueLast_ = false;
    struct RecentProject
    {
        std::string path;
        int         sessions = 0;
        std::string lastActive; // ISO timestamp of the newest session there
    };
    std::vector<RecentProject> recent_;
    std::shared_ptr<PickerState> picker_;
};
} // namespace ui
