// Manual and About windows (menu bar). The manual explains the palette metaphor, how to read
// a glyph, and the deviation/constellation views; example glyphs are drawn live through the
// same core mapping visuals and grid renderer the real views use.

#include "gui/app_state.h"

#include <imgui.h>

#include "soundpalette/version.h"

namespace spapp {

namespace {

// One manual row: an example glyph rendered into a fixed cell, description text beside it.
// The Visual values are hand-picked to match mapping v1 (§7) for the described sound.
void example_row(const sp::Visual &v, const char *title, const char *text, float cell) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(cell, cell));
    draw_glyph(ImGui::GetWindowDrawList(), v, ImVec2(pos.x + cell * 0.5f, pos.y + cell * 0.5f),
               cell);
    ImGui::TableSetColumnIndex(1);
    ImGui::TextColored(ImVec4(0.62f, 0.72f, 0.86f, 1.0f), "%s", title);
    ImGui::TextWrapped("%s", text);
}

sp::Visual make_visual(double hue, double sat, double light, double size, double spike01,
                       int spikes, double jitter01, double tail01, std::uint64_t seed) {
    sp::Visual v;
    v.hue_deg = hue;
    v.sat = sat;
    v.light = light;
    v.size_px = size;
    v.spike01 = spike01;
    v.spikes = spikes;
    v.jitter01 = jitter01;
    v.tail01 = tail01;
    v.seed = seed;
    return v;
}

} // namespace

