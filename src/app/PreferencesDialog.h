#pragma once

#include <string>

#include "app/Settings.h"

namespace clipper {

// edits a copy, nothing changes until OK
class PreferencesDialog {
public:
    enum class Result { None, Ok, Cancel };

    void open(const Settings& current);
    Result draw();  // every frame, returns Ok once on confirm
    const Settings& edited() const { return draft_; }

private:
    void drawGeneralTab();
    void drawHistoryTab();
    void drawAppearanceTab();

    void drawShortcutRecorder();

    bool requestOpen_ = false;
    bool recording_ = false;  // waiting for a key combo
    Settings draft_;
    std::string appInput_;
};

}  // namespace clipper
