// Manual and About windows (menu bar). The manual gives each of the eight perceptual
// parameters its own section with a live spectrum strip (five glyphs sweeping only that
// parameter through the real mapping) and, for the color-coded dims, a gradient bar of the
// actual color path; example glyphs are drawn live through the same core mapping visuals and
// grid renderer the real views use.

#include "gui/app_state.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

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

sp::Visual make_visual(double hue, double ton01, double spike01, int spikes, double tail01,
                       double loud01, double sharp01, double rough01, double fluct01,
                       std::uint64_t seed) {
    sp::Visual v;
    v.hue_deg = hue;
    v.sat = 70.0;     // blob saturation is fixed in the ray design
    v.light = 55.0;   // blob lightness is fixed in the split design
    v.size_px = 20.0; // blob size is fixed
    v.ton01 = ton01;
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

// Neutral baseline for the spectrum strips: everything mid/quiet so the one swept parameter
// is the only thing that changes between the five glyphs.
sp::Visual spectrum_base() {
    return make_visual(120.0, 0.65, 0.0, 0, 0.30, 0.5, 0.45, 0.15, 0.25, 11);
}

// Low/high captions under a strip or gradient bar, right label aligned to the strip's edge.
void strip_labels(ImVec2 origin, float width, const char *lo, const char *hi) {
    ImGui::TextDisabled("%s", lo);
    ImGui::SameLine();
    ImVec2 size = ImGui::CalcTextSize(hi);
    ImGui::SetCursorScreenPos(ImVec2(origin.x + width - size.x, ImGui::GetCursorScreenPos().y));
    ImGui::TextDisabled("%s", hi);
}

// Five glyphs sweeping a single parameter from t = 0 to 1 (everything else at the baseline),
// through the same draw_glyph the grid uses — so the strip is the mapping, not an artist's
// impression of it.
template <typename MakeVisualAt>
void spectrum_strip(const char *lo, const char *hi, float cell, MakeVisualAt at) {
    constexpr int kSteps = 5;
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList *draw = ImGui::GetWindowDrawList();
    for (int i = 0; i < kSteps; ++i) {
        const double t = static_cast<double>(i) / (kSteps - 1);
        draw_glyph(draw, at(t),
                   ImVec2(origin.x + (static_cast<float>(i) + 0.5f) * cell, origin.y + 0.5f * cell),
                   cell);
    }
    ImGui::Dummy(ImVec2(cell * kSteps, cell));
    strip_labels(origin, cell * kSteps, lo, hi);
}

// Horizontal gradient bar interpolating hue and saturation in HSL — the literal color path a
// parameter travels, labeled at both ends.
void gradient_bar(double hue_lo, double hue_hi, double sat_lo, double sat_hi, double light,
                  const char *lo, const char *hi, float width, float height) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList *draw = ImGui::GetWindowDrawList();
    constexpr int kBands = 64;
    for (int i = 0; i < kBands; ++i) {
        const float t0 = static_cast<float>(i) / kBands;
        const float t1 = static_cast<float>(i + 1) / kBands;
        const double t = 0.5 * (t0 + t1);
        draw->AddRectFilled(ImVec2(p.x + t0 * width, p.y), ImVec2(p.x + t1 * width, p.y + height),
                            hsl_to_rgba(hue_lo + t * (hue_hi - hue_lo),
                                        sat_lo + t * (sat_hi - sat_lo), light, 1.0));
    }
    draw->AddRect(p, ImVec2(p.x + width, p.y + height), IM_COL32(120, 120, 120, 255));
    ImGui::Dummy(ImVec2(width, height));
    strip_labels(p, width, lo, hi);
}

} // namespace

