// ReproVR setup — a small Direct2D front end for installing the layer into a game
// folder and editing that game's settings.
//
// Direct2D rather than a web view on purpose: the WebView2 runtime is present on
// this machine but its SDK is not, so a web UI would mean vendoring a whole SDK
// and shipping a browser engine next to a 1 MB layer. Direct2D and DirectWrite
// are part of Windows, the executable stays alone in its folder, and the flat
// look is drawn by hand.
//
// One geometry pass. layout() produces every rectangle — cards, controls,
// clickable cells — and paint() and hit_test() both read that same list, so a
// control can never be drawn in one place and clicked in another. Coordinates are
// device pixels: the render target is created at 96 DPI and layout() multiplies by
// the window's DPI exactly once.
//
// Every setting is a segmented control, and only that: a 2-option setting
// (on/off) and a 5-option setting use the same widget in the same place, so
// there is nothing to learn per row and no second way to click.
//
// `reprovr_setup.exe --self-check` lays out at the default size, verifies the
// geometry (cells inside their row, no overlap, centre of each control resolving
// to itself) and exits with the verdict, so the invariant above is testable
// without a mouse.

#include "installer.hpp"
#include "settings_model.hpp"

#include <windows.h>
#include <windowsx.h>
#include <d2d1.h>
#include <dwrite.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wrl/client.h>

#include <algorithm>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace reprovr::setup;

namespace {

constexpr wchar_t kWindowClass[] = L"ReproVRSetupWindow";
constexpr wchar_t kWindowTitle[] = L"ReproVR";

// Flat palette. One accent, one danger, everything else neutral.
const D2D1_COLOR_F kBackground = D2D1::ColorF(0xF5F6F8);
const D2D1_COLOR_F kSidebar = D2D1::ColorF(0xFFFFFF);
const D2D1_COLOR_F kCard = D2D1::ColorF(0xFFFFFF);
const D2D1_COLOR_F kCardHot = D2D1::ColorF(0xFBFCFF);
const D2D1_COLOR_F kBorder = D2D1::ColorF(0xE3E5E9);
const D2D1_COLOR_F kText = D2D1::ColorF(0x1B1D21);
const D2D1_COLOR_F kMuted = D2D1::ColorF(0x6B7280);
const D2D1_COLOR_F kAccent = D2D1::ColorF(0x2F6FED);
const D2D1_COLOR_F kAccentSoft = D2D1::ColorF(0xE8F0FE);
const D2D1_COLOR_F kTrack = D2D1::ColorF(0xF0F1F4);
const D2D1_COLOR_F kTrackHot = D2D1::ColorF(0xE4E7EC);
const D2D1_COLOR_F kDanger = D2D1::ColorF(0xD64545);
const D2D1_COLOR_F kTooltip = D2D1::ColorF(0x1F2328);
const D2D1_COLOR_F kOnDark = D2D1::ColorF(0xFFFFFF);

constexpr float kSidebarWidth = 268.0f;
constexpr float kPad = 22.0f;
constexpr float kCardRadius = 12.0f;
constexpr float kRowHeight = 62.0f;
constexpr float kRowGap = 8.0f;
constexpr float kHeaderHeight = 96.0f;
constexpr float kGameItem = 54.0f;
constexpr float kSegmentHeight = 30.0f;

enum class Action {
    none,
    select_game,
    remove_game,
    add_game,
    install,
    uninstall,
    choice,
};

// One clickable cell of a segmented control.
struct Cell {
    D2D1_RECT_F rect{};
    int option{};
};

// One setting row: the card, and the cells the user can hit on it.
struct Row {
    D2D1_RECT_F rect{};
    int setting{};
    int chosen{};
    std::vector<Cell> cells;
};

struct Button {
    D2D1_RECT_F rect{};
    Action action{Action::none};
    int game{};
};

// What sits under a point. Returned by hit_test() and by nothing else, so hover,
// cursor and click can never disagree about what is under the mouse.
struct Target {
    Action action{Action::none};
    int game{-1};
    int setting{-1};
    int option{-1};

    bool operator==(const Target& other) const {
        return action == other.action && game == other.game &&
               setting == other.setting && option == other.option;
    }
    bool clickable() const { return action != Action::none; }
};

struct App {
    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> dwrite;
    ComPtr<ID2D1HwndRenderTarget> target;
    ComPtr<ID2D1SolidColorBrush> brush;
    ComPtr<IDWriteTextFormat> title;
    ComPtr<IDWriteTextFormat> subtitle;
    ComPtr<IDWriteTextFormat> body;
    ComPtr<IDWriteTextFormat> caption;
    ComPtr<IDWriteTextFormat> tiny;

    HWND hwnd{};
    float dpi{96.0f};
    float width{1100.0f};
    float height{760.0f};

    std::vector<Game> games;
    int selected{};
    std::vector<Row> rows;
    std::vector<Button> buttons;
    float scroll{};
    float games_scroll{};
    float content_height{};

