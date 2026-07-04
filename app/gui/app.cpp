#include "gui/app_state.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <utility>

#include <imgui.h>
#include <nfd.h>

#include "miniaudio.h"

#include "soundpalette/glyph.h"
#include "soundpalette/version.h"

namespace spapp {

namespace {

double now_seconds() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

bool contains_case_insensitive(const std::string &haystack, const std::string &needle) {
    if (needle.empty()) {
        return true;
    }
    auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                          [](char a, char b) {
                              return std::tolower(static_cast<unsigned char>(a)) ==
                                     std::tolower(static_cast<unsigned char>(b));
                          });
    return it != haystack.end();
}

void draw_menu_bar(AppState &state) {
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open folder...", nullptr, false, !state.smoke_mode)) {
            nfdu8char_t *picked = nullptr;
            if (NFD_PickFolderU8(&picked, nullptr) == NFD_OKAY) {
                state.root_dir = picked;
                NFD_FreePathU8(picked);
                start_rescan(state);
            }
        }
        if (ImGui::MenuItem("Rescan", nullptr, false,
                            !state.root_dir.empty() && !state.scanning.load())) {
            start_rescan(state);
        }
        if (ImGui::MenuItem("Export SVG...", nullptr, false,
                            !state.manifest.files.empty() && !state.smoke_mode)) {
            nfdu8char_t *saved = nullptr;
            nfdu8filteritem_t filter{"SVG image", "svg"};
            if (NFD_SaveDialogU8(&saved, &filter, 1, nullptr, "sheet.svg") == NFD_OKAY) {
                export_svg_to(state, saved);
                NFD_FreePathU8(saved);
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", "Ctrl+Q")) {
            state.want_quit = true;
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Profile")) {
        if (ImGui::MenuItem("Load...", nullptr, false, !state.smoke_mode)) {
            nfdu8char_t *picked = nullptr;
            nfdu8filteritem_t filter{"Palette profile or manifest", "sppal.json,json"};
            if (NFD_OpenDialogU8(&picked, &filter, 1, nullptr) == NFD_OKAY) {
                state.status_message = load_baseline(state, picked)
                                           ? std::string("profile: ") + picked
                                           : std::string("invalid profile: ") + picked;
                NFD_FreePathU8(picked);
            }
        }
        if (ImGui::MenuItem("Create from folder...", nullptr, false,
                            !state.smoke_mode && !state.manifest.files.empty())) {
            state.want_create_profile = 1;
        }
        if (ImGui::MenuItem("Create from current selection...", nullptr, false,
                            !state.smoke_mode && !state.multi_selected.empty())) {
            state.want_create_profile = 2;
        }
        if (ImGui::MenuItem("Clear", nullptr, false, state.profile_loaded)) {
            state.profile_loaded = false;
            state.profile = sp::Profile{};
            recompute_badges(state);
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        static const float kScales[] = {1.0f, 1.25f, 1.5f, 2.0f};
        static const char *kLabels[] = {"UI scale 100%", "UI scale 125%", "UI scale 150%",
                                        "UI scale 200%"};
        for (int i = 0; i < 4; ++i) {
            if (ImGui::MenuItem(kLabels[i], nullptr, state.user_scale == kScales[i])) {
                state.user_scale = kScales[i];
                ImGui::GetStyle().FontScaleMain = state.user_scale;
            }
        }
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

void draw_sidebar(AppState &state) {
    ImGui::TextUnformatted("Sort by");
    static const char *kModes[] = {"hue",  "brightness", "size",     "attack",
                                   "tail", "name",       "deviation"};
    int mode = static_cast<int>(state.sort_mode);
    const int mode_count = state.profile_loaded ? 7 : 6; // deviation needs a profile
    for (int i = 0; i < mode_count; ++i) {
        if (ImGui::RadioButton(kModes[i], mode == i)) {
            state.sort_mode = static_cast<SortMode>(i);
            rebuild_order(state);
        }
    }
    ImGui::Separator();
    ImGui::TextUnformatted("Filter");
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputText("##filter", state.filter_text, sizeof(state.filter_text))) {
        rebuild_order(state);
    }
    if (state.profile_loaded) {
        ImGui::Separator();
        ImGui::TextUnformatted("Deviation");
        ImGui::Checkbox("halos", &state.show_halos);
        ImGui::Checkbox("z labels", &state.show_z_labels);
        ImGui::Checkbox("dim conforming", &state.dim_conforming);
        if (ImGui::Checkbox("outliers only", &state.outliers_only)) {
            rebuild_order(state);
        }
    }
}

void draw_status_bar(AppState &state) {
    if (state.scanning.load()) {
        ImGui::Text("analyzed %zu/%zu", state.scan_done.load(), state.scan_total.load());
    } else {
        if (state.profile_loaded) {
            ImGui::Text("%zu files | scan %.2f s | mapping v%d | %s | profile: %s (%d cat)",
                        state.manifest.files.size(), state.last_scan_seconds,
                        state.manifest.mapping_version, state.audio_ok ? "audio on" : "audio off",
                        state.profile.name.empty() ? "(baseline)" : state.profile.name.c_str(),
                        static_cast<int>(state.profile.categories.size()));
        } else {
            ImGui::Text("%zu files | scan %.2f s | mapping v%d | %s", state.manifest.files.size(),
                        state.last_scan_seconds, state.manifest.mapping_version,
                        state.audio_ok ? "audio on" : "audio off");
        }
        if (!state.view_note.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.9f, 0.75f, 0.4f, 1.0f), "| %s", state.view_note.c_str());
        }
    }
    if (!state.status_message.empty()) {
        ImGui::SameLine();
        ImGui::TextUnformatted("|");
        ImGui::SameLine();
        ImGui::TextUnformatted(state.status_message.c_str());
    }
}

} // namespace

