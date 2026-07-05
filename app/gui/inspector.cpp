#include "gui/app_state.h"

#include <algorithm>
#include <cmath>

#include <imgui.h>

namespace spapp {

namespace {

// One anchored perceptual scale bar (§7): folder min-max as the track, ±1σ band shaded, a
// marker for the selected sound, a tick at the unit's definitional anchor, value + unit.
void perception_bar(const char *label, const char *unit, double value, double anchor,
                    double folder_min, double folder_max, double mean, double stddev) {
    const float fs = ImGui::GetFontSize();
    ImGui::Text("%-10s", label);
    ImGui::SameLine(6.5f * fs);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    const float bar_w = 11.0f * fs;
    const float bar_h = ImGui::GetTextLineHeight() * 0.8f;
    ImGui::Dummy(ImVec2(bar_w, ImGui::GetTextLineHeight()));
    ImGui::SameLine();
    ImGui::Text("%.2f %s", value, unit);

    ImDrawList *draw = ImGui::GetWindowDrawList();
    const double lo = std::min({folder_min, anchor, value});
    const double hi = std::max({folder_max, anchor, value, lo + 1e-9});
    auto to_x = [&](double v) { return pos.x + static_cast<float>((v - lo) / (hi - lo)) * bar_w; };
    // Track (folder min..max), ±1σ band, anchor tick, marker.
    draw->AddRectFilled(ImVec2(pos.x, pos.y + bar_h * 0.35f),
                        ImVec2(pos.x + bar_w, pos.y + bar_h * 0.65f), IM_COL32(70, 70, 78, 255));
    draw->AddRectFilled(ImVec2(to_x(mean - stddev), pos.y + bar_h * 0.25f),
                        ImVec2(to_x(mean + stddev), pos.y + bar_h * 0.75f),
                        IM_COL32(99, 153, 34, 70));
    draw->AddLine(ImVec2(to_x(anchor), pos.y), ImVec2(to_x(anchor), pos.y + bar_h),
                  IM_COL32(160, 160, 160, 200), 1.0f);
    draw->AddCircleFilled(ImVec2(to_x(value), pos.y + bar_h * 0.5f), bar_h * 0.35f,
                          IM_COL32(120, 170, 220, 255));
}

// Folder statistics of one psycho metric (reuses the scanned manifest; no new analysis).
void folder_stats(const AppState &state, double (*get)(const sp::PsychoFeatures &), double &out_min,
                  double &out_max, double &out_mean, double &out_std) {
    out_min = 1e30;
    out_max = -1e30;
    double sum = 0.0, sumsq = 0.0;
    int n = 0;
    for (const sp::FileEntry &f : state.manifest.files) {
        if (!f.error.empty() || f.loudness.silent) {
            continue;
        }
        const double v = get(f.psycho);
        out_min = std::min(out_min, v);
        out_max = std::max(out_max, v);
        sum += v;
        sumsq += v * v;
        ++n;
    }
    if (n == 0) {
        out_min = out_max = out_mean = out_std = 0.0;
        return;
    }
    out_mean = sum / n;
    out_std = std::sqrt(std::max(0.0, sumsq / n - out_mean * out_mean));
}

void draw_perception_bars(const AppState &state, const sp::FileEntry &e) {
    struct Row {
        const char *label;
        const char *unit;
        double value;
        double anchor; // the unit's definitional reference value
        double (*get)(const sp::PsychoFeatures &);
    };
    const Row rows[4] = {
        {"loudness", "sones", e.psycho.sones_n5, 1.0,
         [](const sp::PsychoFeatures &p) { return p.sones_n5; }},
        {"sharpness", "acum", e.psycho.sharpness_acum, 1.0,
         [](const sp::PsychoFeatures &p) { return p.sharpness_acum; }},
        {"roughness", "asper", e.psycho.roughness_asper, 1.0,
         [](const sp::PsychoFeatures &p) { return p.roughness_asper; }},
        {"fluctuation", "vacil", e.psycho.fluctuation_vacil, 1.0,
         [](const sp::PsychoFeatures &p) { return p.fluctuation_vacil; }},
    };
    for (const Row &r : rows) {
        double mn, mx, mean, sd;
        folder_stats(state, r.get, mn, mx, mean, sd);
        perception_bar(r.label, r.unit, r.value, r.anchor, mn, mx, mean, sd);
    }
    ImGui::TextDisabled("ref_spl %.1f dB SPL; fluctuation is experimental", e.psycho.ref_spl);
}

} // namespace

void draw_inspector(AppState &state) {
    ImGui::TextUnformatted("Inspector");
    ImGui::Separator();

    if (state.selected < 0 || state.selected >= static_cast<int>(state.manifest.files.size())) {
        ImGui::TextDisabled("Click a glyph to inspect it");
        return;
    }
    const sp::FileEntry &e = state.manifest.files[static_cast<std::size_t>(state.selected)];

    ImGui::TextWrapped("%s", e.path.c_str());
    if (!e.error.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "error: %s", e.error.c_str());
        return;
    }