    std::wstring status;
    std::wstring tooltip;
    D2D1_RECT_F tooltip_anchor{};
    float mouse_x{}, mouse_y{};
    Target hot;

    float dp(float value) const { return value * dpi / 96.0f; }
};

App g_app;

// ---------------------------------------------------------------- resources ---

bool make_format(App& app, ComPtr<IDWriteTextFormat>& slot, float size,
                 DWRITE_FONT_WEIGHT weight) {
    ComPtr<IDWriteTextFormat> format;
    if (FAILED(app.dwrite->CreateTextFormat(
            L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, size * app.dpi / 96.0f, L"fr-fr",
            format.GetAddressOf()))) {
        return false;
    }
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    slot = format;
    return true;
}

bool create_text_formats(App& app) {
    return make_format(app, app.title, 21.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD) &&
           make_format(app, app.subtitle, 13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD) &&
           make_format(app, app.body, 13.5f, DWRITE_FONT_WEIGHT_NORMAL) &&
           make_format(app, app.caption, 12.0f, DWRITE_FONT_WEIGHT_NORMAL) &&
           make_format(app, app.tiny, 11.0f, DWRITE_FONT_WEIGHT_NORMAL);
}

bool create_device_resources(App& app) {
    if (!app.target) {
        RECT rc{};
        GetClientRect(app.hwnd, &rc);
        const D2D1_SIZE_U size = D2D1::SizeU(
            static_cast<UINT32>(std::max<LONG>(rc.right - rc.left, 1)),
            static_cast<UINT32>(std::max<LONG>(rc.bottom - rc.top, 1)));
        // 96 DPI on the target: layout() is the only place that scales, so a
        // convenient 150 % display cannot put the controls under the wrong point.
        const D2D1_RENDER_TARGET_PROPERTIES properties =
            D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_DEFAULT,
                D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_UNKNOWN),
                96.0f, 96.0f);
        if (FAILED(app.d2d->CreateHwndRenderTarget(
                properties, D2D1::HwndRenderTargetProperties(app.hwnd, size),
                app.target.GetAddressOf()))) {
            return false;
        }
        app.target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        app.target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
    }
    if (!app.brush &&
        FAILED(app.target->CreateSolidColorBrush(kText, app.brush.GetAddressOf()))) {
        return false;
    }
    if (!app.title) return create_text_formats(app);
    return true;
}

void discard_device_resources(App& app) {
    app.brush.Reset();
    app.target.Reset();
}

// -------------------------------------------------------------------- drawing -

bool inside(D2D1_RECT_F r, float x, float y) {
    return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}

D2D1_RECT_F inset(D2D1_RECT_F r, float dx, float dy) {
    return D2D1::RectF(r.left + dx, r.top + dy, r.right - dx, r.bottom - dy);
}

// Returns the part of `r` inside `clip`, or an empty rectangle.
D2D1_RECT_F clipped(D2D1_RECT_F r, D2D1_RECT_F clip) {
    D2D1_RECT_F out = D2D1::RectF(std::max(r.left, clip.left),
                                  std::max(r.top, clip.top),
                                  std::min(r.right, clip.right),
                                  std::min(r.bottom, clip.bottom));
    if (out.right <= out.left || out.bottom <= out.top) {
        return D2D1::RectF(0, 0, 0, 0);
    }
    return out;
}

bool empty(D2D1_RECT_F r) { return r.right <= r.left || r.bottom <= r.top; }

void fill_round(App& app, D2D1_RECT_F r, const D2D1_COLOR_F& color, float radius) {
    app.brush->SetColor(color);
    const float value = app.dp(radius);
    app.target->FillRoundedRectangle(D2D1::RoundedRect(r, value, value),
                                     app.brush.Get());
}

void stroke_round(App& app, D2D1_RECT_F r, const D2D1_COLOR_F& color, float radius,
                  float width) {
    app.brush->SetColor(color);
    const float value = app.dp(radius);
    app.target->DrawRoundedRectangle(D2D1::RoundedRect(r, value, value),
                                     app.brush.Get(), app.dp(width));
}