unsigned int hsl_to_rgba(double hue_deg, double sat_pct, double light_pct, double alpha) {
    double h = hue_deg;
    h = h - 360.0 * std::floor(h / 360.0);
    double s = std::clamp(sat_pct / 100.0, 0.0, 1.0);
    double l = std::clamp(light_pct / 100.0, 0.0, 1.0);

    double c = (1.0 - std::fabs(2.0 * l - 1.0)) * s;
    double hp = h / 60.0;
    double x = c * (1.0 - std::fabs(std::fmod(hp, 2.0) - 1.0));
    double r = 0.0, g = 0.0, b = 0.0;
    if (hp < 1.0) {
        r = c, g = x;
    } else if (hp < 2.0) {
        r = x, g = c;
    } else if (hp < 3.0) {
        g = c, b = x;
    } else if (hp < 4.0) {
        g = x, b = c;
    } else if (hp < 5.0) {
        r = x, b = c;
    } else {
        r = c, b = x;
    }
    double m = l - c / 2.0;
    ImVec4 v(static_cast<float>(r + m), static_cast<float>(g + m), static_cast<float>(b + m),
             static_cast<float>(alpha));
    return ImGui::ColorConvertFloat4ToU32(v);
}

void start_rescan(AppState &state) {
    if (state.root_dir.empty() || state.scanning.load()) {
        return;
    }
    shutdown_scan_thread(state);

    state.scanning.store(true);
    state.scan_done.store(0);
    state.scan_total.store(0);
    state.scan_started_at = now_seconds();

    std::string dir = state.root_dir;
    state.scan_thread = std::thread([&state, dir]() {
        sp::ScanOptions options;
        options.on_progress = [&state](std::size_t done, std::size_t total) {
            state.scan_done.store(done);
            state.scan_total.store(total);
        };
        sp::Manifest result = sp::scan_directory(dir, options);
        {
            std::lock_guard<std::mutex> lock(state.scan_mutex);
            state.pending_manifest = std::move(result);
        }
        state.scanning.store(false);
    });
}

void poll_rescan(AppState &state) {
    if (state.scanning.load()) {
        return;
    }
    std::optional<sp::Manifest> adopted;
    {
        std::lock_guard<std::mutex> lock(state.scan_mutex);
        if (state.pending_manifest.has_value()) {
            adopted = std::move(state.pending_manifest);
            state.pending_manifest.reset();
        }
    }
    if (!adopted.has_value()) {
        return;
    }
    state.manifest = std::move(*adopted);
    state.last_scan_seconds = now_seconds() - state.scan_started_at;
    state.selected = -1;
    rebuild_visuals(state);
}

void rebuild_visuals(AppState &state) {
    // The tuner edits a runtime copy; the whole grid re-derives Visual from cached Features
    // with no re-analysis (§10).
    sp::set_active_mapping_config(state.tuner);
    for (sp::FileEntry &e : state.manifest.files) {
        if (e.error.empty()) {
            e.visual = sp::map_v1(e.features, e.loudness, sp::path_seed(e.path));
        }
    }
    recompute_badges(state);
    rebuild_order(state);
}

