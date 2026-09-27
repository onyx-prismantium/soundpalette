#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "soundpalette/annotator.h"
#include "soundpalette/deviation.h"
#include "soundpalette/library.h"
#include "soundpalette/manifest.h"
#include "soundpalette/mapping.h"
#include "soundpalette/profile.h"
#include "soundpalette/recipe.h"

struct ImDrawList; // imgui.h; app_state.h stays imgui-free
struct ImVec2;

namespace spapp {

// Sidebar sort modes (§10 + extension-2 §7.1 "by deviation").
enum class SortMode { kHue = 0, kBrightness, kSize, kAttack, kTail, kName, kDeviation };

// Central-view switcher (extension-2 §7.2; extension-4 §9.2 adds the library).
enum class ViewMode { kGrid = 0, kConstellation, kLibrary };

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
    // Parameter checkboxes (sidebar, below the filter): unchecking a parameter neutralizes
    // its visual encoding in the grid — a viewing aid only, analysis and lint are untouched.
    // Order matches the manual's sections 1-8.
    bool show_warmth = true, show_tonality = true, show_attack = true, show_tail = true;
    bool show_loudness = true, show_sharpness = true, show_roughness = true,
         show_fluctuation = true;
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
    std::array<double, 8> proposal_dims_before{};
    std::array<double, 8> proposal_dims_after{};

    bool smoke_mode = false; // suppresses NFD dialogs
    bool want_quit = false;  // set by File > Quit; main loop closes the window

    // M17 library view (extension-4 §9.2). `library` is the open index for root_dir (null when
    // the folder has none); rows are the current query result, parallel to lib_row_file.
    std::unique_ptr<sp::Library> library;
    std::string library_error;
    bool lib_dirty = true; // re-run the query + refresh counts on the next frame
    char lib_search[256] = {0};
    std::string lib_category; // "" = all categories
    std::string lib_catid;    // "" = all CatIDs of the category
    bool lib_only_unannotated = false;
    bool lib_only_low_conf = false; // confidence < 0.5
    bool lib_only_locked = false;
    std::vector<sp::LibraryRow> lib_rows;
    std::vector<int> lib_row_file; // manifest.files index per row, -1 if not scanned
    std::vector<std::pair<std::string, std::size_t>> lib_category_counts;
    std::unordered_map<std::string, std::vector<std::pair<std::string, std::size_t>>>
        lib_catid_counts; // per expanded category
    std::size_t lib_file_count = 0;
    std::size_t lib_annotated_count = 0;

    // Inspector annotation editor: buffers belong to lib_edit_path.
    std::string lib_edit_path;
    std::optional<sp::LibraryRow> lib_edit_row;
    char lib_edit_catid[32] = {0};
    char lib_edit_fx[128] = {0};
    char lib_edit_desc[256] = {0};
    char lib_edit_kw[256] = {0};

    // Background annotate job (worker thread; UI polls).
    std::string annotator_command; // "" = $SP_ANNOTATOR or the default name
    std::thread annotate_thread;
    std::atomic<bool> annotating{false};
    std::atomic<bool> annotate_cancel{false};
    std::atomic<std::size_t> annotate_done{0};
    std::atomic<std::size_t> annotate_total{0};
    std::mutex annotate_mutex; // guards annotate_last_path + annotate_result
    std::string annotate_last_path;
    std::optional<sp::AnnotateReport> annotate_result;

    // Annotator reachability probe (Library tab, once per command change).
    std::thread probe_thread;
    std::atomic<int> probe_state{0}; // 0 = not probed, 1 = probing, 2 = ok, 3 = failed
    std::mutex probe_mutex;
    sp::AnnotatorInfo probe_info;
    std::string probe_error;
    std::string probed_command;
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
// The manual's spectrum strips draw only the half a section talks about (centered in the
// cell); the grid and the example rows draw the full split glyph.
enum class GlyphPart { full, blob, line };

// Render-time parameter mask (sidebar checkboxes). An unchecked parameter draws as neutral:
// warmth -> gray blob, tonality -> no rays, attack -> round, tail -> no trail, loudness ->
// hairline, sharpness -> gray line, roughness/fluctuation -> minimum wave. A half whose four
// parameters are all off is not drawn at all, so a fully unchecked glyph disappears. The
// manual always draws unmasked — it documents the mapping, not the current view.
struct GlyphMask {
    bool warmth = true, tonality = true, attack = true, tail = true;
    bool loudness = true, sharpness = true, roughness = true, fluctuation = true;
    bool blob_shown() const {
        return warmth || tonality || attack || tail;
    }
    bool line_shown() const {
        return loudness || sharpness || roughness || fluctuation;
    }
};

void draw_glyph(ImDrawList *draw, const sp::Visual &v, ImVec2 center, float cell_px,
                GlyphPart part = GlyphPart::full, const GlyphMask &mask = {});

// harmonize_panel.cpp (M9, extension §6.5).
bool load_baseline(AppState &state, const std::string &path); // manifest OR .sppal.json
void recompute_badges(AppState &state); // recomputes deviations when a profile is loaded
void draw_harmonize(AppState &state);   // inspector section for the selected outlier

// constellation.cpp (M11, extension-2 §7.2).
void draw_constellation(AppState &state);
void draw_deviation_section(AppState &state, int file_index); // shared inspector/tooltip part

// library_view.cpp (M17, extension-4 §9.2).
void library_open_for_root(AppState &state); // opens <root>/.soundpalette if present
bool library_create(AppState &state);        // creates + ingests + classifies offline
void library_update(AppState &state);        // incremental ingest (synchronous)
void library_requery(AppState &state);       // runs the current filter/search
void library_select_row(AppState &state, int row);
void draw_library(AppState &state);                            // the Library tab
void draw_library_menu(AppState &state);                       // "Library" menu
void draw_library_status(AppState &state);                     // footer segment
void draw_annotation_section(AppState &state, int file_index); // inspector part
void start_annotate(AppState &state, std::vector<std::string> rel_paths);
void poll_annotate(AppState &state);
void shutdown_annotate_thread(AppState &state);
void start_annotator_probe(AppState &state);

// Shared helper: HSL (§7 visual attributes) -> ImGui-packed RGBA.
unsigned int hsl_to_rgba(double hue_deg, double sat_pct, double light_pct, double alpha);

} // namespace spapp
