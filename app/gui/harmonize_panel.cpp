// M9 harmonize section (extension §6.5): badge data, proposal for the selected outlier,
// predicted glyph preview, A/B playback, and Apply. All analysis/DSP comes from core.

#include "gui/app_state.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include <imgui.h>
#include <nlohmann/json.hpp>

#include "soundpalette/glyph.h"
#include "soundpalette/propose.h"

namespace spapp {

namespace {

constexpr const char *kDimNames[8] = {"bright01", "warm01", "ton01",    "atk01",
                                      "tail01",   "loud01", "jitter01", "fluct01"};

// Decodes the selected entry, runs the solver, and derives the predicted visual by applying
// the proposed chain in memory and re-analyzing (the same closed loop harmonize uses).
bool build_proposal(AppState &state, const sp::FileEntry &e) {
    std::filesystem::path abs = std::filesystem::path(state.root_dir) / e.path;
    std::string err;
    auto audio = sp::decode_file_native(abs, err);
    if (!audio.has_value()) {
        state.status_message = err;
        return false;
    }
    sp::Loudness loudness;
    sp::Features features;
    sp::PsychoFeatures psycho;
    sp::analyze_native(*audio, loudness, features, psycho);

    // Category targeting (extension-2 §6.1): pull stats from the file's resolved category.
    const int cat = sp::resolve_category(state.profile, e.path);
    const std::array<sp::DimStats, 8> &target_stats =
        cat >= 0 ? state.profile.categories[static_cast<std::size_t>(cat)].stats
                 : state.profile.stats;
    state.proposal = sp::propose_recipe(*audio, features, loudness, psycho, target_stats,
                                        state.profile.threshold);
    state.proposal.source_path = e.path;
    state.proposal.source_sha256 = sp::file_sha256(abs);
    state.proposal.target_baseline = state.profile_source_path;
    state.proposal_dims_before = sp::mapping_dims(features, loudness, psycho);

    sp::NativeAudio processed = *audio;
    sp::ApplyReport report;
    sp::apply_chain(processed, state.proposal.ops, report);
    sp::Loudness post_loudness;
    sp::Features post_features;
    sp::PsychoFeatures post_psycho;
    sp::analyze_native(processed, post_loudness, post_features, post_psycho);
    state.proposal_dims_after = sp::mapping_dims(post_features, post_loudness, post_psycho);
    state.predicted_visual =
        sp::map_v2(post_features, post_loudness, post_psycho, sp::path_seed(e.path));

    state.proposal_valid = true;
    state.proposal_for = state.selected;
    return true;
}

// Applies the proposal chain to a fresh decode and writes wav (+ optional sidecar).
bool write_processed(AppState &state, const sp::FileEntry &e, const std::filesystem::path &wav_out,
                     bool with_sidecar) {
    std::filesystem::path abs = std::filesystem::path(state.root_dir) / e.path;
    std::string err;
    auto audio = sp::decode_file_native(abs, err);
    if (!audio.has_value()) {
        state.status_message = err;
        return false;
    }
    sp::ApplyReport report;
    sp::apply_chain(*audio, state.proposal.ops, report);
    std::error_code ec;
    std::filesystem::create_directories(wav_out.parent_path(), ec);
    if (!sp::write_wav_f32(wav_out, *audio, err)) {
        state.status_message = err;
        return false;
    }
    if (with_sidecar) {
        std::filesystem::path sidecar = wav_out;
        sidecar.replace_extension(); // strip .wav
        sidecar += ".recipe.json";   // <stem>.harmonized.recipe.json
        std::ofstream f(sidecar, std::ios::binary);
        f << sp::recipe_to_json(state.proposal) << "\n";
    }
    return true;
}

void draw_predicted_glyph(const sp::Visual &v, float size) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##predicted", ImVec2(size, size));
    ImDrawList *draw = ImGui::GetWindowDrawList();
    ImVec2 center(pos.x + size * 0.5f, pos.y + size * 0.5f);
    const float scale = (size * 0.5f - 4.0f) / (64.0f * 1.45f);
    std::vector<std::array<float, 2>> outline = sp::glyph_outline(v);
    std::vector<ImVec2> pts(outline.size());
    for (std::size_t i = 0; i < outline.size(); ++i) {
        pts[i] = ImVec2(center.x + outline[i][0] * scale, center.y + outline[i][1] * scale);
    }
    draw->AddConcavePolyFilled(pts.data(), static_cast<int>(pts.size()),
                               hsl_to_rgba(v.hue_deg, v.sat, v.light, 1.0));
}

} // namespace

