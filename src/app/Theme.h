#pragma once

#include "imgui.h"
#include "platform/Appearance.h"

namespace clipper {

// colours imgui's style doesn't have a slot for. the look: neutral greys, the window background behind
// rounded cards with hairline borders, selection = a soft raised pill. the accent only for small things
struct ThemeColors {
    ImU32 folderIcon;    // folders in the list
    ImU32 fileIcon;      // outline icons (text clips, sidebar)
    ImU32 selectedText;  // text on a selected row
    ImVec4 dimText;      // secondary text: ages, sizes, hints
    ImU32 faint;         // section labels, key hints
    ImU32 panel;         // cards (list, details, popup)
    ImU32 raised;        // selected rows, inputs, buttons
    ImU32 hover;         // rows under the mouse
    ImU32 hairline;      // card and selection borders
    ImU32 pill;          // count badges
    ImU32 accent;
    bool dark;
};

// scale = monitor content scale
void applyTheme(const SystemAppearance& appearance, float scale);
void loadFonts(const SystemAppearance& appearance, float scale);

const ThemeColors& themeColors();

}  // namespace clipper
