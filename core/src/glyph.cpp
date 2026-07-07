#include "soundpalette/glyph.h"
#include "soundpalette/deviation.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace sp {

namespace {

constexpr double kPi = 3.14159265358979323846;

std::string escape_xml(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        default:
            out += c;
        }
    }
    return out;
}

std::string fmt(double x) {
    std::ostringstream ss;
    ss.setf(std::ios::fixed);
    ss.precision(3);
    ss << x;
    return ss.str();
}

// Integer components: resvg (the MCP sheet_png rasterizer) renders hsl() with decimal
// components as black; browsers are lenient. Rounding is visually lossless.
std::string hsl_ints(double hue_deg, double sat, double light) {
    std::ostringstream ss;
    ss << "hsl(" << static_cast<long>(std::lround(hue_deg)) << ","
       << static_cast<long>(std::lround(sat)) << "%," << static_cast<long>(std::lround(light))
       << "%)";
    return ss.str();
}

std::string hsl_color(const Visual &v) {
    return hsl_ints(v.hue_deg, v.sat, v.light);
}

// The glyph polygon + tonality rays + decay-tail circles, positioned with center (cx, cy).
// No outer <svg> tag; shared by glyph_svg() (wrapped standalone) and sheet_svg() (positioned
// within the grid).
std::string glyph_fragment(const Visual &v, double cx, double cy) {
    std::ostringstream ss;
    std::vector<std::array<float, 2>> pts = glyph_outline(v);

    ss << "<polygon points=\"";
    for (std::size_t i = 0; i < pts.size(); ++i) {
        if (i > 0) {
            ss << " ";
        }
        ss << fmt(cx + pts[i][0]) << "," << fmt(cy + pts[i][1]);
    }
    ss << "\" fill=\"" << hsl_color(v) << "\" />\n";

    // Tonality rays: straight fan = tonal, wobbling fan = noise-like.
    for (const std::vector<std::array<float, 2>> &ray : glyph_rays(v)) {
        ss << "<polyline points=\"";
        for (std::size_t i = 0; i < ray.size(); ++i) {
            if (i > 0) {
                ss << " ";
            }
            ss << fmt(cx + ray[i][0]) << "," << fmt(cy + ray[i][1]);
        }
        ss << "\" fill=\"none\" stroke=\"" << hsl_color(v)
           << "\" stroke-width=\"1.6\" stroke-linecap=\"round\" />\n";
    }

    // Decay tail (trail revision): five fading half moons on EACH side, concave side
    // facing the blob, starting at its edge. Geometry shared with the GUI via glyph_tail
    // (empty when tail01 < 0.05).
    for (const TailMoon &moon : glyph_tail(v)) {
        ss << "<polygon points=\"";
        for (std::size_t i = 0; i < moon.pts.size(); ++i) {
            if (i > 0) {
                ss << " ";
            }
            ss << fmt(cx + moon.pts[i][0]) << "," << fmt(cy + moon.pts[i][1]);
        }
        ss << "\" fill=\"" << hsl_color(v) << "\" fill-opacity=\"" << fmt(moon.opacity)
           << "\" />\n";
    }

    return ss.str();
}

// The psychoacoustic line (split-glyph lower half): stroke width = loudness (linear in
// sones, so the line's area stays honest), color blue->red = sharpness, sine amplitude =
// roughness, sine frequency = fluctuation. Both wave parameters have enforced visible
// minimums so neither hides the other. Skipped for silent files.
std::string psycho_line_fragment(const Visual &v, double x0, double x1, double y) {
    const MappingConfig &c = active_mapping_config();
    const double sones = (v.loud01 * c.loud_sone_div) * (v.loud01 * c.loud_sone_div);
    const double width = std::clamp(c.line_width_min_px + c.line_width_per_sone_px * sones,
                                    c.line_width_min_px, c.line_width_max_px);
    const double amp = c.line_amp_min_px + v.jitter01 * (c.line_amp_max_px - c.line_amp_min_px);
    const double cycles = c.line_cycles_min + v.fluct01 * (c.line_cycles_max - c.line_cycles_min);
    const double hue = c.sharp_hue_lo_deg + v.sharp01 * (c.sharp_hue_hi_deg - c.sharp_hue_lo_deg);

    std::ostringstream ss;
    ss << "<polyline points=\"";
    const int n = 48;
    for (int i = 0; i <= n; ++i) {
        const double t = static_cast<double>(i) / n;
        const double x = x0 + t * (x1 - x0);
        const double yy = y - amp * std::sin(2.0 * kPi * cycles * t);
        if (i > 0) {
            ss << " ";
        }
        ss << fmt(x) << "," << fmt(yy);
    }
    ss << "\" fill=\"none\" stroke=\"" << hsl_ints(hue, 85.0, 55.0) << "\" stroke-width=\""
       << fmt(width) << "\" stroke-linecap=\"round\" stroke-linejoin=\"round\" />\n";
    return ss.str();
}