// One text layout per drawn string, sized to `rect`, centred in it. Alignment is
// done by DirectWrite on the rectangle it is given, never by a guessed offset.
void draw_text(App& app, const std::wstring& value, IDWriteTextFormat* format,
               D2D1_RECT_F rect, const D2D1_COLOR_F& color,
               DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING,
               bool wrap = false, bool ellipsis = false) {
    if (empty(rect)) return;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(app.dwrite->CreateTextLayout(
            value.c_str(), static_cast<UINT32>(value.size()), format,
            rect.right - rect.left, rect.bottom - rect.top,
            layout.GetAddressOf()))) {
        return;
    }
    layout->SetTextAlignment(align);
    layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    layout->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP
                                 : DWRITE_WORD_WRAPPING_NO_WRAP);
    ComPtr<IDWriteInlineObject> dots;
    if (ellipsis &&
        SUCCEEDED(app.dwrite->CreateEllipsisTrimmingSign(format,
                                                         dots.GetAddressOf()))) {
        const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        layout->SetTrimming(&trimming, dots.Get());
    }
    app.brush->SetColor(color);
    app.target->DrawTextLayout(D2D1::Point2F(rect.left, rect.top), layout.Get(),
                              app.brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void draw_ellipsis_text(App& app, const std::wstring& value,
                        IDWriteTextFormat* format, D2D1_RECT_F rect,
                        const D2D1_COLOR_F& color,
                        DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING) {
    draw_text(app, value, format, rect, color, align, false, true);
}

float text_width(App& app, const wchar_t* value, IDWriteTextFormat* format) {
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(app.dwrite->CreateTextLayout(value,
                                            static_cast<UINT32>(wcslen(value)),
                                            format, 800.0f, 200.0f,
                                            layout.GetAddressOf()))) {
        return app.dp(60.0f);
    }
    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(layout->GetMetrics(&metrics))) return app.dp(60.0f);
    return metrics.widthIncludingTrailingWhitespace;
}

// ------------------------------------------------------------------- layout ---

D2D1_RECT_F sidebar_rect(const App& app) {
    return D2D1::RectF(0, 0, app.dp(kSidebarWidth), app.height);
}

D2D1_RECT_F body_rect(const App& app) {
    return D2D1::RectF(app.dp(kSidebarWidth) + app.dp(kPad),
                       app.dp(kHeaderHeight), app.width - app.dp(kPad),
                       app.height);
}

D2D1_RECT_F add_button_rect(const App& app) {
    const float right = app.dp(kSidebarWidth) - app.dp(12.0f);
    return D2D1::RectF(app.dp(12.0f), app.height - app.dp(92.0f), right,
                       app.height - app.dp(52.0f));
}

// The game list lives between the sidebar header and the Add button.
D2D1_RECT_F game_list_rect(const App& app) {
    return D2D1::RectF(0, app.dp(58.0f), app.dp(kSidebarWidth),
                       add_button_rect(app).top - app.dp(10.0f));
}

void layout(App& app) {
    app.rows.clear();
    app.buttons.clear();

    // ---- sidebar: one row per game, its cross, and the Add button ----
    const D2D1_RECT_F list = game_list_rect(app);
    const float item = app.dp(kGameItem);
    const float total = static_cast<float>(app.games.size()) * item;
    app.games_scroll = std::clamp(app.games_scroll, 0.0f,
                                  std::max(0.0f, total - (list.bottom - list.top)));
    for (std::size_t i = 0; i < app.games.size(); ++i) {
        const float top = list.top + static_cast<float>(i) * item - app.games_scroll;
        const D2D1_RECT_F item_rect = D2D1::RectF(
            app.dp(12.0f), top, app.dp(kSidebarWidth) - app.dp(12.0f),
            top + item - app.dp(6.0f));
        if (empty(clipped(item_rect, list))) continue;
        app.buttons.push_back({item_rect, Action::select_game,
                               static_cast<int>(i)});
        if (static_cast<int>(i) == app.selected) {
            app.buttons.push_back({D2D1::RectF(item_rect.right - app.dp(30.0f),
                                               item_rect.top + app.dp(10.0f),
                                               item_rect.right - app.dp(8.0f),
                                               item_rect.top + app.dp(30.0f)),
                                   Action::remove_game, static_cast<int>(i)});
        }
    }
    app.buttons.push_back({add_button_rect(app), Action::add_game, -1});

    // ---- content header: title, path, and the install button ----
    const bool has_game = !app.games.empty();
    const bool is_installed =
        has_game && installed(app.games[static_cast<std::size_t>(app.selected)].directory);
    app.buttons.push_back(
        {D2D1::RectF(app.width - app.dp(200.0f), app.dp(30.0f),
                     app.width - app.dp(kPad), app.dp(68.0f)),
         is_installed ? Action::uninstall : Action::install, -1});

    // ---- settings rows ----
    const D2D1_RECT_F body = body_rect(app);
    const auto& table = settings();
    float y = body.top - app.scroll;
    const wchar_t* group = nullptr;
    for (std::size_t i = 0; i < table.size(); ++i) {
        const Setting& setting = table[i];
        if (group == nullptr || wcscmp(group, setting.group) != 0) {
            group = setting.group;
            y += app.dp(14.0f);
            y += app.dp(24.0f);
        }
        const D2D1_RECT_F row = D2D1::RectF(body.left, y, body.right,
                                            y + app.dp(kRowHeight));
        y += app.dp(kRowHeight) + app.dp(kRowGap);
        const float row_top = row.top;
        if (empty(clipped(row, body))) continue;

        Row entry;
        entry.rect = row;
        entry.setting = static_cast<int>(i);
        entry.chosen = has_game
            ? read_setting(setting,
                           app.games[static_cast<std::size_t>(app.selected)].directory)
            : kDefaultIndex;

        // Segments are laid out right to left, from measured label widths, then
        // clamped to the row so they can never spill outside the card.
        float total_cells = 0.0f;
        std::vector<float> widths;
        for (const auto& label : setting.labels) {
            const float width = text_width(app, label, app.caption.Get()) +
                                app.dp(28.0f);
            widths.push_back(width);
            total_cells += width;
        }
        const float available = (row.right - row.left) - app.dp(300.0f);
        float right = row.right - app.dp(kPad);
        if (total_cells > available && total_cells > 0.0f) {
            const float factor = available / total_cells;
            for (auto& width : widths) width = std::max(width * factor, app.dp(34.0f));
            total_cells = 0.0f;
            for (const float width : widths)
                total_cells += width;
            right = row.right - app.dp(kPad);
        }
        float x = right - total_cells;
        const float cell_height = app.dp(kSegmentHeight);
        const float cell_top = row_top + (app.dp(kRowHeight) - cell_height) / 2.0f;
        for (std::size_t c = 0; c < setting.labels.size(); ++c) {
            const D2D1_RECT_F cell_rect =
                D2D1::RectF(x, cell_top, x + widths[c], cell_top + cell_height);
            // Only the visible slice is clickable: a row scrolled under the header
            // must not answer a click aimed at the header.
            const D2D1_RECT_F visible = clipped(cell_rect, body);
            if (!empty(visible)) {
                entry.cells.push_back({visible, static_cast<int>(c)});
            }
            x += widths[c];
        }
        app.rows.push_back(std::move(entry));
    }
    app.content_height = y - body.top + app.scroll;
    const float view = body.bottom - body.top;
    app.scroll = std::clamp(app.scroll, 0.0f,
                            std::max(0.0f, app.content_height - view));
}

