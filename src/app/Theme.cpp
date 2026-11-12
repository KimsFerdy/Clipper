#include "app/Theme.h"

#include <cmath>

namespace clipper {

namespace {

ThemeColors g_colors;

ImVec4 hex(unsigned rgb, float a = 1.0f) {
    return ImVec4(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, a);
}

ImVec4 mix(const ImVec4& a, const ImVec4& b, float t) {
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}

ImVec4 alpha(ImVec4 c, float a) {
    c.w = a;
    return c;
}

ImU32 u32(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }

// neutral greys, a touch cool. bg = the window (sidebar sits on it), panel = cards, raised = what's picked
struct Palette {
    ImVec4 bg, panel, raised, hover, hairline, input, text, dim, faint;
};

Palette darkPalette() {
    return {hex(0x151517), hex(0x1C1C1F), hex(0x2A2A2F), hex(0x232327), hex(0x2C2C31),
            hex(0x18181B), hex(0xE9E9EC), hex(0x8E8E96), hex(0x65656D)};
}

Palette lightPalette() {
    return {hex(0xEDEDEF), hex(0xF7F7F8), hex(0xFFFFFF), hex(0xE7E7EA), hex(0xDADADF),
            hex(0xFFFFFF), hex(0x1D1D1F), hex(0x77777F), hex(0xA2A2AA)};
}

}  // namespace

void applyTheme(const SystemAppearance& ap, float scale) {
    ImGuiStyle style;
    if (ap.dark)
        ImGui::StyleColorsDark(&style);
    else
        ImGui::StyleColorsLight(&style);

    style.WindowPadding = ImVec2(14, 14);
    style.FramePadding = ImVec2(10, 7);
    style.ItemSpacing = ImVec2(8, 8);
    style.ItemInnerSpacing = ImVec2(8, 6);
    style.CellPadding = ImVec2(10, 5);
    style.WindowRounding = 12;  // modals
    style.ChildRounding = 12;   // cards
    style.FrameRounding = 8;
    style.PopupRounding = 10;
    style.ScrollbarRounding = 8;
    style.GrabRounding = 6;
    style.TabRounding = 8;
    style.WindowBorderSize = 1;
    style.ChildBorderSize = 1;
    style.FrameBorderSize = 1;
    style.PopupBorderSize = 1;
    style.TabBorderSize = 0;
    style.ScrollbarSize = 8;
    style.GrabMinSize = 12;
    style.SeparatorTextBorderSize = 1;
    style.WindowTitleAlign = ImVec2(0.5f, 0.5f);

    const Palette p = ap.dark ? darkPalette() : lightPalette();
    const ImVec4 accent(ap.accent[0], ap.accent[1], ap.accent[2], 1.0f);
    const ImVec4 clear(0, 0, 0, 0);

    ImVec4* c = style.Colors;
    c[ImGuiCol_Text] = p.text;
    c[ImGuiCol_TextDisabled] = p.dim;
    c[ImGuiCol_WindowBg] = p.bg;
    c[ImGuiCol_ChildBg] = clear;  // cards set theirs where they're drawn
    c[ImGuiCol_PopupBg] = p.panel;
    c[ImGuiCol_Border] = p.hairline;
    c[ImGuiCol_BorderShadow] = clear;
    c[ImGuiCol_FrameBg] = p.input;
    c[ImGuiCol_FrameBgHovered] = mix(p.input, p.text, 0.03f);
    c[ImGuiCol_FrameBgActive] = p.input;
    c[ImGuiCol_TitleBg] = p.panel;
    c[ImGuiCol_TitleBgActive] = p.panel;
    c[ImGuiCol_TitleBgCollapsed] = p.panel;
    c[ImGuiCol_MenuBarBg] = p.bg;
    c[ImGuiCol_ScrollbarBg] = clear;
    c[ImGuiCol_ScrollbarGrab] = alpha(p.dim, 0.30f);
    c[ImGuiCol_ScrollbarGrabHovered] = alpha(p.dim, 0.50f);
    c[ImGuiCol_ScrollbarGrabActive] = alpha(p.dim, 0.70f);
    c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = accent;
    c[ImGuiCol_Button] = p.raised;
    c[ImGuiCol_ButtonHovered] = mix(p.raised, p.text, 0.06f);
    c[ImGuiCol_ButtonActive] = mix(p.raised, p.text, 0.12f);
    c[ImGuiCol_Header] = p.raised;  // selected: a raised pill, not an accent fill
    c[ImGuiCol_HeaderHovered] = p.hover;
    c[ImGuiCol_HeaderActive] = p.raised;
    c[ImGuiCol_Separator] = p.hairline;
    c[ImGuiCol_SeparatorHovered] = p.hairline;
    c[ImGuiCol_SeparatorActive] = accent;
    c[ImGuiCol_ResizeGrip] = clear;
    c[ImGuiCol_ResizeGripHovered] = clear;
    c[ImGuiCol_ResizeGripActive] = clear;
    c[ImGuiCol_Tab] = clear;
    c[ImGuiCol_TabHovered] = p.hover;
    c[ImGuiCol_TabSelected] = p.raised;
    c[ImGuiCol_TabSelectedOverline] = clear;
    c[ImGuiCol_TabDimmed] = clear;
    c[ImGuiCol_TabDimmedSelected] = p.raised;
    c[ImGuiCol_TabDimmedSelectedOverline] = clear;
    c[ImGuiCol_TableHeaderBg] = p.panel;
    c[ImGuiCol_TableBorderStrong] = p.hairline;
    c[ImGuiCol_TableBorderLight] = p.hairline;
    c[ImGuiCol_TableRowBg] = clear;
    c[ImGuiCol_TableRowBgAlt] = clear;
    c[ImGuiCol_TextSelectedBg] = alpha(accent, 0.30f);
    c[ImGuiCol_DragDropTarget] = accent;
    c[ImGuiCol_NavCursor] = alpha(accent, 0.6f);
    c[ImGuiCol_ModalWindowDimBg] = ap.dark ? ImVec4(0, 0, 0, 0.45f) : ImVec4(0.1f, 0.1f, 0.12f, 0.18f);

    style.ScaleAllSizes(scale);
    ImGui::GetStyle() = style;

    g_colors.folderIcon = u32(mix(p.dim, accent, 0.55f));
    g_colors.fileIcon = u32(p.dim);
    g_colors.selectedText = u32(p.text);
    g_colors.dimText = p.dim;
    g_colors.faint = u32(p.faint);
    g_colors.panel = u32(p.panel);
    g_colors.raised = u32(p.raised);
    g_colors.hover = u32(p.hover);
    g_colors.hairline = u32(p.hairline);
    g_colors.pill = u32(ap.dark ? alpha(p.text, 0.07f) : alpha(p.text, 0.06f));
    g_colors.accent = u32(accent);
    g_colors.dark = ap.dark;
}

void loadFonts(const SystemAppearance& ap, float scale) {
    ImGuiIO& io = ImGui::GetIO();
    if (!ap.fontFile.empty()) {
        ImFontConfig cfg;
        cfg.OversampleH = 2;
        cfg.OversampleV = 1;
        const float sizePx = std::round(ap.fontSizePx * scale);
        // latin + greek + cyrillic + punctuation + arrows, anything else shows as '?' for now
        static const ImWchar ranges[] = {0x0020, 0x024F, 0x0370, 0x03FF, 0x0400, 0x052F, 0x2000, 0x206F, 0x2190, 0x21FF, 0};
        if (io.Fonts->AddFontFromFileTTF(ap.fontFile.c_str(), sizePx, &cfg, ranges)) return;
    }
    ImFontConfig cfg;
    cfg.SizePixels = std::round(13.0f * scale);
    io.Fonts->AddFontDefault(&cfg);
}

const ThemeColors& themeColors() { return g_colors; }

}  // namespace clipper
