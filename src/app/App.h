#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "app/PreferencesDialog.h"
#include "app/Settings.h"
#include "app/Thumbnails.h"
#include "core/ClipStore.h"
#include "core/Filter.h"
#include "platform/AppBus.h"
#include "platform/Appearance.h"
#include "platform/Tray.h"
#include "platform/X11Clipboard.h"
#include "platform/X11Desktop.h"

struct ImVec2;

namespace clipper {

// the whole app. one glfw window that's either the popup (Ctrl+Shift+V, at the mouse, pick and paste)
// or the manager (organize the history: pin, folders, titles, edit, delete). main() does the
// window juggling, this decides what and draws it
class App {
public:
    // wake: thread safe, pokes the main loop (glfwPostEmptyEvent)
    App(const SystemAppearance& system, float displayScale, std::function<void()> wake, X11Desktop& desktop);
    ~App();
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    void setOwnWindow(X11Desktop::WindowId window) { ownWindow_ = window; }
    void start(bool background);
    void remote(const AppBus::Request& request);  // another launch / the shortcut
    void tick();                                  // every loop iteration, visible or not
    void draw();                                  // only while visible

    enum class Mode { Hidden, Popup, Manager };
    enum class WindowRequest { None, Popup, Manager, ToggleManager, Hide };
    WindowRequest takeWindowRequest();
    void windowChanged(Mode mode);  // main() did it. Hidden after a pick = time to paste
    Mode mode() const { return mode_; }
    void focusChanged(bool focused);
    bool popupAnchor(int* x, int* y) const;  // where the pointer was when the popup got asked for

    bool hideOnClose() const;
    bool quitRequested() const { return quit_; }
    bool consumeFontRebuild();
    const Settings& settings() const { return settings_; }
    const WindowGeometry& windowGeometry() const { return settings_.window; }
    void rememberWindowGeometry(const WindowGeometry& geometry);
    SystemAppearance appearance() const;
    float fontScale() const { return displayScale_ * settings_.fontZoom; }

private:
    // capture + clipboard
    void takeCaptures();
    bool excludedApp(const std::string& source) const;
    bool putOnClipboard(const Clip& clip, bool primaryToo);
    void pick(uint64_t id, bool paste);  // popup: clip chosen
    void pasteNow();
    void copyToClipboard(uint64_t id);  // manager: Enter / Copy
    void setCapturing(bool on);
    void updateTray();
    void applyPreferences(Settings next);
    void deleteClip(uint64_t id);

    void refilter();
    const Clip* clipAt(const std::vector<uint64_t>& ids, int row) const;

    // popup
    void openPopup();
    void drawPopup();
    void popupKeys();

    // manager
    void drawManager();
    void drawMenuBar();
    void drawSidebar();
    void drawClipList();
    void drawDetails();
    void drawStatusBar();
    void drawModals();
    void drawClipContextMenu(const Clip& clip);
    void managerKeys();
    void selectClip(uint64_t id);
    void dropTarget(int view, uint32_t folder);  // a clip dragged onto the sidebar

    // shared drawing
    void drawKindIcon(const Clip& clip, ImVec2 pos, float size, bool selected);
    void drawPreview(const Clip& clip, float height);  // text / image / file list
    void setStatus(std::string message);
    void setZoom(float zoom);

    std::function<void()> wake_;
    X11Desktop& desktop_;
    X11Desktop::WindowId ownWindow_ = 0;

    Settings settings_;
    SystemAppearance systemAppearance_;
    float displayScale_ = 1.0f;
    ClipStore store_;
    std::string storeError_;
    std::unique_ptr<X11Clipboard> clipboard_;
    std::unique_ptr<Tray> tray_;
    std::unique_ptr<Thumbnails> thumbs_;
    PreferencesDialog preferences_;

    Mode mode_ = Mode::Hidden;
    WindowRequest windowRequest_ = WindowRequest::None;
    bool waitingForTray_ = false;
    std::chrono::steady_clock::time_point startTime_;
    bool fontsDirty_ = false;
    bool quit_ = false;
    uint64_t seenRevision_ = 0;

    // popup state. target = the window that had focus when the shortcut got pressed
    ClipFilter popupFilter_;
    std::vector<uint64_t> popupResults_;
    int popupRow_ = 0;
    bool popupFocusSearch_ = false;
    bool popupHadFocus_ = false;
    X11Desktop::WindowId target_ = 0;
    int anchorX_ = 0, anchorY_ = 0;
    bool haveAnchor_ = false;
    struct PendingPaste {
        bool active = false;
        X11Desktop::WindowId window = 0;
        bool terminal = false;
    } pendingPaste_;

    // manager state
    ClipFilter managerFilter_;
    std::vector<uint64_t> managerResults_;
    uint64_t selectedId_ = 0;
    bool scrollToSelected_ = false;
    bool focusManagerSearch_ = false;
    unsigned int managerSearchId_ = 0;  // ImGuiID of the search box, arrows still drive the list while it's active
    size_t countAll_ = 0, countPinned_ = 0;
    std::vector<size_t> folderCounts_;  // same order as store_.folders()

    // details pane edits
    uint64_t editingId_ = 0;
    std::string titleEdit_;
    std::string textEdit_;
    bool focusTitle_ = false;

    // modals
    bool openClearConfirm_ = false;
    bool openFolderName_ = false;
    uint32_t renamingFolder_ = 0;    // 0 = new folder
    uint64_t moveAfterCreate_ = 0;   // "New folder..." from a clip's menu puts it in there
    std::string folderNameEdit_;
    bool openAbout_ = false;
    bool openShortcuts_ = false;

    std::string statusMessage_;
    std::chrono::steady_clock::time_point statusTime_;
};

}  // namespace clipper