Target hit_test(App& app, float x, float y) {
    // Buttons first, in reverse: the cross is recorded after the game item it sits
    // on, so the later rectangle is the one that wins.
    for (auto it = app.buttons.rbegin(); it != app.buttons.rend(); ++it) {
        if (inside(it->rect, x, y)) {
            return Target{it->action, it->game, -1, -1};
        }
    }
    for (const auto& row : app.rows) {
        for (const auto& cell : row.cells) {
            if (inside(cell.rect, x, y)) {
                return Target{Action::choice, -1, row.setting, cell.option};
            }
        }
    }
    return Target{};
}

// -------------------------------------------------------------------- paint ---

void paint_sidebar(App& app) {
    const D2D1_RECT_F area = sidebar_rect(app);
    app.brush->SetColor(kSidebar);
    app.target->FillRectangle(area, app.brush.Get());
    app.brush->SetColor(kBorder);
    app.target->DrawLine(D2D1::Point2F(area.right, area.top),
                         D2D1::Point2F(area.right, area.bottom), app.brush.Get(),
                         app.dp(1.0f));

    draw_text(app, L"Installed games", app.subtitle.Get(),
              D2D1::RectF(app.dp(20.0f), app.dp(20.0f),
                          app.dp(kSidebarWidth) - app.dp(20.0f), app.dp(46.0f)),
              kMuted);

    if (app.status.empty()) return;
    draw_text(app, app.status, app.tiny.Get(),
              D2D1::RectF(app.dp(18.0f), area.bottom - app.dp(46.0f),
                          app.dp(kSidebarWidth) - app.dp(18.0f),
                          area.bottom - app.dp(8.0f)),
              kMuted, DWRITE_TEXT_ALIGNMENT_LEADING, true);
}

void paint_button(App& app, const Button& button) {
    switch (button.action) {
        case Action::select_game: {
            const Game& game = app.games[static_cast<std::size_t>(button.game)];
            const bool selected = button.game == app.selected;
            const bool hot = app.hot.action == Action::select_game &&
                             app.hot.game == button.game;
            if (selected || hot) {
                fill_round(app, button.rect, selected ? kAccentSoft : kTrack, 10.0f);
            }
            const D2D1_RECT_F text_area = D2D1::RectF(
                button.rect.left + app.dp(14.0f),
                button.rect.top + app.dp(6.0f),
                button.rect.right - app.dp(34.0f),
                button.rect.top + app.dp(26.0f));
            draw_ellipsis_text(app, game.name, app.body.Get(), text_area, kText);
            draw_ellipsis_text(
                app, game.directory.wstring(), app.tiny.Get(),
                D2D1::RectF(text_area.left, text_area.bottom,
                            text_area.right, button.rect.bottom - app.dp(4.0f)),
                kMuted);
            break;
        }
        case Action::remove_game: {
            const bool hot = app.hot.action == Action::remove_game &&
                             app.hot.game == button.game;
            draw_text(app, L"✕", app.caption.Get(), button.rect,
                      hot ? kDanger : kMuted, DWRITE_TEXT_ALIGNMENT_CENTER);
            break;
        }
        case Action::add_game: {
            const bool hot = app.hot.action == Action::add_game;
            fill_round(app, button.rect, hot ? kAccentSoft : kSidebar, 10.0f);
            stroke_round(app, button.rect, kAccent, 10.0f, 1.2f);
            draw_text(app, L"+  Add a game", app.body.Get(), button.rect, kAccent,
                      DWRITE_TEXT_ALIGNMENT_CENTER);
            break;
        }
        case Action::install:
        case Action::uninstall: {
            const bool uninstall = button.action == Action::uninstall;
            const bool hot = app.hot.action == button.action;
            if (uninstall) {
                fill_round(app, button.rect, hot ? D2D1::ColorF(0xFDF1F1) : kCard,
                           10.0f);
                stroke_round(app, button.rect, kDanger, 10.0f, 1.2f);
            } else {
                fill_round(app, button.rect,
                           hot ? D2D1::ColorF(0x255CCB) : kAccent, 10.0f);
            }
            draw_text(app, uninstall ? L"Uninstall" : L"Install", app.body.Get(),
                      button.rect, uninstall ? kDanger : kOnDark,
                      DWRITE_TEXT_ALIGNMENT_CENTER);
            break;
        }
        default:
            break;
    }
}