std::string error_mark_fragment(double cx, double cy, double size) {
    std::ostringstream ss;
    double half = size * 0.35;
    ss << "<line x1=\"" << fmt(cx - half) << "\" y1=\"" << fmt(cy - half) << "\" x2=\""
       << fmt(cx + half) << "\" y2=\"" << fmt(cy + half)
       << "\" stroke=\"#888888\" stroke-width=\"3\" />\n";
    ss << "<line x1=\"" << fmt(cx + half) << "\" y1=\"" << fmt(cy - half) << "\" x2=\""
       << fmt(cx - half) << "\" y2=\"" << fmt(cy + half)
       << "\" stroke=\"#888888\" stroke-width=\"3\" />\n";
    return ss.str();
}

} // namespace

std::vector<std::array<float, 2>> glyph_outline(const Visual &v, int base_points) {
    // Fixed five-point star: two points up (-115 deg, -65 deg) and three down (40, 90,
    // 140 deg; y grows downward), so the horizontal axis stays clear for the decay trail
    // and the three tonality rays interleave with the upper points. Each point is a
    // cosine^1.5 lobe of half-width 20 deg; the radius between lobes dips deeply so the
    // extreme end is a true star, not spikes on a round blob — at spike01 = 1 the
    // point-to-valley ratio is 1.50 : 0.45 (~3.3 : 1), while slow attacks stay perfectly
    // round. All point angles are multiples of 2.5 deg, so sampling snaps to a multiple
    // of 144 and every tip lands exactly on a sample.
    constexpr double kStarAnglesDeg[5] = {-115.0, -65.0, 40.0, 90.0, 140.0};
    constexpr double kLobeHalfWidthDeg = 20.0;
    int n = base_points;
    if (v.spikes > 0) {
        n = ((base_points + 143) / 144) * 144;
    }
    std::vector<std::array<float, 2>> pts(static_cast<std::size_t>(n));

    for (int i = 0; i < n; ++i) {
        double theta = i * 2.0 * kPi / n;

        double shape = 0.0;
        if (v.spikes > 0) {
            const double theta_deg = theta * 180.0 / kPi;
            double dist_deg = 180.0;
            for (double p : kStarAnglesDeg) {
                dist_deg = std::min(dist_deg, std::fabs(std::remainder(theta_deg - p, 360.0)));
            }
            if (dist_deg < kLobeHalfWidthDeg) {
                shape = std::pow(std::cos(0.5 * kPi * dist_deg / kLobeHalfWidthDeg), 1.5);
            }
        }
        double r = v.size_px * (1.0 + v.spike01 * (0.50 * shape - 0.55 * (1.0 - shape)));

        pts[static_cast<std::size_t>(i)] = {static_cast<float>(r * std::cos(theta)),
                                            static_cast<float>(r * std::sin(theta))};
    }

    return pts;
}

