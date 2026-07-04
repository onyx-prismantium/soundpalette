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
    ImGui::EndMainMenuBar();
}

void draw_sidebar(AppState &state) {
    ImGui::TextUnformatted("Sort by");
    static const char *kModes[] = {"hue", "brightness", "size", "attack", "tail", "name"};
    int mode = static_cast<int>(state.sort_mode);
    for (int i = 0; i < 6; ++i) {
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
}

void draw_status_bar(AppState &state) {
    if (state.scanning.load()) {
        ImGui::Text("analyzed %zu/%zu", state.scan_done.load(), state.scan_total.load());
    } else {
        ImGui::Text("%zu files | scan %.2f s | mapping v%d | %s", state.manifest.files.size(),
                    state.last_scan_seconds, state.manifest.mapping_version,
                    state.audio_ok ? "audio on" : "audio off");
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
    rebuild_order(state);
}

void rebuild_order(AppState &state) {
    const std::vector<sp::FileEntry> &files = state.manifest.files;
    state.order.clear();
    state.order.reserve(files.size());
    std::string needle(state.filter_text);
    for (int i = 0; i < static_cast<int>(files.size()); ++i) {
        if (contains_case_insensitive(files[static_cast<std::size_t>(i)].path, needle)) {
            state.order.push_back(i);
        }
    }

    SortMode mode = state.sort_mode;
    std::stable_sort(state.order.begin(), state.order.end(), [&files, mode](int a, int b) {
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
        case SortMode::kName:
        default:
            return ea.path < eb.path;
        }
    });
}

void shutdown_scan_thread(AppState &state) {
    if (state.scan_thread.joinable()) {
        state.scan_thread.join();
    }
}

void play_entry(AppState &state, const sp::FileEntry &entry) {
    // Plays the original file, not the analysis buffer (§10). Skips gracefully headless.
    if (!state.audio_ok || state.engine == nullptr || !entry.error.empty()) {
        return;
    }
    std::filesystem::path abs = std::filesystem::path(state.root_dir) / entry.path;
    ma_engine *engine = static_cast<ma_engine *>(state.engine);
    if (ma_engine_play_sound(engine, abs.string().c_str(), nullptr) != MA_SUCCESS) {
        state.status_message = "playback failed: " + entry.path;
    }
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
    draw_menu_bar(state);

    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
    ImGui::Begin("##main", nullptr, flags);

    const float status_h = ImGui::GetFrameHeightWithSpacing();
    const float sidebar_w = 170.0f;
    const float inspector_w = 340.0f;

    ImGui::BeginChild("sidebar", ImVec2(sidebar_w, -status_h), ImGuiChildFlags_Borders);
    draw_sidebar(state);
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("grid", ImVec2(-inspector_w - 8.0f, -status_h), ImGuiChildFlags_Borders);
    draw_grid(state);
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("right", ImVec2(0.0f, -status_h), ImGuiChildFlags_Borders);
    draw_inspector(state);
    ImGui::Separator();
    draw_tuner(state);
    ImGui::EndChild();

    draw_status_bar(state);
    ImGui::End();
}

} // namespace spapp