void rebuild_order(AppState &state) {
    const std::vector<sp::FileEntry> &files = state.manifest.files;
    state.order.clear();
    state.order.reserve(files.size());
    std::string needle(state.filter_text);
    for (int i = 0; i < static_cast<int>(files.size()); ++i) {
        if (!contains_case_insensitive(files[static_cast<std::size_t>(i)].path, needle)) {
            continue;
        }
        if (state.outliers_only && state.profile_loaded &&
            i < static_cast<int>(state.deviations.size()) &&
            state.deviations[static_cast<std::size_t>(i)].band == sp::DevBand::none) {
            continue; // §7.1 "outliers only" grid filter
        }
        state.order.push_back(i);
    }

    SortMode mode = state.sort_mode;
    if (mode == SortMode::kDeviation && !state.profile_loaded) {
        mode = SortMode::kName;
    }
    const std::vector<sp::Deviation> &devs = state.deviations;
    auto sort_key_less = [&files, &devs, mode](int a, int b) {
        const sp::FileEntry &ea = files[static_cast<std::size_t>(a)];
        const sp::FileEntry &eb = files[static_cast<std::size_t>(b)];
        switch (mode) {
        case SortMode::kHue:
            return ea.visual.hue_deg < eb.visual.hue_deg;
        case SortMode::kBrightness:
            return ea.visual.light < eb.visual.light;
        case SortMode::kSize:
            return ea.visual.size_px < eb.visual.size_px;
        case SortMode::kAttack:
            return ea.features.attack_s < eb.features.attack_s;
        case SortMode::kTail:
            return ea.features.tail_s < eb.features.tail_s;
        case SortMode::kDeviation:
            if (static_cast<std::size_t>(a) < devs.size() &&
                static_cast<std::size_t>(b) < devs.size()) {
                return devs[static_cast<std::size_t>(a)].max_z >
                       devs[static_cast<std::size_t>(b)].max_z;
            }
            return ea.path < eb.path;
        case SortMode::kName:
        default:
            return ea.path < eb.path;
        }
    };
    std::stable_sort(state.order.begin(), state.order.end(), sort_key_less);

    // Folder sections: group by the scan-root-relative parent folder; sections sorted by
    // path, files inside each section by the active sort mode (already sorted above, and
    // the grouping below is order-preserving).
    state.groups.clear();
    for (int i : state.order) {
        const std::string &path = files[static_cast<std::size_t>(i)].path;
        std::size_t slash = path.find_last_of('/');
        std::string folder = slash == std::string::npos ? "/" : "/" + path.substr(0, slash);
        auto it =
            std::find_if(state.groups.begin(), state.groups.end(),
                         [&folder](const AppState::GridGroup &g) { return g.folder == folder; });
        if (it == state.groups.end()) {
            state.groups.push_back({folder, {}});
            it = std::prev(state.groups.end());
        }
        it->indices.push_back(i);
    }
    std::stable_sort(state.groups.begin(), state.groups.end(),
                     [](const AppState::GridGroup &a, const AppState::GridGroup &b) {
                         return a.folder < b.folder;
                     });
}

void shutdown_scan_thread(AppState &state) {
    if (state.scan_thread.joinable()) {
        state.scan_thread.join();
    }
}

void playback_stop(AppState &state) {
    if (state.active_sound != nullptr) {
        ma_sound *sound = static_cast<ma_sound *>(state.active_sound);
        ma_sound_uninit(sound);
        delete sound;
        state.active_sound = nullptr;
    }
    state.playing_index = -1;
    state.paused = false;
}

void playback_start_path(AppState &state, const std::string &abs_path, int ui_index) {
    // Plays the original file, not the analysis buffer (§10). Skips gracefully headless.
    if (!state.audio_ok || state.engine == nullptr) {
        return;
    }
    playback_stop(state);

    ma_engine *engine = static_cast<ma_engine *>(state.engine);
    ma_sound *sound = new ma_sound;
    if (ma_sound_init_from_file(engine, abs_path.c_str(), 0, nullptr, nullptr, sound) !=
        MA_SUCCESS) {
        delete sound;
        state.status_message = "playback failed: " + abs_path;
        return;
    }
    if (ma_sound_start(sound) != MA_SUCCESS) {
        ma_sound_uninit(sound);
        delete sound;
        state.status_message = "playback failed: " + abs_path;
        return;
    }
    state.active_sound = sound;
    state.playing_index = ui_index;
    state.paused = false;
}

void playback_start(AppState &state, int file_index) {
    if (file_index < 0 || file_index >= static_cast<int>(state.manifest.files.size())) {
        return;
    }
    const sp::FileEntry &e = state.manifest.files[static_cast<std::size_t>(file_index)];
    if (!e.error.empty()) {
        return;
    }
    std::filesystem::path abs = std::filesystem::path(state.root_dir) / e.path;
    playback_start_path(state, abs.string(), file_index);
}

void playback_toggle_pause(AppState &state) {
    if (state.active_sound == nullptr) {
        return;
    }
    ma_sound *sound = static_cast<ma_sound *>(state.active_sound);
    if (state.paused) {
        ma_sound_start(sound);
        state.paused = false;
    } else {
        ma_sound_stop(sound); // ma_sound_stop pauses; the cursor is kept
        state.paused = true;
    }
}

