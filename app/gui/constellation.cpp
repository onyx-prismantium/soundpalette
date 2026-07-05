// M11 constellation view (extension-2 §7.2): 2-D scatter of the loaded set against the
// profile's 1-sigma/2-sigma region, with axis pickers and deterministic PCA. All math comes
// from core (mapping_dims, pca.h, profile cov); this file only projects and draws.

#include "gui/app_state.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include <imgui.h>

#include "soundpalette/pca.h"

namespace spapp {

namespace {

constexpr const char *kDimNames[8] = {"bright01", "warm01", "ton01",    "atk01",
                                      "tail01",   "loud01", "jitter01", "fluct01"};
constexpr const char *kAxisNames[9] = {"bright01", "warm01",   "ton01", "atk01", "tail01",
                                       "loud01",   "jitter01", "PCA 1", "PCA 2"};
constexpr float kPi = 3.14159265f;

struct Point {
    int file_index;
    double x, y;
    const sp::FileEntry *entry;
    const sp::Deviation *dev;
};

// Ellipse outline for k*sigma of the 2x2 covariance, in data space.
std::vector<ImVec2> ellipse_points(double cx, double cy, const sp::Eigen2 &eig, double k,
                                   const std::function<ImVec2(double, double)> &to_screen) {
    std::vector<ImVec2> pts;
    const double r1 = k * std::sqrt(std::max(0.0, eig.lambda1));
    const double r2 = k * std::sqrt(std::max(0.0, eig.lambda2));
    const double ct = std::cos(eig.theta), st = std::sin(eig.theta);
    for (int i = 0; i < 96; ++i) {
        const double a = 2.0 * kPi * i / 96.0;
        const double ex = r1 * std::cos(a), ey = r2 * std::sin(a);
        pts.push_back(to_screen(cx + ex * ct - ey * st, cy + ex * st + ey * ct));
    }
    return pts;
}

void add_dashed_polyline(ImDrawList *draw, const std::vector<ImVec2> &pts, unsigned int color,
                         float thickness) {
    for (std::size_t i = 0; i < pts.size(); i += 2) {
        draw->AddLine(pts[i], pts[(i + 1) % pts.size()], color, thickness);
    }
}

void add_dashed_line(ImDrawList *draw, ImVec2 a, ImVec2 b, unsigned int color, float thickness) {
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    const int segments = std::max(1, static_cast<int>(len / 8.0f));
    for (int i = 0; i < segments; i += 2) {
        const float t0 = static_cast<float>(i) / segments;
        const float t1 = static_cast<float>(i + 1) / segments;
        draw->AddLine(ImVec2(a.x + dx * t0, a.y + dy * t0), ImVec2(a.x + dx * t1, a.y + dy * t1),
                      color, thickness);
    }
}

} // namespace

void draw_deviation_section(AppState &state, int file_index) {
    if (!state.profile_loaded || file_index < 0 ||
        file_index >= static_cast<int>(state.deviations.size())) {
        return;
    }
    const sp::Deviation &dev = state.deviations[static_cast<std::size_t>(file_index)];
    const double t = state.profile.threshold;

    ImGui::Separator();
    ImGui::TextUnformatted("Deviation");
    ImGui::Text("category    %s", dev.category.empty() ? "(top-level)" : dev.category.c_str());
    ImGui::Text("max |z|     %.2f (%s), worst %s", dev.max_z, sp::dev_band_name(dev.band),
                kDimNames[dev.worst_dim]);

    // Seven z bars with the +-T band shaded (extension-2 §7.1). Scale: +-2T across the bar.
    ImDrawList *draw = ImGui::GetWindowDrawList();
    const float fs = ImGui::GetFontSize();
    const float bar_w = 11.0f * fs;
    const float bar_h = ImGui::GetTextLineHeight() * 0.8f;
    for (int d = 0; d < 8; ++d) {
        ImGui::Text("%-9s", kDimNames[d]);
        ImGui::SameLine(7.0f * fs);
        ImVec2 pos = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(bar_w, ImGui::GetTextLineHeight()));
        const float cx = pos.x + bar_w * 0.5f;
        // Shaded +-T band, center line, then the z bar.
        const float t_half = bar_w * 0.25f; // T maps to a quarter width (scale +-2T)
        draw->AddRectFilled(ImVec2(cx - t_half, pos.y), ImVec2(cx + t_half, pos.y + bar_h),
                            IM_COL32(90, 110, 90, 70));
        draw->AddLine(ImVec2(cx, pos.y), ImVec2(cx, pos.y + bar_h), IM_COL32(160, 160, 160, 120));
        const double z = dev.z[static_cast<std::size_t>(d)];
        const float extent =
            static_cast<float>(std::clamp(z / (2.0 * t), -1.0, 1.0)) * bar_w * 0.5f;
        const bool over = std::fabs(z) >= t;
        draw->AddRectFilled(ImVec2(cx, pos.y + 1.0f), ImVec2(cx + extent, pos.y + bar_h - 1.0f),
                            over ? IM_COL32(226, 75, 74, 220) : IM_COL32(120, 170, 220, 200));
    }
}