void paint_row(App& app, const Row& row) {
    const Setting& setting = settings()[static_cast<std::size_t>(row.setting)];
    const bool hovered = app.hot.setting == row.setting;
    fill_round(app, row.rect, hovered ? kCardHot : kCard, kCardRadius);
    stroke_round(app, row.rect, hovered ? kAccentSoft : kBorder, kCardRadius, 1.0f);

    const float cells_left = row.cells.empty()
        ? row.rect.right
        : row.cells.front().rect.left;
    draw_ellipsis_text(
        app, setting.title, app.body.Get(),
        D2D1::RectF(row.rect.left + app.dp(kPad), row.rect.top,
                    cells_left - app.dp(16.0f), row.rect.bottom),
        kText);

    // The whole segment track, then the selected and hovered cells on top of it.
    if (!row.cells.empty()) {
        const D2D1_RECT_F track = D2D1::RectF(
            row.cells.front().rect.left - app.dp(2.0f),
            row.cells.front().rect.top - app.dp(2.0f),
            row.cells.back().rect.right + app.dp(2.0f),
            row.cells.front().rect.bottom + app.dp(2.0f));
        fill_round(app, track, kTrack, kSegmentHeight / 2.0f + 2.0f);
    }
    for (const auto& cell : row.cells) {
        const bool chosen = cell.option == row.chosen;
        const bool hot = app.hot.setting == row.setting &&
                         app.hot.option == cell.option;
        if (chosen) {
            fill_round(app, cell.rect, hot ? D2D1::ColorF(0x255CCB) : kAccent,
                       kSegmentHeight / 2.0f);
        } else if (hot) {
            fill_round(app, cell.rect, kTrackHot, kSegmentHeight / 2.0f);
        }
        draw_text(app, setting.labels[static_cast<std::size_t>(cell.option)],
                  app.caption.Get(), cell.rect, chosen ? kOnDark : kMuted,
                  DWRITE_TEXT_ALIGNMENT_CENTER);
    }
}

void paint_content(App& app) {
    const D2D1_RECT_F body = body_rect(app);
    if (app.games.empty()) {
        draw_text(app, L"No game added yet", app.title.Get(),
                  D2D1::RectF(body.left, body.top + app.dp(30.0f), body.right,
                              body.top + app.dp(70.0f)),
                  kText);
        draw_text(app,
                  L"Add the folder that holds the game executable (for example "
                  L"…\\Binaries\\Win64), then press Install.",
                  app.body.Get(),
                  D2D1::RectF(body.left, body.top + app.dp(72.0f), body.right,
                              body.top + app.dp(140.0f)),
                  kMuted, DWRITE_TEXT_ALIGNMENT_LEADING, true);
        return;
    }

    const Game& game = app.games[static_cast<std::size_t>(app.selected)];
    draw_ellipsis_text(
        app, game.name, app.title.Get(),
        D2D1::RectF(body.left, app.dp(22.0f), app.width - app.dp(224.0f),
                    app.dp(50.0f)),
        kText);
    draw_ellipsis_text(app, game.directory.wstring(), app.caption.Get(),
                       D2D1::RectF(body.left, app.dp(50.0f),
                                   app.width - app.dp(224.0f), app.dp(72.0f)),
                       kMuted);

    app.target->PushAxisAlignedClip(body, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    const wchar_t* group = nullptr;
    float group_top = 0.0f;
    for (const auto& row : app.rows) {
        const Setting& setting = settings()[static_cast<std::size_t>(row.setting)];
        if (group == nullptr || wcscmp(group, setting.group) != 0) {
            group = setting.group;
            group_top = row.rect.top - app.dp(24.0f);
            draw_text(app, group, app.subtitle.Get(),
                      D2D1::RectF(row.rect.left, group_top, row.rect.right,
                                  group_top + app.dp(22.0f)),
                      kMuted);
        }
        paint_row(app, row);
    }
    app.target->PopAxisAlignedClip();
}

void paint_tooltip(App& app) {
    if (app.tooltip.empty()) return;
    const float width = app.dp(400.0f);
    const D2D1_RECT_F anchor = app.tooltip_anchor;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(app.dwrite->CreateTextLayout(
            app.tooltip.c_str(), static_cast<UINT32>(app.tooltip.size()),
            app.caption.Get(), width - app.dp(28.0f), app.height,
            layout.GetAddressOf()))) {
        return;
    }
    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(layout->GetMetrics(&metrics))) return;
    const float height = metrics.height + app.dp(20.0f);

    float x = anchor.left;
    float y = anchor.bottom + app.dp(10.0f);
    if (x + width > app.width - app.dp(10.0f)) x = app.width - width - app.dp(10.0f);
    if (y + height > app.height - app.dp(10.0f)) y = anchor.top - height - app.dp(10.0f);
    x = std::max(x, app.dp(10.0f));
    y = std::max(y, app.dp(10.0f));
    const D2D1_RECT_F box = D2D1::RectF(x, y, x + width, y + height);
    fill_round(app, box, kTooltip, 10.0f);
    draw_text(app, app.tooltip, app.caption.Get(),
              inset(box, app.dp(14.0f), app.dp(10.0f)), D2D1::ColorF(0xF2F3F5),
              DWRITE_TEXT_ALIGNMENT_LEADING, true);
}