void playback_update(AppState &state) {
    if (state.active_sound == nullptr) {
        return;
    }
    ma_sound *sound = static_cast<ma_sound *>(state.active_sound);
    if (ma_sound_at_end(sound)) {
        playback_stop(state);
    }
}

double playback_remaining_s(const AppState &state) {
    if (state.active_sound == nullptr) {
        return 0.0;
    }
    ma_sound *sound = static_cast<ma_sound *>(state.active_sound);
    float length = 0.0f;
    float cursor = 0.0f;
    if (ma_sound_get_length_in_seconds(sound, &length) != MA_SUCCESS ||
        ma_sound_get_cursor_in_seconds(sound, &cursor) != MA_SUCCESS) {
        return 0.0;
    }
    double remaining = static_cast<double>(length) - static_cast<double>(cursor);
    return remaining > 0.0 ? remaining : 0.0;
}

bool export_svg_to(AppState &state, const std::string &path) {
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        state.status_message = "cannot write " + path;
        return false;
    }
    f << sp::sheet_svg(state.manifest, 8);
    state.status_message = "exported " + path;
    return true;
}

void draw_ui(AppState &state) {
    poll_rescan(state);
    playback_update(state);
    draw_menu_bar(state);

    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
    ImGui::Begin("##main", nullptr, flags);

    const float status_h = ImGui::GetFrameHeightWithSpacing();
    const float s = state.ui_scale();
    const float sidebar_w = 170.0f * s; // panels track the UI scale, not just the fonts
    const float inspector_w = 340.0f * s;

    ImGui::BeginChild("sidebar", ImVec2(sidebar_w, -status_h), ImGuiChildFlags_Borders);
    draw_sidebar(state);
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("grid", ImVec2(-inspector_w - 8.0f * s, -status_h), ImGuiChildFlags_Borders);
    if (ImGui::BeginTabBar("##views")) {
        // Capture the forced-selection flags from the pre-frame view_mode BEFORE any tab
        // callback overwrites it with the currently active tab.
        const ImGuiTabItemFlags grid_flags =
            state.force_view_tab && state.view_mode == ViewMode::kGrid
                ? ImGuiTabItemFlags_SetSelected
                : 0;
        const ImGuiTabItemFlags con_flags =
            state.force_view_tab && state.view_mode == ViewMode::kConstellation
                ? ImGuiTabItemFlags_SetSelected
                : 0;
        state.force_view_tab = false;
        if (ImGui::BeginTabItem("Grid", nullptr, grid_flags)) {
            state.view_mode = ViewMode::kGrid;
            draw_grid(state);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Constellation", nullptr, con_flags)) {
            state.view_mode = ViewMode::kConstellation;
            draw_constellation(state);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("right", ImVec2(0.0f, -status_h), ImGuiChildFlags_Borders);
    draw_inspector(state);
    ImGui::Separator();
    draw_tuner(state);
    ImGui::EndChild();

    draw_status_bar(state);

    // Create-profile name dialog (Profile menu, §7.1): from the whole folder or the current
    // multi-selection; writes <name>.sppal.json under the project root via core and loads it.
    if (state.want_create_profile != 0) {
        ImGui::OpenPopup("Create profile");
    }
    if (ImGui::BeginPopupModal("Create profile", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        static char name_buf[128] = "my_palette";
        ImGui::InputText("name", name_buf, sizeof(name_buf));
        const bool from_selection = state.want_create_profile == 2;
        ImGui::TextDisabled(from_selection ? "from %zu selected sounds" : "from all %zu sounds",
                            from_selection ? state.multi_selected.size()
                                           : state.manifest.files.size());
        if (ImGui::Button("Create") && name_buf[0] != '\0') {
            std::vector<sp::FileEntry> entries;
            if (from_selection) {
                for (int idx : state.multi_selected) {
                    if (idx >= 0 && idx < static_cast<int>(state.manifest.files.size())) {
                        entries.push_back(state.manifest.files[static_cast<std::size_t>(idx)]);
                    }
                }
            } else {
                entries = state.manifest.files;
            }
            sp::Profile p = sp::profile_from_entries(entries, name_buf, "", 2.5, {});
            p.created_from_type = from_selection ? "selection" : "folder";
            p.created_from_root = state.root_dir;
            std::filesystem::path out =
                std::filesystem::path(state.root_dir) / (std::string(name_buf) + ".sppal.json");
            std::ofstream f(out, std::ios::binary);
            if (f) {
                f << sp::profile_to_json(p) << "\n";
                state.profile = std::move(p);
                state.profile_source_path = out.string();
                state.profile_loaded = true;
                recompute_badges(state);
                rebuild_order(state);
                state.status_message = "profile written: " + out.string();
            } else {
                state.status_message = "cannot write " + out.string();
            }
            state.want_create_profile = 0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            state.want_create_profile = 0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::End();
}

} // namespace spapp