bool load_baseline(AppState &state, const std::string &path) {
    std::ifstream f(path);
    if (!f) {
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    const std::string text = ss.str();

    std::string err;
    if (auto profile = sp::profile_from_json(text, err); profile.has_value()) {
        state.profile = std::move(*profile); // native .sppal.json
    } else {
        // Manifest fallback: adapt the stats block into an anonymous profile (M10 §4.3).
        nlohmann::json j;
        try {
            j = nlohmann::json::parse(text);
        } catch (const std::exception &) {
            return false;
        }
        if (!j.contains("stats")) {
            return false;
        }
        sp::Manifest baseline;
        baseline.mapping_version = j.value("mapping_version", 1);
        baseline.ref_spl = j.value("ref_spl", 75.0);
        for (int d = 0; d < 8; ++d) {
            if (!j["stats"].contains(kDimNames[d])) {
                return false;
            }
            const auto &sj = j["stats"][kDimNames[d]];
            auto &out = baseline.stats[static_cast<std::size_t>(d)];
            out.mean = sj.value("mean", 0.0);
            out.std = sj.value("std", 0.0);
            out.min = sj.value("min", 0.0);
            out.max = sj.value("max", 0.0);
        }
        state.profile = sp::profile_from_manifest(baseline);
    }

    // Extension-3 §0: refuse mapping_version/ref_spl mismatches, never mix silently.
    if (std::string compat = sp::profile_compat_error(state.profile); !compat.empty()) {
        state.status_message = compat;
        state.profile = sp::Profile{};
        return false;
    }

    state.profile_source_path = path;
    state.profile_loaded = true;
    state.proposal_valid = false;
    recompute_badges(state);
    return true;
}

void recompute_badges(AppState &state) {
    state.deviations.assign(state.manifest.files.size(), sp::Deviation{});
    if (!state.profile_loaded) {
        return;
    }
    for (std::size_t i = 0; i < state.manifest.files.size(); ++i) {
        state.deviations[i] = sp::compute_deviation(state.manifest.files[i], state.profile);
    }
}

void draw_harmonize(AppState &state) {
    if (!state.profile_loaded || state.selected < 0 ||
        state.selected >= static_cast<int>(state.manifest.files.size())) {
        return;
    }
    const sp::FileEntry &e = state.manifest.files[static_cast<std::size_t>(state.selected)];
    if (!e.error.empty()) {
        return;
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Harmonize");

    const double max_z = state.deviations[static_cast<std::size_t>(state.selected)].max_z;
    if (max_z < state.profile.threshold) {
        ImGui::TextDisabled("within palette (max |z| = %.2f)", max_z);
        return;
    }
    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "off-palette: max |z| = %.2f", max_z);

    if (ImGui::Button("Propose fix")) {
        build_proposal(state, e);
    }
    if (!state.proposal_valid || state.proposal_for != state.selected) {
        return;
    }

    const sp::Recipe &r = state.proposal;
    ImGui::Text("max_z %.2f -> %.2f%s", r.result_max_z_before, r.result_max_z_after,
                r.result_converged ? " (converged)" : "");
    if (!r.result_unresolved.empty()) {
        std::string dims;
        for (std::size_t i = 0; i < r.result_unresolved.size(); ++i) {
            dims += (i ? ", " : "") + r.result_unresolved[i];
        }
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "unresolved: %s", dims.c_str());
    }

    for (const sp::Op &op : r.ops) {
        switch (op.op) {
        case sp::OpType::kGainDb:
            ImGui::BulletText("gain %.1f dB", op.db);
            break;
        case sp::OpType::kGainToLufs:
            ImGui::BulletText("gain to %.1f LUFS (ceiling %.1f dBTP)", op.target_lufs,
                              op.tp_ceiling_db);
            break;
        case sp::OpType::kLowShelf:
            ImGui::BulletText("low shelf %.0f Hz %+.1f dB", op.freq_hz, op.gain_db);
            break;
        case sp::OpType::kHighShelf:
            ImGui::BulletText("high shelf %.0f Hz %+.1f dB", op.freq_hz, op.gain_db);
            break;
        case sp::OpType::kAttackSoften:
            ImGui::BulletText("soften attack %.0f ms", op.fade_ms);
            break;
        case sp::OpType::kTailShorten:
            ImGui::BulletText("shorten tail to %.2f s", op.target_tail_s);
            break;
        }
    }

    // Before/after mini bars for the seven dims.
    for (int d = 0; d < 8; ++d) {
        ImGui::Text("%-9s", kDimNames[d]);
        const float fs = ImGui::GetFontSize();
        ImGui::SameLine(7.0f * fs);
        ImGui::ProgressBar(static_cast<float>(state.proposal_dims_before[d]),
                           ImVec2(5.5f * fs, ImGui::GetTextLineHeight()), "");
        ImGui::SameLine();
        ImGui::ProgressBar(static_cast<float>(state.proposal_dims_after[d]),
                           ImVec2(5.5f * fs, ImGui::GetTextLineHeight()), "");
    }

    ImGui::TextUnformatted("predicted:");
    ImGui::SameLine();
    draw_predicted_glyph(state.predicted_visual, 72.0f * state.ui_scale());

    if (ImGui::Button("Play original")) {
        playback_start(state, state.selected);
    }
    ImGui::SameLine();
    if (ImGui::Button("Play processed")) {
        std::filesystem::path preview =
            std::filesystem::path(state.root_dir) / "harmonized" / "_preview.wav";
        if (write_processed(state, e, preview, false)) {
            playback_start_path(state, preview.string(), -2);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Apply")) {
        std::filesystem::path rel(e.path);
        std::filesystem::path out = std::filesystem::path(state.root_dir) / "harmonized" /
                                    rel.parent_path() / (rel.stem().string() + ".harmonized.wav");
        if (write_processed(state, e, out, true)) {
            state.status_message = "harmonized -> " + out.string();
            start_rescan(state); // the new file joins the grid on the next scan
        }
    }
}

} // namespace spapp