void paint(App& app) {
    app.target->BeginDraw();
    app.target->Clear(kBackground);
    paint_sidebar(app);
    paint_content(app);
    for (const auto& button : app.buttons) paint_button(app, button);
    paint_tooltip(app);
    if (app.target->EndDraw() == D2DERR_RECREATE_TARGET) {
        discard_device_resources(app);
    }
}

// ------------------------------------------------------------------- actions ---

void refresh_games(App& app) {
    auto_populate_games_if_empty();
    app.games = load_games();
    if (app.selected >= static_cast<int>(app.games.size())) {
        app.selected = static_cast<int>(app.games.size()) - 1;
    }
    if (app.selected < 0) app.selected = 0;
}

void pick_folder(App& app) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(dialog.GetAddressOf())))) {
        return;
    }
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dialog->SetTitle(L"Choose the game folder (the one holding its executable)");
    if (FAILED(dialog->Show(app.hwnd))) return;
    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(item.GetAddressOf()))) return;
    PWSTR raw = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) || raw == nullptr) return;
    const std::filesystem::path directory(raw);
    CoTaskMemFree(raw);

    // The layer is only found from the folder that holds the game executable, so
    // take the game's own name from there when the executable is recognisable.
    std::wstring name;
    std::error_code code;
    for (const auto& entry : std::filesystem::directory_iterator(
             directory, std::filesystem::directory_options::skip_permission_denied,
             code)) {
        const auto extension = entry.path().extension().wstring();
        if (_wcsicmp(extension.c_str(), L".exe") == 0 &&
            entry.path().filename().wstring().find(L"-Win64-Shipping") !=
                std::wstring::npos) {
            name = directory.parent_path()
                       .parent_path()
                       .parent_path()
                       .filename()
                       .wstring();
            break;
        }
    }
    if (name.empty()) name = directory.filename().wstring();
    const int id = add_game(directory, name);
    refresh_games(app);
    for (std::size_t i = 0; i < app.games.size(); ++i) {
        if (app.games[i].id == id) app.selected = static_cast<int>(i);
    }
    app.status = L"Folder added";
    app.scroll = 0.0f;
    app.games_scroll = 0.0f;
}

void choose(App& app, int setting_index, int option) {
    if (app.games.empty()) return;
    const auto& table = settings();
    if (setting_index < 0 || setting_index >= static_cast<int>(table.size())) return;
    const Setting& setting = table[static_cast<std::size_t>(setting_index)];
    if (option < 0 || option >= static_cast<int>(setting.values.size())) return;
    if (write_setting(setting, option,
                      app.games[static_cast<std::size_t>(app.selected)].directory)) {
        app.status = std::wstring(setting.title) + L": " +
                     setting.labels[static_cast<std::size_t>(option)];
    } else {
        app.status = std::wstring(L"Cannot write settings into ") +
                     app.games[static_cast<std::size_t>(app.selected)]
                         .directory.wstring();
    }
}

void activate(App& app, const Target& target) {
    const auto directory = [&app] {
        return app.games.empty()
            ? std::filesystem::path{}
            : app.games[static_cast<std::size_t>(app.selected)].directory;
    };
    std::wstring error;
    switch (target.action) {
        case Action::select_game:
            if (target.game >= 0 && target.game < static_cast<int>(app.games.size())) {
                if (app.selected != target.game) app.scroll = 0.0f;
                app.selected = target.game;
            }
            break;
        case Action::remove_game:
            if (target.game >= 0 && target.game < static_cast<int>(app.games.size())) {
                remove_game(app.games[static_cast<std::size_t>(target.game)].id);
                refresh_games(app);
                app.status = L"Removed from the list (files untouched)";
            }
            break;
        case Action::add_game:
            pick_folder(app);
            break;
        case Action::install:
            app.status = install(directory(), source_directory(), &error)
                ? L"Installed into this game"
                : (error.empty() ? L"Installation failed" : error);
            break;
        case Action::uninstall:
            app.status = uninstall(directory(), &error)
                ? L"Uninstalled (settings kept)"
                : (error.empty() ? L"Uninstallation failed" : error);
            break;
        case Action::choice:
            choose(app, target.setting, target.option);
            break;
        default:
            break;
    }
}