    if (ImGui::CollapsingHeader("Decode", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("duration    %.4f s%s", e.duration_s, e.truncated ? " (truncated)" : "");
        ImGui::Text("source      %d Hz, %d ch", e.sample_rate, e.channels);
        ImGui::Text("sha256      %.16s...", e.sha256.c_str());
        ImGui::Text("loudness    %.2f LUFS", e.loudness.lufs_i);
        ImGui::Text("true peak   %.2f dBTP", e.loudness.true_peak_db);
        ImGui::Text("silent      %s", e.loudness.silent ? "yes" : "no");
    }

    if (ImGui::CollapsingHeader("Features", ImGuiTreeNodeFlags_DefaultOpen)) {
        const sp::Features &f = e.features;
        ImGui::Text("centroid    %.1f Hz", f.centroid_hz);
        ImGui::Text("rolloff85   %.1f Hz", f.rolloff85_hz);
        ImGui::Text("flatness    %.4f", f.flatness);
        ImGui::Text("zcr         %.4f", f.zcr);
        ImGui::Text("attack      %.4f s", f.attack_s);
        ImGui::Text("tail        %.4f s%s", f.tail_s, f.tail_clipped ? " (clipped)" : "");
        ImGui::Text("roughness   %.4f", f.roughness);
        ImGui::Text("warmth      %.4f", f.warmth);
        ImGui::Text("bands       %.2f %.2f %.2f %.2f %.2f %.2f", f.bands[0], f.bands[1], f.bands[2],
                    f.bands[3], f.bands[4], f.bands[5]);
    }

    if (ImGui::CollapsingHeader("Perception", ImGuiTreeNodeFlags_DefaultOpen)) {
        draw_perception_bars(state, e);
    }

    if (ImGui::CollapsingHeader("Visual", ImGuiTreeNodeFlags_DefaultOpen)) {
        const sp::Visual &v = e.visual;
        ImGui::Text("blob hue    %.1f deg (warmth)", v.hue_deg);
        ImGui::Text("blob sat    %.1f %% (tonality)", v.sat);
        ImGui::Text("spike01     %.2f (%d spikes)", v.spike01, v.spikes);
        ImGui::Text("tail01      %.2f", v.tail01);
        ImGui::Text("line loud01 %.2f (width)", v.loud01);
        ImGui::Text("line sharp  %.2f (blue->red)", v.sharp01);
        ImGui::Text("line rough  %.2f (wave amp)", v.jitter01);
        ImGui::Text("line fluct  %.2f (wave freq)", v.fluct01);
        ImGui::SameLine();
        ImGui::ColorButton(
            "##swatch", ImGui::ColorConvertU32ToFloat4(hsl_to_rgba(v.hue_deg, v.sat, v.light, 1.0)),
            ImGuiColorEditFlags_NoTooltip,
            ImVec2(3.0f * ImGui::GetFontSize(), 1.5f * ImGui::GetFontSize()));
    }

    draw_deviation_section(state, state.selected); // M11: category, max z, seven z bars
    draw_harmonize(state); // M9 section, shown when a profile is loaded (extension §6.5)
}

} // namespace spapp
