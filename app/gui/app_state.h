#pragma once

#include <atomic>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "soundpalette/deviation.h"
#include "soundpalette/manifest.h"
#include "soundpalette/mapping.h"
#include "soundpalette/profile.h"
#include "soundpalette/recipe.h"

struct ImDrawList; // imgui.h; app_state.h stays imgui-free
struct ImVec2;

namespace spapp {

// Sidebar sort modes (§10 + extension-2 §7.1 "by deviation").
enum class SortMode { kHue = 0, kBrightness, kSize, kAttack, kTail, kName, kDeviation };

// Central-view switcher (extension-2 §7.2).
enum class ViewMode { kGrid = 0, kConstellation };

struct AppState {
    // Data.
    std::string root_dir;  // currently open folder ("" = nothing open)
    sp::Manifest manifest; // UI-thread copy; visuals re-derived by the tuner
    double last_scan_seconds = 0.0;

    // Background rescan (§10: worker thread, UI stays responsive).
    std::thread scan_thread;
    std::mutex scan_mutex;                        // guards pending_manifest
    std::optional<sp::Manifest> pending_manifest; // filled by the worker, adopted by the UI
    std::atomic<bool> scanning{false};
    std::atomic<std::size_t> scan_done{0};
    std::atomic<std::size_t> scan_total{0};
    double scan_started_at = 0.0;

    // UI.
    SortMode sort_mode = SortMode::kName;
    char filter_text[256] = {0};
    int selected = -1;      // index into manifest.files, -1 = none
    std::vector<int> order; // display order (indices into manifest.files) after sort+filter
    // Folder sections: each subfolder of the scan root becomes a titled area in the grid
    // (full folder path above, divider below); files inside are sorted by sort_mode.
    struct GridGroup {
        std::string folder; // scan-root-relative, "/" for root-level files
        std::vector<int> indices;
    };
    std::vector<GridGroup> groups;
    std::string status_message; // transient one-line feedback (exports, errors)

    // Mapping tuner (§10): runtime copy of the config; edits re-derive all visuals live.
    sp::MappingConfig tuner;

    // Playback (§10): one ma_engine instance; null when no sound device exists (headless).
    void *engine = nullptr;
    bool audio_ok = false;

    // Controlled playback: at most one ma_sound alive at a time, owned here. The grid shows
    // play/pause/stop per cell; the playing cell counts remaining seconds down.
    void *active_sound = nullptr; // ma_sound*, heap-owned
    int playing_index = -1;       // manifest.files index of the playing entry, -1 = none
    bool paused = false;
    double playing_length_s = 0.0; // cached at start; never queried while playing (MP3 race)

    // UI scaling: dpi_scale from the monitor content scale at startup (4K readability),
    // user_scale from the View menu. Grid metrics multiply by ui_scale().
    float dpi_scale = 1.0f;
    float user_scale = 1.0f;
    float ui_scale() const {
        return dpi_scale * user_scale;
    }

    // M10/M11: loaded palette profile (a --baseline manifest is adapted into an anonymous
    // profile) and the per-entry deviations computed from it; drives halos, sorting, the
    // deviation inspector section, and the M9 harmonize panel's category targeting.
    bool profile_loaded = false;
    std::string profile_source_path;
    sp::Profile profile;
    std::vector<sp::Deviation> deviations; // parallel to manifest.files

    // M11 view state (extension-2 §7).
    ViewMode view_mode = ViewMode::kGrid;
    bool show_halos = true;
    bool show_z_labels = true;
    bool dim_conforming = false;
    bool outliers_only = false;
    int constellation_axis_x = 0; // 0..6 dims, 7 = PCA1, 8 = PCA2
    int constellation_axis_y = 1;
    int constellation_category = -1; // -1 = all (top-level), else category index
    // Constellation zoom/pan: wheel zooms about the cursor, left-drag pans. zoom 1 + pan 0
    // is the auto-fit view; reset on axis/category change and via the "reset view" button.
    double constellation_zoom = 1.0;
    double constellation_pan_x = 0.0; // data-unit offset of the view center
    double constellation_pan_y = 0.0;
    bool show_manual = false;     // Manual window (menu bar)
    bool show_about = false;      // About window (menu bar)
    std::string view_note;        // e.g. the PCA fallback message
    bool force_view_tab = false;  // set by --view to pre-select a tab in smoke mode
    int want_create_profile = 0;  // 1 = from folder, 2 = from selection (modal pending)
    std::set<int> multi_selected; // ctrl+click multi-selection for create-from-selection

    bool proposal_valid = false;
    int proposal_for = -1; // manifest.files index the proposal belongs to
    sp::Recipe proposal;
    sp::Visual predicted_visual; // §6.5 predicted glyph from the solver's post-metrics
    std::array<double, 7> proposal_dims_before{};
    std::array<double, 7> proposal_dims_after{};

    bool smoke_mode = false; // suppresses NFD dialogs
    bool want_quit = false;  // set by File > Quit; main loop closes the window
};

// app.cpp — orchestration.
void start_rescan(AppState &state);
void poll_rescan(AppState &state);     // adopt a finished background scan, if any
void rebuild_visuals(AppState &state); // re-derive every Visual from cached Features
void rebuild_order(AppState &state);   // apply sort mode + text filter
void shutdown_scan_thread(AppState &state);
void draw_ui(AppState &state); // whole frame: menu, sidebar, grid, inspector, status
bool export_svg_to(AppState &state, const std::string &path);

// Controlled playback (grid transport buttons; M9 A/B preview reuses playback_start_path).
void playback_start(AppState &state, int file_index);
void playback_start_path(AppState &state, const std::string &abs_path, int ui_index);
void playback_toggle_pause(AppState &state);
void playback_stop(AppState &state);
void playback_update(AppState &state);              // per-frame: auto-stop at end of sound
double playback_remaining_s(const AppState &state); // seconds left in the active sound

// Panels.
void draw_grid(AppState &state);      // grid.cpp
void draw_inspector(AppState &state); // inspector.cpp
void draw_tuner(AppState &state);     // tuner.cpp

// manual.cpp: Manual and About windows (menu bar entries).
void draw_manual(AppState &state);
void draw_about(AppState &state);

// grid.cpp: glyph + decay tail into a cell_px-square cell, shared with the manual's examples.
void draw_glyph(ImDrawList *draw, const sp::Visual &v, ImVec2 center, float cell_px);

// harmonize_panel.cpp (M9, extension §6.5).
bool load_baseline(AppState &state, const std::string &path); // manifest OR .sppal.json
void recompute_badges(AppState &state); // recomputes deviations when a profile is loaded
void draw_harmonize(AppState &state);   // inspector section for the selected outlier

// constellation.cpp (M11, extension-2 §7.2).
void draw_constellation(AppState &state);
void draw_deviation_section(AppState &state, int file_index); // shared inspector/tooltip part

// Shared helper: HSL (§7 visual attributes) -> ImGui-packed RGBA.
unsigned int hsl_to_rgba(double hue_deg, double sat_pct, double light_pct, double alpha);

} // namespace spapp