// ------------------------------------------------------------------ self-test --

// Verifies what the old code got wrong: every cell inside its row, no two cells
// overlapping, and the centre of every control resolving to that same control.
bool self_check(App& app, float scroll, std::string* report = nullptr) {
    app.scroll = scroll;
    layout(app);
    bool ok = true;
    const auto fail = [&ok, report](const char* what) {
        ok = false;
        if (report) {
            *report += what;
            *report += "\n";
        }
        OutputDebugStringA(what);
        OutputDebugStringA("\n");
    };
    const D2D1_RECT_F body = body_rect(app);
    for (const auto& row : app.rows) {
        float previous_right = row.rect.left;
        for (const auto& cell : row.cells) {
            if (cell.rect.left < row.rect.left || cell.rect.right > row.rect.right ||
                cell.rect.top < row.rect.top || cell.rect.bottom > row.rect.bottom) {
                fail("a cell leaves its row");
            }
            if (cell.rect.left < previous_right - 0.5f) fail("cells overlap");
            // Every recorded cell is inside the scrolling body: a row hidden under
            // the header must not answer a click aimed at the header.
            if (empty(clipped(cell.rect, body))) fail("a cell is outside the body");
            previous_right = cell.rect.right;
        }
        for (const auto& cell : row.cells) {
            const Target hit =
                hit_test(app, (cell.rect.left + cell.rect.right) / 2.0f,
                         (cell.rect.top + cell.rect.bottom) / 2.0f);
            if (hit.action != Action::choice || hit.setting != row.setting ||
                hit.option != cell.option) {
                fail("a cell does not resolve to itself");
            }
        }
    }
    // Rows must not share pixels: two visible cards over the same point would make
    // the click ambiguous.
    for (std::size_t i = 0; i < app.rows.size(); ++i) {
        for (std::size_t j = i + 1; j < app.rows.size(); ++j) {
            const D2D1_RECT_F a = app.rows[i].rect;
            const D2D1_RECT_F b = app.rows[j].rect;
            if (a.left < b.right && b.left < a.right && a.top < b.bottom &&
                b.top < a.bottom) {
                fail("two rows overlap");
            }
        }
    }
    for (const auto& button : app.buttons) {
        const Target hit = hit_test(app, (button.rect.left + button.rect.right) / 2.0f,
                                    (button.rect.top + button.rect.bottom) / 2.0f);
        if (hit.action != button.action ||
            (button.game >= 0 && hit.game != button.game)) {
            fail("a button does not resolve to itself");
        }
    }
    // A click in the gap between two cards, and a click in the header strip, must
    // reach no setting. That is the case the old code got wrong: rows scrolled under
    // the header kept their rectangles and answered clicks aimed at the title.
    for (std::size_t i = 1; i < app.rows.size(); ++i) {
        const float gap = (app.rows[i - 1].rect.bottom + app.rows[i].rect.top) / 2.0f;
        const float x = (app.rows[i].rect.left + app.rows[i].rect.right) / 2.0f;
        if (hit_test(app, x, gap).action == Action::choice) {
            fail("the gap between two rows answers a click");
        }
    }
    if (hit_test(app, app.dp(kSidebarWidth) + app.dp(40.0f), app.dp(8.0f)).action ==
        Action::choice) {
        fail("the header strip answers a click");
    }
    if (!app.rows.empty() && !app.games.empty()) {
        return ok && !app.rows.front().cells.empty();
    }
    return ok;
}

// ------------------------------------------------------------------- window ---

