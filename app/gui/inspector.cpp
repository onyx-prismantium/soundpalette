#include "gui/app_state.h"

#include <imgui.h>

namespace spapp {

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

    if (ImGui::CollapsingHeader("Visual", ImGuiTreeNodeFlags_DefaultOpen)) {
        const sp::Visual &v = e.visual;
        ImGui::Text("hue         %.1f deg", v.hue_deg);
        ImGui::Text("sat/light   %.1f %% / %.1f %%", v.sat, v.light);
        ImGui::Text("size        %.1f px", v.size_px);
        ImGui::Text("spike01     %.2f (%d spikes)", v.spike01, v.spikes);
        ImGui::Text("jitter01    %.2f", v.jitter01);
        ImGui::Text("tail01      %.2f", v.tail01);
        ImGui::SameLine();
        ImGui::ColorButton(
            "##swatch", ImGui::ColorConvertU32ToFloat4(hsl_to_rgba(v.hue_deg, v.sat, v.light, 1.0)),
            ImGuiColorEditFlags_NoTooltip, ImVec2(40, 20));
    }

    draw_harmonize(state); // M9 section, shown when a baseline is loaded (extension §6.5)
}

} // namespace spapp