std::vector<std::vector<std::array<float, 2>>> glyph_rays(const Visual &v) {
    if (v.silent) {
        return {};
    }
    // Three rays fanned +-50 deg around straight up (-140, -90, -40 deg), starting just
    // off the blob edge; they interleave with the star's upper points at -115 / -65 deg.
    // The waviness is a perpendicular sine whose amplitude scales with noisiness
    // (1 - ton01): tonal material shows a clean straight fan, noise shows wobbling rays.
    constexpr int kRays = 3;
    constexpr int kSegments = 12;
    constexpr double kFanHalfDeg = 50.0;
    const double start_r = 1.12 * v.size_px;
    const double length = 0.75 * v.size_px;
    const double wave_amp = (1.0 - std::clamp(v.ton01, 0.0, 1.0)) * 0.18 * v.size_px;
    constexpr double kWaveCycles = 2.2;

    std::vector<std::vector<std::array<float, 2>>> rays(kRays);
    for (int r = 0; r < kRays; ++r) {
        const double angle_deg = -90.0 - kFanHalfDeg + (2.0 * kFanHalfDeg * r) / (kRays - 1);
        const double a = angle_deg * kPi / 180.0;
        const double dir_x = std::cos(a), dir_y = std::sin(a);
        const double perp_x = -dir_y, perp_y = dir_x;
        std::vector<std::array<float, 2>> &pts = rays[static_cast<std::size_t>(r)];
        pts.resize(kSegments + 1);
        for (int i = 0; i <= kSegments; ++i) {
            const double t = static_cast<double>(i) / kSegments;
            const double d = start_r + t * length;
            // Alternate the wave phase per ray so the fan wobbles, not marches, in step.
            const double w =
                wave_amp * std::sin(2.0 * kPi * kWaveCycles * t) * (r % 2 ? -1.0 : 1.0);
            pts[static_cast<std::size_t>(i)] = {static_cast<float>(dir_x * d + perp_x * w),
                                                static_cast<float>(dir_y * d + perp_y * w)};
        }
    }
    return rays;
}

std::vector<TailMoon> glyph_tail(const Visual &v) {
    if (v.tail01 < 0.05) {
        return {};
    }
    // Five crescents per side on the horizontal axis, starting at the blob edge: sizes
    // shrink from 0.30*size_px, opacity fades 0.55 -> 0.10, spread tail01*1.15*size_px
    // per side. Each crescent is an outer semicircle facing away from the blob plus a
    // shallower inner arc bulging the same way (0.45 r at the equator), so the face
    // toward the blob is concave — a half moon waning toward the glyph.
    constexpr int kMoonsPerSide = 5;
    constexpr int kArcSegments = 16;
    const double spread = v.tail01 * 1.15 * v.size_px;

    std::vector<TailMoon> moons;
    moons.reserve(kMoonsPerSide * 2);
    for (int i = 0; i < kMoonsPerSide; ++i) {
        const double t = static_cast<double>(i) / (kMoonsPerSide - 1); // 0..1
        const double dx = v.size_px + ((i + 1) / static_cast<double>(kMoonsPerSide)) * spread;
        const double r = 0.30 * v.size_px * (1.0 - 0.75 * t);
        const float opacity = static_cast<float>(0.55 + (0.10 - 0.55) * t);
        // The inner arc's circle passes through both tips (0, +-r) and bulges b past the
        // tip line: center e = (r^2 - b^2) / 2b behind the tips, radius e + b.
        const double b = 0.45 * r;
        const double e = (r * r - b * b) / (2.0 * b);
        const double big_r = e + b;
        const double alpha = std::atan2(r, e);
        for (double side : {-1.0, 1.0}) {
            TailMoon moon;
            moon.opacity = opacity;
            moon.pts.reserve(2 * kArcSegments);
            // Outer semicircle: top tip -> away-facing equator -> bottom tip.
            for (int k = 0; k <= kArcSegments; ++k) {
                const double a = -0.5 * kPi + k * kPi / kArcSegments;
                moon.pts.push_back({static_cast<float>(side * (dx + r * std::cos(a))),
                                    static_cast<float>(r * std::sin(a))});
            }
            // Inner arc back up; the tips are already on the outer arc, so both endpoints
            // are skipped.
            for (int k = 1; k < kArcSegments; ++k) {
                const double a = alpha - k * 2.0 * alpha / kArcSegments;
                moon.pts.push_back({static_cast<float>(side * (dx - e + big_r * std::cos(a))),
                                    static_cast<float>(big_r * std::sin(a))});
            }
            moons.push_back(std::move(moon));
        }
    }
    return moons;
}

std::string glyph_svg(const Visual &v, double cell_px) {
    std::ostringstream ss;
    ss << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    ss << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << fmt(cell_px) << "\" height=\""
       << fmt(cell_px) << "\" viewBox=\"0 0 " << fmt(cell_px) << " " << fmt(cell_px) << "\">\n";
    ss << glyph_fragment(v, cell_px / 2.0, cell_px * 0.35);
    if (!v.silent) {
        ss << psycho_line_fragment(v, cell_px * 0.08, cell_px * 0.92, cell_px * 0.74);
    }
    ss << "</svg>\n";
    return ss.str();
}