void draw_manual(AppState &state) {
    const float s = state.ui_scale();
    ImGui::SetNextWindowSize(ImVec2(640.0f * s, 640.0f * s), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Manual", &state.show_manual)) {
        ImGui::End();
        return;
    }

    const sp::MappingConfig &c = sp::active_mapping_config();
    const float cell = 84.0f * s;   // spectrum-strip cell
    const float strip_w = cell * 5; // gradient bars match the strip width
    const float bar_h = 14.0f * s;
    char lo_buf[64], hi_buf[64];

    ImGui::SeparatorText("Background");
    ImGui::TextWrapped(
        "SoundPalette treats a game's sound effects like a color palette. Every audio file is "
        "analyzed for eight perceptual dimensions and a fixed, versioned mapping (mapping v2) "
        "turns them into a glyph. Because the mapping never changes between runs, a cohesive "
        "sound identity looks like a cohesive palette, and off-brand sounds stick out at a "
        "glance. The same analysis drives the CLI's 'palette lint' gate, so what you see here "
        "is what CI checks.");

    ImGui::SeparatorText("How to read a glyph");
    ImGui::TextWrapped(
        "The glyph is split horizontally. The UPPER half is the analytic blob - spectral "
        "character, four parameters: warmth (hue), tonality (rays above the blob), attack "
        "(star spikes), decay tail (trail both sides). The LOWER half is the psychoacoustic "
        "line - perceived character in real units, four parameters: loudness (width, sones), "
        "sharpness (color, acum), roughness (wave height, asper), fluctuation (wave count, "
        "vacil). Each parameter has its own section below; every strip sweeps exactly one "
        "parameter from low to high while the rest stay fixed, drawn through the same mapping "
        "the grid uses.");

    // ---- Blob (upper half) --------------------------------------------------------------
    ImGui::SeparatorText("1. Warmth -> blob hue");
    ImGui::TextWrapped(
        "Warmth is the share of spectral energy in the low and low-mid bands. The hue starts "
        "at blue (%.0f deg) for cold, thin sounds and rotates through cyan, green, yellow, and "
        "orange toward red as warmth grows - the hue path literally runs cold to hot. A bright "
        "UI blip sits blue; a deep explosion or bass swell sits orange/red.",
        c.hue_base_deg);
    spectrum_strip("cold / thin", "warm / bassy", cell, [&](double t) {
        sp::Visual v = spectrum_base();
        v.hue_deg = c.hue_base_deg - c.hue_warm_span_deg * t;
        return v;
    });
    std::snprintf(lo_buf, sizeof lo_buf, "cold (blue, %.0f deg)", c.hue_base_deg);
    std::snprintf(hi_buf, sizeof hi_buf, "warm (red-orange, %.0f deg)",
                  c.hue_base_deg - c.hue_warm_span_deg);
    gradient_bar(c.hue_base_deg, c.hue_base_deg - c.hue_warm_span_deg, 75.0, 75.0, c.blob_light,
                 lo_buf, hi_buf, strip_w, bar_h);

    ImGui::SeparatorText("2. Tonality -> rays above the blob");
    ImGui::TextWrapped(
        "Tonality comes from spectral flatness. A fan of five rays rises from the blob: "
        "pitched, tonal material (chimes, hums, musical stingers) shines perfectly straight "
        "rays; noise-like material (wind, static, impacts) makes them wobble. Think of it as "
        "the sound's radiance - organized sound radiates cleanly, noise flickers.");
    spectrum_strip("noise-like / wavy rays", "tonal / straight rays", cell, [&](double t) {
        sp::Visual v = spectrum_base();
        v.ton01 = t;
        return v;
    });

    ImGui::SeparatorText("3. Attack -> star spikes");
    ImGui::TextWrapped(
        "Attack is how fast the sound reaches its peak (%.0f ms to %.0f ms, log scale). Soft "
        "attacks stay perfectly round; once the attack crosses the spike threshold, the blob "
        "turns into a star - the points grow while the outline between them is carved inward, "
        "so a hard transient is unmistakable at a glance. Clicks and hits are stars; pads and "
        "swells stay round.",
        c.atk_lo_s * 1000.0, c.atk_hi_s * 1000.0);
    spectrum_strip("slow attack / round", "instant attack / star", cell, [&](double t) {
        sp::Visual v = spectrum_base();
        v.spike01 = t;
        v.spikes = t > c.spike_threshold
                       ? static_cast<int>(std::lround(c.spike_count_base + c.spike_count_span * t))
                       : 0;
        return v;
    });

    ImGui::SeparatorText("4. Decay tail -> blob trail");
    ImGui::TextWrapped(
        "The decay tail is how long the sound rings out (%.2f s to %.1f s, log scale). Fading "
        "circles spread symmetrically from both sides of the blob, starting at its edge; the "
        "longer the decay, the wider the wings. Dry one-shots have no trail; long reverbs and "
        "cymbal washes spread far.",
        c.tail_lo_s, c.tail_hi_s);
    spectrum_strip("dry / no trail", "long decay / wide trail", cell, [&](double t) {
        sp::Visual v = spectrum_base();
        v.tail01 = t;
        return v;
    });

    // ---- Psycho line (lower half) -------------------------------------------------------
    ImGui::SeparatorText("5. Loudness -> line width (sones)");
    ImGui::TextWrapped(
        "Loudness is measured per ISO 532-1 in sones under the ref_spl monitoring convention "
        "(1 sone = a 1 kHz tone at 40 dB SPL). The line's width is linear in sones, so its "
        "AREA doubles when perceived loudness doubles - a hairline whisper next to a thick "
        "slab is an honest ratio, not a suggestion.");
    spectrum_strip(
        "silent-ish (hairline)",
        [&] {
            std::snprintf(hi_buf, sizeof hi_buf, "~%.0f sones (max width)",
                          c.loud_sone_div * c.loud_sone_div);
            return hi_buf;
        }(),
        cell,
        [&](double t) {
            sp::Visual v = spectrum_base();
            v.loud01 = t;
            return v;
        });

    ImGui::SeparatorText("6. Sharpness -> line color (acum)");
    ImGui::TextWrapped(
        "Sharpness (DIN 45692, in acum; 1 acum = narrowband noise at 1 kHz, 60 dB) is how "
        "piercing the sound feels. The line color runs the thermal path: deep blue for dull, "
        "muffled sounds, through cyan, green, and yellow, to red for harsh, hissy ones. Note "
        "this is the LINE's color scale - the blob's hue above encodes warmth, a different "
        "dimension with its own (similar-looking) spectrum.");
    spectrum_strip("dull (blue line)", "sharp (red line)", cell, [&](double t) {
        sp::Visual v = spectrum_base();
        v.sharp01 = t;
        return v;
    });
    std::snprintf(lo_buf, sizeof lo_buf, "%.1f acum (dull)", c.bright_acum_lo);
    std::snprintf(hi_buf, sizeof hi_buf, "%.1f acum (sharp)", c.bright_acum_hi);
    gradient_bar(c.sharp_hue_lo_deg, c.sharp_hue_hi_deg, 85.0, 85.0, 55.0, lo_buf, hi_buf, strip_w,
                 bar_h);

    ImGui::SeparatorText("7. Roughness -> wave height (asper)");
    ImGui::TextWrapped(
        "Roughness (Daniel & Weber, in asper; 1 asper = a 1 kHz tone, 60 dB, fully "
        "amplitude-modulated at 70 Hz) is fast, rattly modulation - engines, growls, "
        "distortion. Rough sounds swing the line harder: taller waves at whatever cycle count "
        "fluctuation sets.");
    std::snprintf(hi_buf, sizeof hi_buf, ">= %.1f asper (tall swings)", c.jitter_asper_hi);
    spectrum_strip("steady (near-flat)", hi_buf, cell, [&](double t) {
        sp::Visual v = spectrum_base();
        v.jitter01 = t;
        return v;
    });

    ImGui::SeparatorText("8. Fluctuation -> wave count (vacil)");
    ImGui::TextWrapped(
        "Fluctuation strength (in vacil, experimental; 1 vacil = the 70 Hz reference modulated "
        "at 4 Hz instead) is slow envelope movement - tremolo, breathing, wobble. More "
        "fluctuation adds more wave cycles along the line.");
    std::snprintf(hi_buf, sizeof hi_buf, ">= %.1f vacil (many cycles)", c.fluct_vacil_hi);
    spectrum_strip("static (few cycles)", hi_buf, cell, [&](double t) {
        sp::Visual v = spectrum_base();
        v.fluct01 = t;
        return v;
    });
    ImGui::TextWrapped(
        "The wave carries roughness and fluctuation at once, so both keep a visible minimum: "
        "a rough but steady sound shows few-but-tall waves, a fluctuating but smooth sound "
        "shows many-but-flat ripples.");

    ImGui::SeparatorText("Putting it together");
    const float ex_cell = 96.0f * s;
    if (ImGui::BeginTable("##examples", 2, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("glyph", ImGuiTableColumnFlags_WidthFixed, ex_cell);
        ImGui::TableSetupColumn("text", ImGuiTableColumnFlags_WidthStretch);
        example_row(make_visual(40.0, 0.95, 0.0, 0, 0.85, 0.55, 0.25, 0.05, 0.35, 1),
                    "Warm tonal pad",
                    "Blob: orange hue (warm), straight rays (tonal), round (slow attack), wide "
                    "symmetric trail (long decay). Line: medium width, blue-green (dull), "
                    "nearly flat wave (smooth) with a gentle ripple count.",
                    ex_cell);
        example_row(make_visual(190.0, 0.5, 0.9, 13, 0.15, 0.8, 0.85, 0.2, 0.2, 2),
                    "Bright percussive hit",
                    "Blob: cyan-blue (cold), a many-pointed star (instant attack), no tail. "
                    "Line: thick (loud) and red (sharp), few shallow waves - percussion is "
                    "neither rough nor fluctuating.",
                    ex_cell);
        example_row(make_visual(120.0, 0.1, 0.4, 8, 0.3, 0.6, 0.5, 0.9, 0.25, 3),
                    "Rough texture / grit",
                    "Blob: green with wobbling rays (noisy). Line: tall swings (high asper) at "
                    "a low cycle count - roughness without slow fluctuation.",
                    ex_cell);
        example_row(make_visual(60.0, 0.6, 0.5, 9, 0.25, 0.3, 0.1, 0.1, 0.15, 4), "Quiet dark thud",
                    "Blob: yellow-orange (warm), gently starred (moderate attack). Line: thin "
                    "(quiet) and deep blue (dull), almost calm - the minimum ripple keeps it "
                    "readable.",
                    ex_cell);
        example_row(make_visual(90.0, 0.45, 0.0, 0, 0.6, 0.5, 0.35, 0.1, 0.95, 6),
                    "Breathing drone",
                    "Blob: round, mid-warm. Line: MANY wave cycles at modest height - strong "
                    "slow fluctuation (vacil) without much roughness. Compare with the grit "
                    "row: same wave, opposite parameter.",
                    ex_cell);
        sp::Visual silent = make_visual(0.0, 0.0, 0.0, 0, 0.0, 0.0, 0.0, 0.0, 0.0, 5);
        silent.sat = 0.0;
        silent.light = 60.0;
        silent.size_px = 8.0;
        silent.silent = true;
        example_row(silent, "Silent file",
                    "Files with no measurable loudness get a fixed small gray dot and no line. "
                    "They never contribute to profile statistics or deviations.",
                    ex_cell);
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