LRESULT handle_message(App& app, HWND hwnd, UINT message, WPARAM wparam,
                       LPARAM lparam) {
    switch (message) {
        case WM_CREATE:
            app.hwnd = hwnd;
            return 0;
        case WM_SIZE: {
            app.width = static_cast<float>(LOWORD(lparam));
            app.height = static_cast<float>(HIWORD(lparam));
            if (app.target) {
                app.target->Resize(D2D1::SizeU(
                    static_cast<UINT32>(std::max<float>(app.width, 1.0f)),
                    static_cast<UINT32>(std::max<float>(app.height, 1.0f))));
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
            info->ptMinTrackSize.x = static_cast<LONG>(app.dp(880.0f));
            info->ptMinTrackSize.y = static_cast<LONG>(app.dp(560.0f));
            return 0;
        }
        case WM_MOUSEMOVE: {
            app.mouse_x = static_cast<float>(GET_X_LPARAM(lparam));
            app.mouse_y = static_cast<float>(GET_Y_LPARAM(lparam));
            const Target before = app.hot;
            app.hot = hit_test(app, app.mouse_x, app.mouse_y);
            SetCursor(LoadCursorW(nullptr, app.hot.clickable() ? IDC_HAND
                                                               : IDC_ARROW));

            // The explanation of the row under the mouse, never a modal dialog.
            std::wstring tooltip;
            D2D1_RECT_F anchor{};
            if (app.hot.action == Action::choice && app.hot.setting >= 0) {
                for (const auto& row : app.rows) {
                    if (row.setting == app.hot.setting) {
                        tooltip = settings()[static_cast<std::size_t>(row.setting)].help;
                        anchor = row.rect;
                        break;
                    }
                }
            }
            if (!(before == app.hot) || tooltip != app.tooltip) {
                app.tooltip = tooltip;
                app.tooltip_anchor = anchor;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_LBUTTONDOWN: {
            const float x = static_cast<float>(GET_X_LPARAM(lparam));
            const float y = static_cast<float>(GET_Y_LPARAM(lparam));
            const Target target = hit_test(app, x, y);
            if (!target.clickable()) break;
            activate(app, target);
            app.hot = hit_test(app, x, y);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ScreenToClient(hwnd, &point);
            const float step =
                GET_WHEEL_DELTA_WPARAM(wparam) / static_cast<float>(WHEEL_DELTA) *
                app.dp(60.0f);
            layout(app);  // sizes the scroll ranges from the current content
            if (point.x < static_cast<LONG>(app.dp(kSidebarWidth))) {
                app.games_scroll -= step;
            } else {
                app.scroll -= step;
            }
            layout(app);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            BeginPaint(hwnd, &ps);
            if (create_device_resources(app)) {
                layout(app);
                paint(app);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DPICHANGED: {
            app.dpi = static_cast<float>(HIWORD(wparam));
            const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            app.title.Reset();
            discard_device_resources(app);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

bool has_argument(const wchar_t* wanted) {
    int count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (arguments == nullptr) return false;
    bool found = false;
    for (int i = 1; i < count && !found; ++i) {
        found = _wcsicmp(arguments[i], wanted) == 0;
    }
    LocalFree(arguments);
    return found;
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
        return 1;
    }
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                __uuidof(ID2D1Factory), nullptr,
                                reinterpret_cast<void**>(g_app.d2d.GetAddressOf()))) ||
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                   __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(
                                       g_app.dwrite.GetAddressOf())))) {
        return 1;
    }
    refresh_games(g_app);
    g_app.status =
        g_app.games.empty() ? L"Step 1: add a game folder" : L"Ready";

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = [](HWND hwnd, UINT message, WPARAM wparam,
                        LPARAM lparam) -> LRESULT {
        return handle_message(g_app, hwnd, message, wparam, lparam);
    };
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&wc)) return 1;

    // The DPI of the window, not of the system: a mixed-DPI setup must not make the
    // layout and the clicks disagree.
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const UINT dpi = GetDpiForSystem();
    g_app.dpi = static_cast<float>(dpi);
    g_app.width = 1100.0f * g_app.dpi / 96.0f;
    g_app.height = 760.0f * g_app.dpi / 96.0f;

    RECT frame{0, 0, static_cast<LONG>(g_app.width), static_cast<LONG>(g_app.height)};
    AdjustWindowRectEx(&frame, WS_OVERLAPPEDWINDOW, FALSE, 0);
    HWND hwnd = CreateWindowExW(
        0, kWindowClass, kWindowTitle, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
        CW_USEDEFAULT, frame.right - frame.left, frame.bottom - frame.top, nullptr,
        nullptr, instance, nullptr);
    if (hwnd == nullptr) return 1;
    g_app.hwnd = hwnd;
    g_app.dpi = static_cast<float>(GetDpiForWindow(hwnd));

    if (has_argument(L"--self-check")) {
        // No window shown: lay out at the default size and verify the geometry.
        g_app.width = g_app.dp(1100.0f);
        g_app.height = g_app.dp(760.0f);
        create_text_formats(g_app);
        // Both ends of the scroll range, because the clipping bug this guards
        // against only shows once the list has moved.
        std::string report;
        const bool ok = self_check(g_app, 0.0f, &report) &
                        self_check(g_app, 300.0f, &report) &
                        self_check(g_app, 1.0e6f, &report);
        const std::wstring message =
            ok ? L"self-check: geometry ok\n" : L"self-check: FAILED\n";
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            DWORD written = 0;
            WriteConsoleW(GetStdHandle(STD_OUTPUT_HANDLE), message.c_str(),
                          static_cast<DWORD>(message.size()), &written, nullptr);
        }
        OutputDebugStringW(message.c_str());
        DestroyWindow(hwnd);
        CoUninitialize();
        return ok ? 0 : 2;
    }

    ShowWindow(hwnd, SW_SHOWDEFAULT);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    discard_device_resources(g_app);
    CoUninitialize();
    return 0;
}