namespace {

// Halo fragment (§6.1): ring behind the glyph plus the z label under the filename slot.
std::string halo_fragment(const Deviation &dev, const Visual &v, double cx, double cy,
                          double cell_x, double cell_y, double cell_px) {
    std::ostringstream ss;
    // The blob is smaller since the ray revision but its rays/tail reach further, so the
    // ring is sized to the full glyph envelope rather than the blob radius.
    const double radius = std::min(cell_px * 0.46, v.size_px * 2.5 + 6.0);
    if (dev.band == DevBand::red) {
        ss << "<circle cx=\"" << fmt(cx) << "\" cy=\"" << fmt(cy) << "\" r=\"" << fmt(radius)
           << "\" fill=\"none\" stroke=\"#E24B4A\" stroke-width=\"3\" data-dev=\"red\" />\n";
    } else {
        ss << "<circle cx=\"" << fmt(cx) << "\" cy=\"" << fmt(cy) << "\" r=\"" << fmt(radius)
           << "\" fill=\"none\" stroke=\"#EF9F27\" stroke-width=\"2\""
           << " stroke-dasharray=\"4,3\" data-dev=\"amber\" />\n";
    }
    ss << "<text x=\"" << fmt(cell_x + cell_px / 2.0) << "\" y=\"" << fmt(cell_y + cell_px - 14.0)
       << "\" font-size=\"9\" font-family=\"sans-serif\" fill=\""
       << (dev.band == DevBand::red ? "#E24B4A" : "#EF9F27") << "\" text-anchor=\"middle\">z "
       << fmt(dev.max_z) << "</text>\n";
    return ss.str();
}

} // namespace

namespace {

// §7 legend strip: the sheet explains its own glyph grammar in perceptual units.
constexpr double kLegendHeightPx = 18.0;
std::string legend_fragment(double width, double y, double ref_spl) {
    std::ostringstream ss;
    ss << "<rect x=\"0\" y=\"" << fmt(y) << "\" width=\"" << fmt(width) << "\" height=\""
       << fmt(kLegendHeightPx) << "\" fill=\"#161616\" />\n";
    ss << "<text x=\"6\" y=\"" << fmt(y + 12.5)
       << "\" font-family=\"monospace\" font-size=\"9\" fill=\"#aaaaaa\">"
       << "blob: hue = warmth &#183; rays = tonality (straight = tonal, wavy = noisy) &#183; "
          "star spikes = attack &#183; "
          "trail = decay &#124; line: width = loudness (sones) &#183; color blue&#8594;red = "
          "sharpness (acum) &#183; wave height = roughness (asper) &#183; wave count = "
          "fluctuation (vacil) &#183; ref_spl "
       << fmt(ref_spl) << " dB SPL</text>\n";
    return ss.str();
}

} // namespace

