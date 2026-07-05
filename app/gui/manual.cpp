// Manual and About windows (menu bar). The manual explains the palette metaphor, how to read
// a glyph, and the deviation/constellation views; example glyphs are drawn live through the
// same core mapping visuals and grid renderer the real views use.

#include "gui/app_state.h"

#include <imgui.h>

#include "soundpalette/version.h"

namespace spapp {

namespace {

// One manual row: an example glyph rendered into a fixed cell, description text beside it.
// The Visual values are hand-picked to match mapping v2 (§7 + extension-3 §6).
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

sp::Visual make_visual(double hue, double sat, double spike01, int spikes, double tail01,
                       double loud01, double sharp01, double rough01, double fluct01,
                       std::uint64_t seed) {
    sp::Visual v;
    v.hue_deg = hue;
    v.sat = sat;
    v.light = 55.0;   // blob lightness is fixed in the split design
    v.size_px = 26.0; // blob size is fixed
    v.spike01 = spike01;
    v.spikes = spikes;
    v.tail01 = tail01;
    v.loud01 = loud01;
    v.sharp01 = sharp01;
    v.jitter01 = rough01;
    v.fluct01 = fluct01;
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
        "analyzed for eight perceptual dimensions - sharpness, warmth, tonality, attack, decay "
        "tail, loudness, roughness, and fluctuation (the loudness, sharpness, roughness, and "
        "fluctuation dims are real psychoacoustic units: sones, acum, asper, vacil) - and a "
        "fixed, versioned mapping (mapping v2) turns them into a glyph. Because the mapping never "
        "changes between runs, a cohesive sound identity "
        "looks like a cohesive palette, and off-brand sounds stick out at a glance. The same "
        "analysis drives the CLI's 'palette lint' gate, so what you see here is what CI checks.");

    ImGui::SeparatorText("How to read a glyph");
    ImGui::TextWrapped(
        "The glyph is split horizontally. The UPPER half is the analytic blob (spectral "
        "character, fixed size); the LOWER half is the psychoacoustic line (perceived "
        "character in real units).");
    ImGui::TextUnformatted("Blob (upper half):");
    ImGui::Bullet();
    ImGui::TextWrapped("Hue <- warmth: cold, thin sounds sit at blue (220 deg); warmth rotates "
                       "the hue through green and yellow toward orange/red.");
    ImGui::Bullet();
    ImGui::TextWrapped("Saturation <- tonality: pitched, tonal material is vivid; noise-like "
                       "material washes out toward gray.");
    ImGui::Bullet();
    ImGui::TextWrapped("Spikes <- attack: fast attacks grow spikes (more and longer as the "
                       "attack sharpens); soft attacks stay round.");
    ImGui::Bullet();
    ImGui::TextWrapped("Trail <- decay tail: fading circles trail to the right; the longer the "
                       "decay, the wider the trail.");
    ImGui::TextUnformatted("Line (lower half):");
    ImGui::Bullet();
    ImGui::TextWrapped("Width <- loudness (ISO 532-1, in sones): the line's thickness is linear "
                       "in sones, so its AREA doubles when loudness doubles. 1 sone = a 1 kHz "
                       "tone at 40 dB SPL under the ref_spl monitoring convention.");
    ImGui::Bullet();
    ImGui::TextWrapped("Color <- sharpness (DIN 45692, in acum): blue = dull, through green and "
                       "yellow to red = sharp. 1 acum = narrowband noise at 1 kHz, 60 dB.");
    ImGui::Bullet();
    ImGui::TextWrapped("Wave height <- roughness (Daniel & Weber, in asper): rough, rattling "
                       "sounds swing the line harder. 1 asper = a 1 kHz tone, 60 dB, fully "
                       "amplitude-modulated at 70 Hz.");
    ImGui::Bullet();
    ImGui::TextWrapped("Wave count <- fluctuation strength (in vacil, experimental): slow "
                       "envelope movement adds more wave cycles. 1 vacil = the 70 Hz reference "
                       "modulated at 4 Hz instead.");
    ImGui::TextWrapped(
        "The wave carries two parameters at once, so both keep a visible minimum: a rough but "
        "steady sound shows few-but-tall waves, a fluctuating but smooth sound shows "
        "many-but-flat ripples.");

    ImGui::SeparatorText("Example glyphs");
    const float cell = 96.0f * s;
    if (ImGui::BeginTable("##examples", 2, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("glyph", ImGuiTableColumnFlags_WidthFixed, cell);
        ImGui::TableSetupColumn("text", ImGuiTableColumnFlags_WidthStretch);
        example_row(make_visual(40.0, 82.0, 0.0, 0, 0.85, 0.55, 0.25, 0.05, 0.35, 1),
                    "Warm tonal pad",
                    "Blob: orange hue (warm), vivid (tonal), round (slow attack), wide tail "
                    "trail (long decay). Line: medium width, blue-green (dull), nearly flat "
                    "wave (smooth) with a gentle ripple count.",
                    cell);
        example_row(make_visual(190.0, 55.0, 0.9, 13, 0.15, 0.8, 0.85, 0.2, 0.2, 2),
                    "Bright percussive hit",
                    "Blob: cyan-blue (cold), many long spikes (instant attack), no tail. Line: "
                    "thick (loud) and red (sharp), few shallow waves - percussion is neither "
                    "rough nor fluctuating.",
                    cell);
        example_row(make_visual(120.0, 31.0, 0.4, 8, 0.3, 0.6, 0.5, 0.9, 0.25, 3),
                    "Rough texture / grit",
                    "Blob: desaturated gray-green (noisy). Line: tall swings (high asper) at a "
                    "low cycle count - roughness without slow fluctuation.",
                    cell);
        example_row(make_visual(60.0, 61.0, 0.5, 9, 0.25, 0.3, 0.1, 0.1, 0.15, 4),
                    "Quiet dark thud",
                    "Blob: yellow-orange (warm), a few short spikes. Line: thin (quiet) and "
                    "deep blue (dull), almost calm - the minimum ripple keeps it readable.",
                    cell);
        example_row(make_visual(90.0, 50.0, 0.0, 0, 0.6, 0.5, 0.35, 0.1, 0.95, 6),
                    "Breathing drone",
                    "Blob: round, mid-warm. Line: MANY wave cycles at modest height - strong "
                    "slow fluctuation (vacil) without much roughness. Compare with the grit "
                    "row: same wave, opposite parameter.",
                    cell);
        sp::Visual silent = make_visual(0.0, 0.0, 0.0, 0, 0.0, 0.0, 0.0, 0.0, 0.0, 5);
        silent.light = 60.0;
        silent.size_px = 8.0;
        silent.silent = true;
        example_row(silent, "Silent file",
                    "Files with no measurable loudness get a fixed small gray dot and no line. "
                    "They never contribute to profile statistics or deviations.",
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
