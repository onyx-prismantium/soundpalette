#include "gui/app_state.h"

#include <cmath>
#include <fstream>

#include <imgui.h>
#include <nfd.h>

namespace spapp {

namespace {

// One tuner slider: range ±50 % around the v1 default (§10). Zero defaults (silent visual
// hue/sat) get a small absolute range instead, since ±50 % of 0 is no range at all.
bool slider(const char *label, double *value, double v1_default) {
    double lo, hi;
    if (std::fabs(v1_default) < 1e-12) {
        lo = 0.0;
        hi = 1.0;
    } else {
        lo = v1_default * 0.5;
        hi = v1_default * 1.5;
        if (lo > hi) {
            std::swap(lo, hi); // negative defaults (e.g. LUFS bounds)
        }
    }
    ImGui::SetNextItemWidth(-130.0f);
    return ImGui::SliderScalar(label, ImGuiDataType_Double, value, &lo, &hi, "%.4g");
}

} // namespace

void draw_tuner(AppState &state) {
    ImGui::TextUnformatted("Mapping tuner");
    ImGui::Separator();

    static const sp::MappingConfig defaults; // v1 constants
    sp::MappingConfig &t = state.tuner;
    bool changed = false;

    if (ImGui::CollapsingHeader("Normalization", ImGuiTreeNodeFlags_DefaultOpen)) {
        changed |= slider("bright lo Hz", &t.bright_lo_hz, defaults.bright_lo_hz);
        changed |= slider("bright hi Hz", &t.bright_hi_hz, defaults.bright_hi_hz);
        changed |= slider("warm lo", &t.warm_lo, defaults.warm_lo);
        changed |= slider("warm hi", &t.warm_hi, defaults.warm_hi);
        changed |= slider("ton flat hi", &t.ton_flatness_hi, defaults.ton_flatness_hi);
        changed |= slider("loud lo LUFS", &t.loud_lo_lufs, defaults.loud_lo_lufs);
        changed |= slider("loud hi LUFS", &t.loud_hi_lufs, defaults.loud_hi_lufs);
        changed |= slider("atk lo s", &t.atk_lo_s, defaults.atk_lo_s);
        changed |= slider("atk hi s", &t.atk_hi_s, defaults.atk_hi_s);
        changed |= slider("tail lo s", &t.tail_lo_s, defaults.tail_lo_s);
        changed |= slider("tail hi s", &t.tail_hi_s, defaults.tail_hi_s);
        changed |= slider("flat hi", &t.flat_hi, defaults.flat_hi);
        changed |= slider("rough scale", &t.rough_scale, defaults.rough_scale);
        changed |= slider("jitter flat w", &t.jitter_flat_weight, defaults.jitter_flat_weight);
        changed |= slider("jitter rough w", &t.jitter_rough_weight, defaults.jitter_rough_weight);
    }
    if (ImGui::CollapsingHeader("Visual attributes")) {
        changed |= slider("hue base deg", &t.hue_base_deg, defaults.hue_base_deg);
        changed |= slider("hue warm span", &t.hue_warm_span_deg, defaults.hue_warm_span_deg);
        changed |= slider("sat base", &t.sat_base, defaults.sat_base);
        changed |= slider("sat ton span", &t.sat_ton_span, defaults.sat_ton_span);
        changed |= slider("light base", &t.light_base, defaults.light_base);
        changed |= slider("light brt span", &t.light_bright_span, defaults.light_bright_span);
        changed |= slider("size base px", &t.size_base_px, defaults.size_base_px);
        changed |= slider("size loud span", &t.size_loud_span_px, defaults.size_loud_span_px);
        changed |= slider("spike thresh", &t.spike_threshold, defaults.spike_threshold);
        changed |= slider("spike base", &t.spike_count_base, defaults.spike_count_base);
        changed |= slider("spike span", &t.spike_count_span, defaults.spike_count_span);
    }
    if (ImGui::CollapsingHeader("Silent visual")) {
        changed |= slider("silent hue", &t.silent_hue_deg, defaults.silent_hue_deg);
        changed |= slider("silent sat", &t.silent_sat, defaults.silent_sat);
        changed |= slider("silent light", &t.silent_light, defaults.silent_light);
        changed |= slider("silent size", &t.silent_size_px, defaults.silent_size_px);
    }

    if (changed) {
        rebuild_visuals(state); // live re-derive from cached Features, no re-analysis (§10)
    }

    if (ImGui::Button("Reset to v1")) {
        state.tuner = defaults;
        rebuild_visuals(state);
    }
    ImGui::SameLine();
    if (ImGui::Button("Export mapping.json") && !state.smoke_mode) {
        nfdu8char_t *saved = nullptr;
        nfdu8filteritem_t filter{"JSON", "json"};
        if (NFD_SaveDialogU8(&saved, &filter, 1, nullptr, "mapping.json") == NFD_OKAY) {
            std::ofstream f(saved, std::ios::binary);
            if (f) {
                f << sp::mapping_config_to_json(state.tuner) << "\n";
                state.status_message = std::string("exported ") + saved;
            } else {
                state.status_message = std::string("cannot write ") + saved;
            }
            NFD_FreePathU8(saved);
        }
    }
}

} // namespace spapp