std::string sheet_svg(const Manifest &manifest, int columns, const Profile &profile,
                      bool with_legend) {
    constexpr double kCellPx = 120.0;
    constexpr double kLabelHeightPx = 11.0;
    constexpr double kGlyphCenterYFraction = 0.35;

    columns = std::max(columns, 1);
    const int rows =
        static_cast<int>((manifest.files.size() + static_cast<std::size_t>(columns) - 1) /
                         static_cast<std::size_t>(columns));
    const double width = columns * kCellPx;
    const double grid_height = std::max(rows, 1) * kCellPx;
    const double height = grid_height + (with_legend ? kLegendHeightPx : 0.0);

    std::ostringstream ss;
    ss << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    ss << "<!-- soundpalette schema_version=" << manifest.schema_version
       << " mapping_version=" << manifest.mapping_version << " profile=\""
       << escape_xml(profile.name) << "\" -->\n";
    ss << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << fmt(width) << "\" height=\""
       << fmt(height) << "\" viewBox=\"0 0 " << fmt(width) << " " << fmt(height) << "\">\n";
    ss << "<rect x=\"0\" y=\"0\" width=\"" << fmt(width) << "\" height=\"" << fmt(height)
       << "\" fill=\"#1e1e1e\" />\n";

    for (std::size_t i = 0; i < manifest.files.size(); ++i) {
        const FileEntry &fe = manifest.files[i];
        int col = static_cast<int>(i) % columns;
        int row = static_cast<int>(i) / columns;
        double cell_x = col * kCellPx;
        double cell_y = row * kCellPx;
        double cx = cell_x + kCellPx / 2.0;
        double cy = cell_y + kCellPx * kGlyphCenterYFraction;

        ss << "<g>\n";
        if (!fe.error.empty()) {
            ss << error_mark_fragment(cx, cy, kCellPx * 0.5);
        } else {
            const Deviation dev = compute_deviation(fe, profile);
            if (dev.band != DevBand::none) {
                ss << halo_fragment(dev, fe.visual, cx, cy, cell_x, cell_y, kCellPx);
            }
            ss << glyph_fragment(fe.visual, cx, cy);
            if (!fe.visual.silent) {
                ss << psycho_line_fragment(fe.visual, cell_x + kCellPx * 0.08,
                                           cell_x + kCellPx * 0.92, cell_y + kCellPx * 0.72);
            }
        }
        ss << "<text x=\"" << fmt(cell_x + kCellPx / 2.0) << "\" y=\""
           << fmt(cell_y + kCellPx - kLabelHeightPx * 0.3) << "\" font-size=\""
           << fmt(kLabelHeightPx) << "\" font-family=\"sans-serif\" fill=\"#cccccc\""
           << " text-anchor=\"middle\">" << escape_xml(fe.path) << "</text>\n";
        ss << "</g>\n";
    }

    if (with_legend) {
        ss << legend_fragment(width, grid_height, manifest.ref_spl);
    }
    ss << "</svg>\n";
    return ss.str();
}

std::string sheet_svg(const Manifest &manifest, int columns, bool with_legend) {
    constexpr double kCellPx = 120.0;
    constexpr double kLabelHeightPx = 11.0;
    constexpr double kGlyphCenterYFraction = 0.35; // leaves room for the label beneath

    columns = std::max(columns, 1);
    const int rows =
        static_cast<int>((manifest.files.size() + static_cast<std::size_t>(columns) - 1) /
                         static_cast<std::size_t>(columns));
    const double width = columns * kCellPx;
    const double grid_height = std::max(rows, 1) * kCellPx;
    const double height = grid_height + (with_legend ? kLegendHeightPx : 0.0);

    std::ostringstream ss;
    ss << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    ss << "<!-- soundpalette schema_version=" << manifest.schema_version
       << " mapping_version=" << manifest.mapping_version << " ref_spl=" << fmt(manifest.ref_spl)
       << " -->\n";
    ss << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << fmt(width) << "\" height=\""
       << fmt(height) << "\" viewBox=\"0 0 " << fmt(width) << " " << fmt(height) << "\">\n";
    ss << "<rect x=\"0\" y=\"0\" width=\"" << fmt(width) << "\" height=\"" << fmt(height)
       << "\" fill=\"#1e1e1e\" />\n";

    for (std::size_t i = 0; i < manifest.files.size(); ++i) {
        const FileEntry &fe = manifest.files[i];
        int col = static_cast<int>(i) % columns;
        int row = static_cast<int>(i) / columns;
        double cell_x = col * kCellPx;
        double cell_y = row * kCellPx;
        double cx = cell_x + kCellPx / 2.0;
        double cy = cell_y + kCellPx * kGlyphCenterYFraction;

        ss << "<g>\n";
        if (!fe.error.empty()) {
            ss << error_mark_fragment(cx, cy, kCellPx * 0.5);
        } else {
            ss << glyph_fragment(fe.visual, cx, cy);
            if (!fe.visual.silent) {
                ss << psycho_line_fragment(fe.visual, cell_x + kCellPx * 0.08,
                                           cell_x + kCellPx * 0.92, cell_y + kCellPx * 0.72);
            }
        }
        ss << "<text x=\"" << fmt(cell_x + kCellPx / 2.0) << "\" y=\""
           << fmt(cell_y + kCellPx - kLabelHeightPx * 0.3) << "\" font-size=\""
           << fmt(kLabelHeightPx) << "\" font-family=\"sans-serif\" fill=\"#cccccc\""
           << " text-anchor=\"middle\">" << escape_xml(fe.path) << "</text>\n";
        ss << "</g>\n";
    }

    if (with_legend) {
        ss << legend_fragment(width, grid_height, manifest.ref_spl);
    }
    ss << "</svg>\n";
    return ss.str();
}

} // namespace sp