void draw_constellation(AppState &state) {
    if (!state.profile_loaded) {
        ImGui::TextDisabled("Load a profile (Profile > Load...) to plot the constellation");
        return;
    }
    const std::vector<sp::FileEntry> &files = state.manifest.files;
    if (files.empty()) {
        ImGui::TextDisabled("No folder open");
        return;
    }
    state.view_note.clear();

    // Axis pickers + category dropdown (§7.2). Changing either resets zoom/pan: the old view
    // window is meaningless in the new data space.
    auto reset_view = [&state] {
        state.constellation_zoom = 1.0;
        state.constellation_pan_x = 0.0;
        state.constellation_pan_y = 0.0;
    };
    const float fs = ImGui::GetFontSize();
    ImGui::SetNextItemWidth(8.0f * fs);
    if (ImGui::Combo("X", &state.constellation_axis_x, kAxisNames, 9)) {
        reset_view();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(8.0f * fs);
    if (ImGui::Combo("Y", &state.constellation_axis_y, kAxisNames, 9)) {
        reset_view();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(8.0f * fs);
    std::vector<const char *> cat_names{"all"};
    for (const sp::CategoryProfile &c : state.profile.categories) {
        cat_names.push_back(c.name.c_str());
    }
    int cat_combo = state.constellation_category + 1;
    if (ImGui::Combo("category", &cat_combo, cat_names.data(),
                     static_cast<int>(cat_names.size()))) {
        state.constellation_category = cat_combo - 1;
        reset_view();
    }
    ImGui::SameLine();
    const bool view_is_fit = state.constellation_zoom == 1.0 && state.constellation_pan_x == 0.0 &&
                             state.constellation_pan_y == 0.0;
    ImGui::BeginDisabled(view_is_fit);
    if (ImGui::SmallButton("reset view")) {
        reset_view();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("wheel: zoom, drag: pan");

    // Point set: non-silent, non-error files, filtered to the selected category (§7.2).
    std::vector<int> plotted;
    std::vector<std::array<double, 8>> plotted_dims;
    for (int i = 0; i < static_cast<int>(files.size()); ++i) {
        const sp::FileEntry &e = files[static_cast<std::size_t>(i)];
        if (!e.error.empty() || e.loudness.silent) {
            continue;
        }
        if (state.constellation_category >= 0) {
            if (sp::resolve_category(state.profile, e.path) != state.constellation_category) {
                continue;
            }
        }
        plotted.push_back(i);
        plotted_dims.push_back(sp::mapping_dims(e.features, e.loudness, e.psycho));
    }

    // PCA over the plotted set when a PCA axis is chosen (§7.2), with fallback.
    int axis_x = state.constellation_axis_x;
    int axis_y = state.constellation_axis_y;
    sp::Pca2 pca;
    const bool wants_pca = axis_x >= 7 || axis_y >= 7;
    if (wants_pca) {
        pca = sp::compute_pca2(plotted_dims);
        if (!pca.valid) {
            axis_x = 0;
            axis_y = 1;
            state.view_note = "PCA unavailable for this set; showing bright01/warm01";
        }
    }
    auto project = [&](const std::array<double, 8> &dims, int axis) {
        if (axis >= 7) {
            const std::array<double, 8> &pc = axis == 7 ? pca.pc1 : pca.pc2;
            double v = 0.0;
            for (std::size_t d = 0; d < 8; ++d) {
                v += (dims[d] - pca.mean[d]) * pc[d];
            }
            return v;
        }
        return dims[static_cast<std::size_t>(axis)];
    };

    // Profile region stats for the selected category.
    const std::array<sp::DimStats, 8> &region_stats =
        state.constellation_category >= 0
            ? state.profile.categories[static_cast<std::size_t>(state.constellation_category)].stats
            : state.profile.stats;
    const sp::Cov8 &region_cov =
        state.constellation_category >= 0
            ? state.profile.categories[static_cast<std::size_t>(state.constellation_category)].cov
            : state.profile.cov;

    // Fixed square plot area with 10 % padding (§7.2).
    ImVec2 avail = ImGui::GetContentRegionAvail();
    const float side = std::max(120.0f, std::min(avail.x, avail.y));
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##plot", ImVec2(side, side));
    ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY); // wheel zooms the plot, not the panel scroll
    ImDrawList *draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, ImVec2(origin.x + side, origin.y + side),
                        IM_COL32(24, 24, 27, 255));
    draw->AddRect(origin, ImVec2(origin.x + side, origin.y + side), IM_COL32(70, 70, 78, 255));

    // Data range: points plus the 2-sigma region, then 10 % padding.
    double min_x = 1e30, max_x = -1e30, min_y = 1e30, max_y = -1e30;
    std::vector<Point> points;
    for (std::size_t k = 0; k < plotted.size(); ++k) {
        Point p;
        p.file_index = plotted[k];
        p.entry = &files[static_cast<std::size_t>(plotted[k])];
        p.dev = &state.deviations[static_cast<std::size_t>(plotted[k])];
        p.x = project(plotted_dims[k], axis_x);
        p.y = project(plotted_dims[k], axis_y);
        min_x = std::min(min_x, p.x);
        max_x = std::max(max_x, p.x);
        min_y = std::min(min_y, p.y);
        max_y = std::max(max_y, p.y);
        points.push_back(p);
    }

    const bool dims_axes = axis_x < 7 && axis_y < 7;
    sp::Eigen2 eig{};
    double region_cx = 0.0, region_cy = 0.0;
    bool have_region = false;
    if (dims_axes) {
        // 1-sigma/2-sigma ellipses from the 2x2 submatrix of cov (§7.2 — honest, not a box).
        const auto ax = static_cast<std::size_t>(axis_x);
        const auto ay = static_cast<std::size_t>(axis_y);
        eig = sp::eigen2x2(region_cov[ax][ax], region_cov[ax][ay], region_cov[ay][ay]);
        region_cx = region_stats[ax].mean;
        region_cy = region_stats[ay].mean;
        have_region = true;
    } else if (wants_pca && pca.valid) {
        // PCA axes: the projected covariance is diag(lambda1, lambda2) by construction.
        eig.lambda1 = axis_x == 7 || axis_y == 8 ? pca.lambda1 : pca.lambda2;
        eig.lambda2 = axis_x == 7 || axis_y == 8 ? pca.lambda2 : pca.lambda1;
        eig.theta = 0.0;
        region_cx = 0.0;
        region_cy = 0.0;
        have_region = true;
    }
    if (have_region) {
        const double r1 = 2.0 * std::sqrt(std::max(0.0, eig.lambda1));
        min_x = std::min(min_x, region_cx - r1);
        max_x = std::max(max_x, region_cx + r1);
        min_y = std::min(min_y, region_cy - r1);
        max_y = std::max(max_y, region_cy + r1);
    }
    if (points.empty() && !have_region) {
        ImGui::TextDisabled("nothing to plot");
        return;
    }
    const double span_x = std::max(1e-6, max_x - min_x);
    const double span_y = std::max(1e-6, max_y - min_y);
    const double pad_x = span_x * 0.1, pad_y = span_y * 0.1;
    min_x -= pad_x;
    max_x += pad_x;
    min_y -= pad_y;
    max_y += pad_y;

    // Apply zoom/pan: shrink the fit window around the panned center.
    {
        const double cx = (min_x + max_x) * 0.5 + state.constellation_pan_x;
        const double cy = (min_y + max_y) * 0.5 + state.constellation_pan_y;
        const double hx = (max_x - min_x) * 0.5 / state.constellation_zoom;
        const double hy = (max_y - min_y) * 0.5 / state.constellation_zoom;
        min_x = cx - hx;
        max_x = cx + hx;
        min_y = cy - hy;
        max_y = cy + hy;
    }

    std::function<ImVec2(double, double)> to_screen = [&](double x, double y) {
        const float sx = origin.x + static_cast<float>((x - min_x) / (max_x - min_x)) * side;
        const float sy = origin.y + side - static_cast<float>((y - min_y) / (max_y - min_y)) * side;
        return ImVec2(sx, sy);
    };

    // Zoom (wheel, about the cursor) and pan (left-drag) on the plot; takes effect next frame.
    {
        const ImGuiIO &io = ImGui::GetIO();
        if (ImGui::IsItemHovered() && io.MouseWheel != 0.0f) {
            const double new_zoom = std::clamp(
                state.constellation_zoom * std::pow(1.2, static_cast<double>(io.MouseWheel)), 1.0,
                64.0);
            const double applied = new_zoom / state.constellation_zoom;
            // Keep the data point under the cursor fixed while the window shrinks/grows.
            const double mx = min_x + (io.MousePos.x - origin.x) / side * (max_x - min_x);
            const double my = min_y + (origin.y + side - io.MousePos.y) / side * (max_y - min_y);
            state.constellation_pan_x += (mx - (min_x + max_x) * 0.5) * (1.0 - 1.0 / applied);
            state.constellation_pan_y += (my - (min_y + max_y) * 0.5) * (1.0 - 1.0 / applied);
            state.constellation_zoom = new_zoom;
        }
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            state.constellation_pan_x -= io.MouseDelta.x / side * (max_x - min_x);
            state.constellation_pan_y += io.MouseDelta.y / side * (max_y - min_y);
        }
    }
    // Zoomed/panned content can leave the square; clip everything drawn below to it.
    draw->PushClipRect(origin, ImVec2(origin.x + side, origin.y + side), true);

    // Region: 1-sigma solid green + 8 % fill, 2-sigma dashed neutral (§7.2).
    if (have_region) {
        std::vector<ImVec2> one = ellipse_points(region_cx, region_cy, eig, 1.0, to_screen);
        std::vector<ImVec2> two = ellipse_points(region_cx, region_cy, eig, 2.0, to_screen);
        draw->AddConvexPolyFilled(one.data(), static_cast<int>(one.size()),
                                  IM_COL32(99, 153, 34, 20)); // #639922 at 8 %
        draw->AddPolyline(one.data(), static_cast<int>(one.size()), IM_COL32(99, 153, 34, 255),
                          ImDrawFlags_Closed, 2.0f);
        add_dashed_polyline(draw, two, IM_COL32(150, 150, 155, 200), 1.5f);
    }

    // Points (§7.2): color from the sound's own Visual; radius from size_px mapped 4..9 px;
    // band rings in the §7.1 styles; hover tooltip; click selects; double-click plays.
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    int hovered_index = -1;
    for (const Point &p : points) {
        const sp::Visual &v = p.entry->visual;
        const ImVec2 c = to_screen(p.x, p.y);
        const float radius =
            4.0f + 5.0f * static_cast<float>(std::clamp((v.size_px - 14.0) / 50.0, 0.0, 1.0));
        draw->AddCircleFilled(c, radius, hsl_to_rgba(v.hue_deg, v.sat, v.light, 1.0));
        if (state.show_halos && p.dev->band == sp::DevBand::red) {
            draw->AddCircle(c, radius + 3.0f, IM_COL32(226, 75, 74, 235), 0, 2.5f);
        } else if (state.show_halos && p.dev->band == sp::DevBand::amber) {
            for (int seg = 0; seg < 8; seg += 2) { // dashed ring, 4 dashes
                draw->PathArcTo(c, radius + 3.0f, kPi * 0.25f * seg, kPi * 0.25f * (seg + 1), 6);
                draw->PathStroke(IM_COL32(239, 159, 39, 235), 0, 2.0f);
            }
        }
        if (state.selected == p.file_index) {
            draw->AddCircle(c, radius + 6.0f, IM_COL32(255, 255, 255, 120), 0, 1.5f);
        }
        const float dx = mouse.x - c.x, dy = mouse.y - c.y;
        if (dx * dx + dy * dy <= (radius + 3.0f) * (radius + 3.0f)) {
            hovered_index = p.file_index;
        }
    }

    if (hovered_index >= 0 && ImGui::IsItemHovered()) {
        const sp::FileEntry &e = files[static_cast<std::size_t>(hovered_index)];
        const sp::Deviation &dev = state.deviations[static_cast<std::size_t>(hovered_index)];
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(e.path.c_str());
        ImGui::Text("max |z| %.2f (%s), worst %s", dev.max_z, sp::dev_band_name(dev.band),
                    kDimNames[dev.worst_dim]);
        ImGui::EndTooltip();
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            state.selected = hovered_index;
        }
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            playback_start(state, hovered_index);
        }
    }
    // Space plays the selected sound (§7.2).
    if (ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_Space, false) &&
        state.selected >= 0) {
        playback_start(state, state.selected);
    }

    // Selected outlier only: dashed distance line to the nearest point of the 1-sigma
    // ellipse with a z label (§7.2 spaghetti rule).
    if (have_region && state.selected >= 0) {
        const Point *sel = nullptr;
        for (const Point &p : points) {
            if (p.file_index == state.selected) {
                sel = &p;
            }
        }
        if (sel != nullptr && sel->dev->band != sp::DevBand::none) {
            const ImVec2 sc = to_screen(sel->x, sel->y);
            std::vector<ImVec2> one = ellipse_points(region_cx, region_cy, eig, 1.0, to_screen);
            ImVec2 nearest = one[0];
            float best = 1e30f;
            for (const ImVec2 &q : one) {
                const float ddx = q.x - sc.x, ddy = q.y - sc.y;
                const float d2 = ddx * ddx + ddy * ddy;
                if (d2 < best) {
                    best = d2;
                    nearest = q;
                }
            }
            add_dashed_line(draw, sc, nearest, IM_COL32(230, 230, 230, 180), 1.5f);
            char zbuf[16];
            std::snprintf(zbuf, sizeof(zbuf), "z %.1f", sel->dev->max_z);
            draw->AddText(
                ImVec2((sc.x + nearest.x) * 0.5f + 6.0f, (sc.y + nearest.y) * 0.5f - 6.0f),
                IM_COL32(230, 230, 230, 220), zbuf);
        }
    }
    draw->PopClipRect();
}

} // namespace spapp
