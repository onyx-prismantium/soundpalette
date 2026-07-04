#include "gui/app_state.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

#include <imgui.h>

#include "soundpalette/glyph.h"

namespace spapp {

namespace {

constexpr float kCellPx = 132.0f; // glyph area at 100 % scale; §9's 120 px SVG cell + margin
constexpr float kLabelPx = 16.0f; // filename line beneath the glyph, at 100 % scale
constexpr int kTailCircles = 5;   // §7 decay tail

const char *kDimNames[7] = {"bright01", "warm01", "ton01", "atk01", "tail01", "loud01", "jitter01"};

// Glyphs are authored at size_px up to 64 (radius) with spikes up to +45 %; scale so the
// largest possible glyph plus its tail fits the cell.
float glyph_scale(float cell_px) {
    return (cell_px * 0.5f - 6.0f) / (64.0f * 1.45f);
}

void draw_glyph(ImDrawList *draw, const sp::FileEntry &e, ImVec2 center, float cell_px) {
    const sp::Visual &v = e.visual;
    const float scale = glyph_scale(cell_px);

    std::vector<std::array<float, 2>> outline = sp::glyph_outline(v);
    std::vector<ImVec2> pts(outline.size());
    for (std::size_t i = 0; i < outline.size(); ++i) {
        pts[i] = ImVec2(center.x + outline[i][0] * scale, center.y + outline[i][1] * scale);
    }
    unsigned int fill = hsl_to_rgba(v.hue_deg, v.sat, v.light, 1.0);
    draw->AddConcavePolyFilled(pts.data(), static_cast<int>(pts.size()), fill);

    // Decay tail (§7): 5 circles to the right, shrinking/fading, spread tail01 * 2.2 * size.
    if (v.tail01 >= 0.05) {
        const double spread = v.tail01 * 2.2 * v.size_px;
        for (int i = 0; i < kTailCircles; ++i) {
            double t = static_cast<double>(i) / (kTailCircles - 1);
            double x = center.x + ((i + 1) / static_cast<double>(kTailCircles)) * spread * scale;
            double radius = 0.16 * v.size_px * (1.0 - 0.8 * t) * scale;
            double opacity = 0.5 + (0.07 - 0.5) * t;
            draw->AddCircleFilled(ImVec2(static_cast<float>(x), center.y),
                                  static_cast<float>(radius),
                                  hsl_to_rgba(v.hue_deg, v.sat, v.light, opacity));
        }
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
        std::array<double, 7> dims = sp::mapping_dims(e.features, e.loudness);
        for (int d = 0; d < 7; ++d) {
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
                        state.selected = file_index;
                    }

                    ImDrawList *draw = ImGui::GetWindowDrawList();
                    ImVec2 center(cell_pos.x + cell * 0.5f, cell_pos.y + cell * 0.5f);

                    if (state.selected == file_index) {
                        draw->AddRect(cell_pos, ImVec2(cell_pos.x + cell, cell_pos.y + row_h),
                                      IM_COL32(255, 255, 255, 80), 4.0f);
                    }
                    // M9 badge: red outline when off-palette vs the loaded baseline.
                    if (state.baseline_loaded &&
                        file_index < static_cast<int>(state.max_z.size()) && e.error.empty() &&
                        state.max_z[static_cast<std::size_t>(file_index)] >=
                            state.harmonize_threshold) {
                        draw->AddRect(cell_pos, ImVec2(cell_pos.x + cell, cell_pos.y + row_h),
                                      IM_COL32(255, 64, 64, 200), 4.0f, 0, 2.0f);
                    }
                    if (!e.error.empty()) {
                        draw_error_mark(draw, center, cell);
                    } else {
                        draw_glyph(draw, e, center, cell);
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