void draw_manual(AppState &state) {
    const float s = state.ui_scale();
    ImGui::SetNextWindowSize(ImVec2(620.0f * s, 560.0f * s), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Manual", &state.show_manual)) {
        ImGui::End();
        return;
    }

    ImGui::SeparatorText("Background");
    ImGui::TextWrapped(
        "SoundPalette treats a game's sound effects like a color palette. Every audio file is "
        "analyzed for seven perceptual dimensions - brightness, warmth, tonality, attack, decay "
        "tail, loudness, and noisiness - and a fixed, versioned mapping (mapping v1) turns them "
        "into a glyph. Because the mapping never changes between runs, a cohesive sound identity "
        "looks like a cohesive palette, and off-brand sounds stick out at a glance. The same "
        "analysis drives the CLI's 'palette lint' gate, so what you see here is what CI checks.");

    ImGui::SeparatorText("How to read a glyph");
    ImGui::TextWrapped(
        "Each visual property is driven by exactly one audio dimension (two for edge jitter):");
    ImGui::Bullet();
    ImGui::TextWrapped("Hue <- warmth: cold, thin sounds sit at blue (220 deg); warmth rotates "
                       "the hue through green and yellow toward orange/red.");
    ImGui::Bullet();
    ImGui::TextWrapped("Lightness <- brightness (spectral centroid): dark rumbles render dark, "
                       "airy or hissy sounds render light.");
    ImGui::Bullet();
    ImGui::TextWrapped("Saturation <- tonality: pitched, tonal material is vivid; noise-like "
                       "material washes out toward gray.");
    ImGui::Bullet();
    ImGui::TextWrapped("Size <- loudness (integrated LUFS): louder files draw bigger glyphs.");
    ImGui::Bullet();
    ImGui::TextWrapped("Spikes <- attack: fast attacks grow spikes (more and longer as the "
                       "attack sharpens); soft attacks stay round.");
    ImGui::Bullet();
    ImGui::TextWrapped("Edge jitter <- noisiness + roughness: flat, gritty spectra make the "
                       "outline wobble (deterministic per file - the same file always jitters "
                       "the same way).");
    ImGui::Bullet();
    ImGui::TextWrapped("Tail <- decay tail: fading circles trail to the right; the longer the "
                       "decay, the wider the trail.");

    ImGui::SeparatorText("Example glyphs");
    const float cell = 96.0f * s;
    if (ImGui::BeginTable("##examples", 2, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("glyph", ImGuiTableColumnFlags_WidthFixed, cell);
        ImGui::TableSetupColumn("text", ImGuiTableColumnFlags_WidthStretch);
        example_row(make_visual(40.0, 82.0, 46.0, 39.0, 0.0, 0, 0.05, 0.85, 1),
                    "Warm tonal pad", // warm01 .9, ton01 .95, tail01 .85
                    "High warmth pulls the hue to orange, strong tonality saturates it, and the "
                    "long decay leaves a wide trail of tail circles. No spikes: the attack is "
                    "slow, so the body stays round.",
                    cell);
        example_row(make_visual(190.0, 55.0, 73.0, 51.0, 0.9, 13, 0.15, 0.15, 2),
                    "Bright percussive hit", // bright01 .9, atk01 .9, loud01 .75
                    "A fast attack grows many long spikes, high brightness makes it light, and "
                    "the cold character keeps the hue cyan-blue. Loud, so the glyph is large; "
                    "short decay, so almost no tail.",
                    cell);
        example_row(make_visual(120.0, 31.0, 55.0, 36.0, 0.4, 8, 0.85, 0.3, 3),
                    "Noisy texture / grit", // ton01 .1, jitter01 .85
                    "Noise-like content desaturates the fill toward gray-green and drives heavy "
                    "edge jitter - the wobbly outline is the signature of flat, rough spectra.",
                    cell);
        example_row(make_visual(60.0, 61.0, 33.0, 21.0, 0.5, 9, 0.1, 0.25, 4),
                    "Quiet dark thud", // bright01 .1, loud01 .15, warm01 .8
                    "Low brightness renders it dark and low loudness keeps it small. Warmth "
                    "still shows in the yellow-orange hue; a moderately sharp attack adds a few "
                    "short spikes.",
                    cell);
        example_row(make_visual(0.0, 0.0, 60.0, 8.0, 0.0, 0, 0.0, 0.0, 5),
                    "Silent file", // fixed silent visual (§7)
                    "Files with no measurable loudness get a fixed small gray dot. They never "
                    "contribute to profile statistics or deviations.",
                    cell);
        ImGui::EndTable();
    }

    ImGui::SeparatorText("Deviation marks (profile loaded)");
    ImGui::TextWrapped(
        "With a palette profile loaded (Profile > Load / Create), every sound gets a z-score per "
        "dimension against its category's statistics. Red solid halo: max |z| at or above the "
        "profile threshold - a real stray. Amber dashed halo: within 80%% of the threshold - "
        "borderline. Halos always pair color with a line style so color is never the only cue. "
        "The sidebar toggles halos and z labels independently; 'dim conforming' fades in-palette "
        "sounds and 'outliers only' filters the grid to flagged files.");
    ImGui::TextWrapped(
        "Genre presets (Profile > Load preset) are designed profiles: hand-authored priors for "
        "Sci-Fi, Horror, Fantasy, and Retro 8-bit with deliberately generous tolerances - "
        "guardrails, not measurements. The footer marks them [designed]. A profile created from "
        "your own sounds is always the stricter, more honest reference.");

    ImGui::SeparatorText("Views");
    ImGui::TextWrapped(
        "Grid: one glyph per file, grouped into folder sections, with per-cell playback "
        "transport. Click selects, Ctrl+click builds a multi-selection for Profile > Create "
        "from current selection.");
    ImGui::TextWrapped(
        "Constellation: the loaded set plotted against the profile's 1-sigma (solid green) and "
        "2-sigma (dashed) region. Pick any two dimensions or PCA axes; filter by category. "
        "Mouse wheel zooms about the cursor, left-drag pans, 'reset view' restores the fit. "
        "Click selects a point, double-click plays it, Space plays the current selection.");

    ImGui::End();
}

void draw_about(AppState &state) {
    const float s = state.ui_scale();
    ImGui::SetNextWindowSize(ImVec2(420.0f * s, 0.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("About", &state.show_about,
                      ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }
    ImGui::Text("SoundPalette %s", sp::kVersionString);
    ImGui::TextDisabled("mapping v%d", sp::active_mapping_config().mapping_version);
    ImGui::Separator();
    ImGui::TextWrapped(
        "A palette view for game audio: scans a folder of sound effects, maps perceptual "
        "features to glyphs through a fixed versioned mapping, and makes off-brand sounds "
        "visible - and lintable - at a glance. Ships as a headless core library, a CLI (CI "
        "palette-lint gate), and this desktop app.");
    ImGui::Separator();
    ImGui::TextWrapped("Third-party licenses: see DEPENDENCIES.md and LICENSES.md in the "
                       "repository root.");
    ImGui::End();
}

} // namespace spapp
