#pragma once

#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "soundpalette/manifest.h"
#include "soundpalette/mapping.h"

namespace spapp {

// Sidebar sort modes (§10).
enum class SortMode { kHue = 0, kBrightness, kSize, kAttack, kTail, kName };

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
    int selected = -1;          // index into manifest.files, -1 = none
    std::vector<int> order;     // display order (indices into manifest.files) after sort+filter
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

    // UI scaling: dpi_scale from the monitor content scale at startup (4K readability),
    // user_scale from the View menu. Grid metrics multiply by ui_scale().
    float dpi_scale = 1.0f;
    float user_scale = 1.0f;
    float ui_scale() const {
        return dpi_scale * user_scale;
    }

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

// Shared helper: HSL (§7 visual attributes) -> ImGui-packed RGBA.
unsigned int hsl_to_rgba(double hue_deg, double sat_pct, double light_pct, double alpha);

} // namespace spapp
