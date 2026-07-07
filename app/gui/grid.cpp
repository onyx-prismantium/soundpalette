#include "gui/app_state.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <unordered_map>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h> // ImRsqrt: the cached fill must fringe exactly like ImGui's

#include "soundpalette/glyph.h"

namespace spapp {

namespace {

constexpr float kCellPx = 118.0f; // glyph area at 100 % scale; tightened so glyphs sit closer
constexpr float kLabelPx = 16.0f; // filename line beneath the glyph, at 100 % scale

const char *kDimNames[8] = {"bright01", "warm01", "ton01",    "atk01",
                            "tail01",   "loud01", "jitter01", "fluct01"};

// Scale so the largest possible glyph envelope fits the cell: with blob_size_px 20 the
// widest extent is the mirrored tail (~49 authored px from center) and the tallest is the
// ray tips (~41 px above the blob center). The 2 px margin keeps neighboring glyphs from
// touching even at full tail spread while packing the grid tight.
float glyph_scale(float cell_px) {
    return (cell_px * 0.5f - 2.0f) / 55.0f;
}

// --- Cached concave fill -----------------------------------------------------------
// AddConcavePolyFilled re-runs an O(N*R) ear-clipping triangulation every call, i.e.
// per glyph per frame — with attack on (144-point star outline, ~50 reflex vertices)
// that alone cost ~4.5 ms per 60 visible glyphs and made the app sluggish. The triangle
// indices depend only on the polygon's SHAPE, which is invariant under the per-cell
// translation and uniform scale, so each distinct shape is triangulated once (by
// ImGui's own triangulator, through a scratch draw list with AA off — the plain fill's
// index buffer IS the triangulation) and re-emitted every frame below with the exact
// vertex/fringe layout of the original. Spike-less blobs stay on the plain call: they
// are convex, ImGui's reflex list stays empty, and the ear test is effectively free.
// (The decay tail used to be the other concave-fill hotspot; it is stroked arcs now.)

std::vector<ImDrawIdx> triangulate_once(ImDrawList *ref, const ImVec2 *pts, int n) {
    ImDrawList tmp(ref->_Data);
    tmp._ResetForNewFrame();
    tmp.Flags = ImDrawListFlags_None;
    tmp.AddConcavePolyFilled(pts, n, IM_COL32_WHITE);
    return std::vector<ImDrawIdx>(tmp.IdxBuffer.begin(), tmp.IdxBuffer.end());
}

// Byte-identical output to AddConcavePolyFilled (imgui_draw.cpp), minus the per-frame
// triangulation. `tris` holds indices relative to the polygon's own 0..n-1 order.
void add_concave_poly_cached(ImDrawList *draw, const ImVec2 *points, int points_count,
                             ImU32 col, const std::vector<ImDrawIdx> &tris) {
    if (points_count < 3 || (col & IM_COL32_A_MASK) == 0 || tris.empty()) {
        return;
    }
    const ImVec2 uv = draw->_Data->TexUvWhitePixel;
    if (draw->Flags & ImDrawListFlags_AntiAliasedFill) {
        const float aa_size = draw->_FringeScale;
        const ImU32 col_trans = col & ~IM_COL32_A_MASK;
        draw->PrimReserve(static_cast<int>(tris.size()) + points_count * 6, points_count * 2);

        const unsigned int vtx_inner_idx = draw->_VtxCurrentIdx;
        const unsigned int vtx_outer_idx = draw->_VtxCurrentIdx + 1;
        for (const ImDrawIdx t : tris) {
            *draw->_IdxWritePtr++ =
                static_cast<ImDrawIdx>(vtx_inner_idx + (static_cast<unsigned int>(t) << 1));
        }

        // Edge normals, then per-point inner/outer vertex pairs and fringe quads — the
        // same math (IM_NORMALIZE2F_OVER_ZERO / IM_FIXNORMAL2F) as the ImGui original.
        static std::vector<ImVec2> normals;
        normals.resize(static_cast<std::size_t>(points_count));
        for (int i0 = points_count - 1, i1 = 0; i1 < points_count; i0 = i1++) {
            float dx = points[i1].x - points[i0].x;
            float dy = points[i1].y - points[i0].y;
            const float d2 = dx * dx + dy * dy;
            if (d2 > 0.0f) {
                const float inv_len = ImRsqrt(d2);
                dx *= inv_len;
                dy *= inv_len;
            }
            normals[static_cast<std::size_t>(i0)] = ImVec2(dy, -dx);
        }
        for (int i0 = points_count - 1, i1 = 0; i1 < points_count; i0 = i1++) {
            const ImVec2 &n0 = normals[static_cast<std::size_t>(i0)];
            const ImVec2 &n1 = normals[static_cast<std::size_t>(i1)];
            float dm_x = (n0.x + n1.x) * 0.5f;
            float dm_y = (n0.y + n1.y) * 0.5f;
            const float d2 = dm_x * dm_x + dm_y * dm_y;
            if (d2 > 0.000001f) {
                const float inv_len2 = std::min(1.0f / d2, 100.0f);
                dm_x *= inv_len2;
                dm_y *= inv_len2;
            }
            dm_x *= aa_size * 0.5f;
            dm_y *= aa_size * 0.5f;

            draw->_VtxWritePtr[0].pos = ImVec2(points[i1].x - dm_x, points[i1].y - dm_y);
            draw->_VtxWritePtr[0].uv = uv;
            draw->_VtxWritePtr[0].col = col; // inner
            draw->_VtxWritePtr[1].pos = ImVec2(points[i1].x + dm_x, points[i1].y + dm_y);
            draw->_VtxWritePtr[1].uv = uv;
            draw->_VtxWritePtr[1].col = col_trans; // outer
            draw->_VtxWritePtr += 2;

            const unsigned int u0 = static_cast<unsigned int>(i0) << 1;
            const unsigned int u1 = static_cast<unsigned int>(i1) << 1;
            draw->_IdxWritePtr[0] = static_cast<ImDrawIdx>(vtx_inner_idx + u1);
            draw->_IdxWritePtr[1] = static_cast<ImDrawIdx>(vtx_inner_idx + u0);
            draw->_IdxWritePtr[2] = static_cast<ImDrawIdx>(vtx_outer_idx + u0);
            draw->_IdxWritePtr[3] = static_cast<ImDrawIdx>(vtx_outer_idx + u0);
            draw->_IdxWritePtr[4] = static_cast<ImDrawIdx>(vtx_outer_idx + u1);
            draw->_IdxWritePtr[5] = static_cast<ImDrawIdx>(vtx_inner_idx + u1);
            draw->_IdxWritePtr += 6;
        }
        draw->_VtxCurrentIdx += static_cast<unsigned int>(points_count * 2);
    } else {
        draw->PrimReserve(static_cast<int>(tris.size()), points_count);
        const unsigned int base = draw->_VtxCurrentIdx;
        for (int i = 0; i < points_count; ++i) {
            draw->_VtxWritePtr[0].pos = points[i];
            draw->_VtxWritePtr[0].uv = uv;
            draw->_VtxWritePtr[0].col = col;
            ++draw->_VtxWritePtr;
        }
        for (const ImDrawIdx t : tris) {
            *draw->_IdxWritePtr++ = static_cast<ImDrawIdx>(base + static_cast<unsigned int>(t));
        }
        draw->_VtxCurrentIdx += static_cast<unsigned int>(points_count);
    }
}

// Starred-outline triangulations keyed by spike01 — the only Visual field that changes
// the outline's shape (spikes>0 always samples 144 points; size/position only scale and
// translate). Tuner remaps just add entries; a few hundred files at ~1 KB each is noise.
void fill_outline(ImDrawList *draw, const std::vector<ImVec2> &pts, const sp::Visual &m,
                  unsigned int col) {
    if (m.spikes <= 0) {
        draw->AddConcavePolyFilled(pts.data(), static_cast<int>(pts.size()), col);
        return; // convex blob: the plain path is already cheap
    }
    static std::unordered_map<float, std::vector<ImDrawIdx>> cache;
    if (cache.size() > 1024) {
        cache.clear(); // the manual's attack slider sweeps spike01; don't grow unbounded
    }
    const float key = static_cast<float>(m.spike01);
    auto it = cache.find(key);
    if (it == cache.end()) {
        it = cache.emplace(key, triangulate_once(draw, pts.data(), static_cast<int>(pts.size())))
                 .first;
    }
    add_concave_poly_cached(draw, pts.data(), static_cast<int>(pts.size()), col, it->second);
}

} // namespace

// Also used by the manual's example glyphs (manual.cpp). Split glyph: analytic blob in the
// upper half of the cell, psychoacoustic line in the lower half (silent files: dot only).
// A single part draws centered in the cell instead of at its half's offset. The mask
// (sidebar parameter checkboxes) neutralizes unchecked encodings at render time.
void draw_glyph(ImDrawList *draw, const sp::Visual &v, ImVec2 center, float cell_px, GlyphPart part,
                const GlyphMask &mask) {
    const float scale = glyph_scale(cell_px);
    const ImVec2 blob_center(center.x, part == GlyphPart::blob ? center.y + 0.05f * cell_px
                                                               : center.y - 0.14f * cell_px);

    // Neutralize unchecked parameters on a render-local copy; the manifest Visual and
    // everything computed from it (lint, inspector, deviations) stay untouched.
    sp::Visual m = v;
    if (!mask.warmth) {
        m.sat = 0.0; // gray blob: color carries no information
    }
    if (!mask.attack) {
        m.spike01 = 0.0;
        m.spikes = 0;
    }
    if (!mask.tail) {
        m.tail01 = 0.0;
    }
    if (!mask.loudness) {
        m.loud01 = 0.0; // hairline
    }
    if (!mask.roughness) {
        m.jitter01 = 0.0;
    }
    if (!mask.fluctuation) {
        m.fluct01 = 0.0;
    }

    if (part != GlyphPart::line && mask.blob_shown()) {
        std::vector<std::array<float, 2>> outline = sp::glyph_outline(m);
        std::vector<ImVec2> pts(outline.size());
        for (std::size_t i = 0; i < outline.size(); ++i) {
            pts[i] = ImVec2(blob_center.x + outline[i][0] * scale,
                            blob_center.y + outline[i][1] * scale);
        }
        unsigned int fill = hsl_to_rgba(m.hue_deg, m.sat, m.light, 1.0);
        fill_outline(draw, pts, m, fill);

        // Tonality rays: straight fan = tonal, wobbling fan = noise-like (same geometry as
        // the SVG sheet via glyph_rays).
        if (mask.tonality) {
            for (const std::vector<std::array<float, 2>> &ray : sp::glyph_rays(m)) {
                for (const std::array<float, 2> &p : ray) {
                    draw->PathLineTo(
                        ImVec2(blob_center.x + p[0] * scale, blob_center.y + p[1] * scale));
                }
                draw->PathStroke(fill, 0, std::max(1.0f, 1.6f * scale));
            }
        }

        // Decay tail (C revision): 5 fading stroked arcs on EACH side, opening toward
        // the blob (same geometry as the SVG sheet via glyph_tail). Plain path strokes:
        // the previous filled crescents re-triangulated per frame and made the tail
        // toggle sluggish even with the triangulation cached.
        for (const sp::TailArc &arc : sp::glyph_tail(m)) {
            for (const std::array<float, 2> &p : arc.pts) {
                draw->PathLineTo(
                    ImVec2(blob_center.x + p[0] * scale, blob_center.y + p[1] * scale));
            }
            draw->PathStroke(hsl_to_rgba(m.hue_deg, m.sat, m.light, arc.opacity), 0,
                             std::max(1.0f, arc.width * scale));
        }
    }

    if (v.silent || part == GlyphPart::blob || !mask.line_shown()) {
        return; // silent: the fixed gray dot above is the whole story
    }

    // Psycho line (lower half): width = loudness (linear in sones -> honest line area),
    // color blue->red = sharpness, sine amplitude = roughness, frequency = fluctuation;
    // enforced minimums keep both wave parameters visible (same math as the SVG sheet).
    const sp::MappingConfig &c = sp::active_mapping_config();
    const double sones = (m.loud01 * c.loud_sone_div) * (m.loud01 * c.loud_sone_div);
    const float cell_norm = cell_px / 120.0f; // constants are authored at the 120 px SVG cell
    const float width =
        static_cast<float>(std::clamp(c.line_width_min_px + c.line_width_per_sone_px * sones,
                                      c.line_width_min_px, c.line_width_max_px)) *
        cell_norm;
    const float amp = static_cast<float>(c.line_amp_min_px +
                                         m.jitter01 * (c.line_amp_max_px - c.line_amp_min_px)) *
                      cell_norm;
    const double cycles = c.line_cycles_min + m.fluct01 * (c.line_cycles_max - c.line_cycles_min);
    const double hue = c.sharp_hue_lo_deg + m.sharp01 * (c.sharp_hue_hi_deg - c.sharp_hue_lo_deg);
    // Gray line: color carries nothing; drawn lighter than the colored line's L=55 so the
    // hairline (loudness off too) still separates from the dark background.
    const double line_sat = mask.sharpness ? 85.0 : 0.0;
    const double line_light = mask.sharpness ? 55.0 : 72.0;

    const float x0 = center.x - 0.42f * cell_px;
    const float x1 = center.x + 0.42f * cell_px;
    const float ly = part == GlyphPart::line ? center.y : center.y + 0.30f * cell_px;
    const int n = 48;
    for (int i = 0; i <= n; ++i) {
        const float t = static_cast<float>(i) / n;
        draw->PathLineTo(
            ImVec2(x0 + t * (x1 - x0),
                   ly - amp * static_cast<float>(std::sin(2.0 * 3.14159265358979 * cycles * t))));
    }
    draw->PathStroke(hsl_to_rgba(hue, line_sat, line_light, 1.0), 0, std::max(1.0f, width));
}

namespace {

// ImGui has no dashed stroke; draw a dashed circle as alternating short arcs (~dash 4 px,
// gap 3 px at 100 % scale — extension-2 §7.1 amber style).
void add_dashed_circle(ImDrawList *draw, ImVec2 center, float radius, unsigned int color,
                       float thickness, float ui_scale) {
    const float dash = 4.0f * ui_scale;
    const float gap = 3.0f * ui_scale;
    const float circumference = 2.0f * 3.14159265f * radius;
    const int pairs = std::max(4, static_cast<int>(circumference / (dash + gap)));
    const float step = 2.0f * 3.14159265f / static_cast<float>(pairs);
    const float dash_angle = step * dash / (dash + gap);
    for (int i = 0; i < pairs; ++i) {
        const float a0 = static_cast<float>(i) * step;
        draw->PathArcTo(center, radius, a0, a0 + dash_angle, 8);
        draw->PathStroke(color, 0, thickness);
    }
}

// Deviation halo (extension-2 §7.1): ring behind the glyph; band + line style pair so color
// is never the only cue. Red: solid, width 2 + min(3, max_z - T). Amber: dashed, width 2.
void draw_halo(ImDrawList *draw, const sp::Deviation &dev, double threshold, ImVec2 center,
               float radius, float ui_scale) {
    if (dev.band == sp::DevBand::red) {
        const float w =
            (2.0f + std::min(3.0f, static_cast<float>(dev.max_z - threshold))) * ui_scale;
        draw->AddCircle(center, radius, IM_COL32(226, 75, 74, 235), 0, w);
    } else if (dev.band == sp::DevBand::amber) {
        add_dashed_circle(draw, center, radius, IM_COL32(239, 159, 39, 235), 2.0f * ui_scale,
                          ui_scale);
    }
}

void draw_error_mark(ImDrawList *draw, ImVec2 center, float cell_px) {
    const float half = cell_px * 0.18f;
    const unsigned int gray = IM_COL32(136, 136, 136, 255);
    draw->AddLine(ImVec2(center.x - half, center.y - half),
                  ImVec2(center.x + half, center.y + half), gray, 3.0f);
    draw->AddLine(ImVec2(center.x + half, center.y - half),
                  ImVec2(center.x - half, center.y + half), gray, 3.0f);
}

void draw_tooltip(const sp::FileEntry &e) {
    ImGui::BeginTooltip();
    ImGui::TextUnformatted(e.path.c_str());
    if (e.error.empty()) {
        ImGui::Text("%.3f s  |  %.1f LUFS", e.duration_s, e.loudness.lufs_i);
        ImGui::Separator();
        std::array<double, 8> dims = sp::mapping_dims(e.features, e.loudness, e.psycho);
        for (int d = 0; d < 8; ++d) {
            ImGui::Text("%-9s", kDimNames[d]);
            ImGui::SameLine(7.0f * ImGui::GetFontSize());
            ImGui::ProgressBar(static_cast<float>(dims[static_cast<std::size_t>(d)]),
                               ImVec2(9.0f * ImGui::GetFontSize(), ImGui::GetTextLineHeight()), "");
        }
    } else {
        ImGui::TextUnformatted(e.error.c_str());
    }
    ImGui::EndTooltip();
}

} // namespace

void draw_grid(AppState &state) {
    const std::vector<sp::FileEntry> &files = state.manifest.files;
    if (state.order.empty()) {
        ImGui::TextDisabled(files.empty() ? "No folder open (File > Open folder...)"
                                          : "No files match the filter");
        return;
    }

    const float s = state.ui_scale();
    const float cell = kCellPx * s;
    const float label_h = kLabelPx * s;
    const float controls_h = ImGui::GetFrameHeightWithSpacing();

    const float avail_w = ImGui::GetContentRegionAvail().x;
    const int columns = std::max(1, static_cast<int>(avail_w / cell));
    const float row_h = cell + label_h + controls_h;

    // Folder sections: full folder path above, that folder's glyphs, a divider below.
    // Subfolders of the opened project folder become the grid's areas.
    for (std::size_t g = 0; g < state.groups.size(); ++g) {
        const AppState::GridGroup &group = state.groups[g];
        ImGui::PushID(static_cast<int>(g));

        ImGui::TextColored(ImVec4(0.62f, 0.72f, 0.86f, 1.0f), "%s", group.folder.c_str());
        ImGui::Spacing();

        const int rows = (static_cast<int>(group.indices.size()) + columns - 1) / columns;

        // Only visible rows are drawn (§10) — one clipper per section keeps that guarantee
        // while section headers stay cheap unconditional text.
        ImGuiListClipper clipper;
        clipper.Begin(rows, row_h);
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const ImVec2 row_base = ImGui::GetCursorScreenPos();
                for (int col = 0; col < columns; ++col) {
                    int slot = row * columns + col;
                    if (slot >= static_cast<int>(group.indices.size())) {
                        break;
                    }
                    int file_index = group.indices[static_cast<std::size_t>(slot)];
                    const sp::FileEntry &e = files[static_cast<std::size_t>(file_index)];

                    ImGui::PushID(slot);
                    ImVec2 cell_pos(row_base.x + col * cell, row_base.y);
                    ImGui::SetCursorScreenPos(cell_pos);
                    // Glyph + label area only; the transport row below has its own widgets.
                    ImGui::InvisibleButton("cell", ImVec2(cell, cell + label_h));

                    bool hovered = ImGui::IsItemHovered();
                    if (ImGui::IsItemClicked()) {
                        if (ImGui::GetIO().KeyCtrl) {
                            // Ctrl+click builds the multi-selection for
                            // Profile > Create from current selection (§7.1).
                            if (!state.multi_selected.insert(file_index).second) {
                                state.multi_selected.erase(file_index);
                            }
                        } else {
                            state.selected = file_index;
                        }
                    }

                    ImDrawList *draw = ImGui::GetWindowDrawList();
                    ImVec2 center(cell_pos.x + cell * 0.5f, cell_pos.y + cell * 0.5f);

                    if (state.selected == file_index) {
                        draw->AddRect(cell_pos, ImVec2(cell_pos.x + cell, cell_pos.y + row_h),
                                      IM_COL32(255, 255, 255, 80), 4.0f);
                    }
                    if (state.multi_selected.count(file_index) != 0) {
                        draw->AddRect(cell_pos, ImVec2(cell_pos.x + cell, cell_pos.y + row_h),
                                      IM_COL32(120, 190, 255, 160), 4.0f, 0, 2.0f);
                    }
                    const sp::Deviation *dev =
                        state.profile_loaded &&
                                file_index < static_cast<int>(state.deviations.size())
                            ? &state.deviations[static_cast<std::size_t>(file_index)]
                            : nullptr;
                    const bool conforming = dev == nullptr || dev->band == sp::DevBand::none;
                    if (!e.error.empty()) {
                        draw_error_mark(draw, center, cell);
                    } else {
                        // Halo behind the glyph (§7.1); "dim conforming" fades the rest so
                        // strays pop.
                        if (dev != nullptr && state.show_halos) {
                            draw_halo(draw, *dev, state.profile.threshold, center, cell * 0.46f, s);
                        }
                        if (state.dim_conforming && conforming) {
                            // The faded ghost honors the parameter checkboxes too.
                            sp::Visual v = e.visual;
                            if (!state.show_warmth) {
                                v.sat = 0.0;
                            }
                            if (!state.show_attack) {
                                v.spike01 = 0.0;
                                v.spikes = 0;
                            }
                            if (state.show_warmth || state.show_tonality || state.show_attack ||
                                state.show_tail) {
                                std::vector<std::array<float, 2>> outline = sp::glyph_outline(v);
                                std::vector<ImVec2> pts(outline.size());
                                const float gs = glyph_scale(cell);
                                for (std::size_t k = 0; k < outline.size(); ++k) {
                                    pts[k] = ImVec2(center.x + outline[k][0] * gs,
                                                    center.y + outline[k][1] * gs);
                                }
                                fill_outline(draw, pts, v,
                                             hsl_to_rgba(v.hue_deg, v.sat, v.light, 0.35));
                            }
                        } else {
                            const GlyphMask mask{state.show_warmth,    state.show_tonality,
                                                 state.show_attack,    state.show_tail,
                                                 state.show_loudness,  state.show_sharpness,
                                                 state.show_roughness, state.show_fluctuation};
                            draw_glyph(draw, e.visual, center, cell, GlyphPart::full, mask);
                        }
                        // z labels are independent of the halo toggle (either works alone).
                        if (dev != nullptr && state.show_z_labels &&
                            dev->band != sp::DevBand::none) {
                            char zbuf[16];
                            std::snprintf(zbuf, sizeof(zbuf), "z %.1f", dev->max_z);
                            const unsigned int zcol = dev->band == sp::DevBand::red
                                                          ? IM_COL32(226, 75, 74, 255)
                                                          : IM_COL32(239, 159, 39, 255);
                            draw->AddText(ImVec2(cell_pos.x + 4.0f * s, cell_pos.y + 2.0f * s),
                                          zcol, zbuf);
                        }
                    }

                    // Filename only beneath the glyph (the folder is the section title).
                    std::size_t slash = e.path.find_last_of('/');
                    std::string label =
                        slash == std::string::npos ? e.path : e.path.substr(slash + 1);
                    ImVec2 text_size = ImGui::CalcTextSize(label.c_str());
                    while (text_size.x > cell - 8.0f && label.size() > 4) {
                        label = label.substr(0, label.size() - 4) + "..."; // no U+2026 in font
                        text_size = ImGui::CalcTextSize(label.c_str());
                    }
                    draw->AddText(
                        ImVec2(cell_pos.x + (cell - text_size.x) * 0.5f, cell_pos.y + cell),
                        IM_COL32(204, 204, 204, 255), label.c_str());

                    if (hovered) {
                        draw_tooltip(e);
                    }

                    // Transport row: play/pause/stop + seconds (remaining while playing).
                    ImGui::SetCursorScreenPos(
                        ImVec2(cell_pos.x + 4.0f * s, cell_pos.y + cell + label_h));
                    if (e.error.empty()) {
                        const bool is_playing = state.playing_index == file_index;
                        ImGui::BeginDisabled(!state.audio_ok);
                        if (is_playing) {
                            if (ImGui::SmallButton(state.paused ? ">" : "||")) {
                                playback_toggle_pause(state);
                            }
                            ImGui::SameLine();
                            if (ImGui::SmallButton("stop")) {
                                playback_stop(state);
                            }
                            ImGui::SameLine();
                            ImGui::Text("%.1fs", playback_remaining_s(state));
                        } else {
                            if (ImGui::SmallButton(">")) {
                                state.selected = file_index;
                                playback_start(state, file_index);
                            }
                            ImGui::SameLine();
                            ImGui::Text("%.1fs", e.duration_s);
                        }
                        ImGui::EndDisabled();
                    } else {
                        ImGui::TextDisabled("--");
                    }

                    ImGui::PopID();
                }
                // Advance the layout cursor exactly one row, independent of cell widgets.
                ImGui::SetCursorScreenPos(row_base);
                ImGui::Dummy(ImVec2(avail_w, row_h));
            }
        }
        clipper.End();

        if (g + 1 < state.groups.size()) {
            ImGui::Separator(); // visible divider to the next folder
            ImGui::Spacing();
        }
        ImGui::PopID();
    }
}

} // namespace spapp
