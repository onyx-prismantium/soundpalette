#include "soundpalette/glyph.h"
#include "soundpalette/deviation.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace sp {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kTailCircleCount = 5;

std::uint64_t xorshift64star(std::uint64_t &state) {
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    return state * 0x2545F4914F6CDD1DULL;
}

// Uniform in [0, 1).
double next_uniform01(std::uint64_t &state) {
    std::uint64_t bits = xorshift64star(state) >> 11;
    return static_cast<double>(bits) * (1.0 / 9007199254740992.0);
}

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

std::string hsl_color(const Visual &v) {
    std::ostringstream ss;
    ss << "hsl(" << fmt(v.hue_deg) << "," << fmt(v.sat) << "%," << fmt(v.light) << "%)";
    return ss.str();
}

// The glyph polygon + decay-tail circles, positioned with center (cx, cy). No outer <svg> tag;
// shared by glyph_svg() (wrapped standalone) and sheet_svg() (positioned within the grid).
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

    // Decay tail: 5 circles to the right, radius shrinking from 0.16*size_px, opacity fading
    // from 0.5 to 0.07, horizontal spread tail01*2.2*size_px (§7). Omitted when tail01 < 0.05.
    if (v.tail01 >= 0.05) {
        const double spread = v.tail01 * 2.2 * v.size_px;
        for (int i = 0; i < kTailCircleCount; ++i) {
            double t = static_cast<double>(i) / (kTailCircleCount - 1); // 0..1
            double x = cx + ((i + 1) / static_cast<double>(kTailCircleCount)) * spread;
            double radius = 0.16 * v.size_px * (1.0 - 0.8 * t);
            double opacity = 0.5 + (0.07 - 0.5) * t;
            ss << "<circle cx=\"" << fmt(x) << "\" cy=\"" << fmt(cy) << "\" r=\"" << fmt(radius)
               << "\" fill=\"" << hsl_color(v) << "\" fill-opacity=\"" << fmt(opacity) << "\" />\n";
        }
    }

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
    std::vector<std::array<float, 2>> pts(static_cast<std::size_t>(base_points));

    std::uint64_t state = v.seed != 0 ? v.seed : 0x9E3779B97F4A7C15ULL;

    for (int i = 0; i < base_points; ++i) {
        double theta = i * 2.0 * kPi / base_points;

        double spike_term = 0.0;
        if (v.spikes > 0) {
            spike_term = v.spike01 * 0.45 * std::max(0.0, std::cos(v.spikes * theta));
        }

        double rand_i = next_uniform01(state);
        double jitter_term = v.jitter01 * 0.30 * (rand_i - 0.5);

        // Mapping v2 (extension-3 §6): slow three-lobe wave for fluctuation strength —
        // visually distinct from jitter's fine random raggedness. Phase from the seed so the
        // same file always waves the same way.
        double fluct_term =
            v.fluct01 * 0.18 *
            std::sin(3.0 * theta + 2.0 * kPi * (static_cast<double>(v.seed % 360) / 360.0));

        double r = v.size_px * (1.0 + spike_term + jitter_term + fluct_term);

        pts[static_cast<std::size_t>(i)] = {static_cast<float>(r * std::cos(theta)),
                                            static_cast<float>(r * std::sin(theta))};
    }

    return pts;
}

std::string glyph_svg(const Visual &v, double cell_px) {
    std::ostringstream ss;
    ss << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    ss << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << fmt(cell_px) << "\" height=\""
       << fmt(cell_px) << "\" viewBox=\"0 0 " << fmt(cell_px) << " " << fmt(cell_px) << "\">\n";
    ss << glyph_fragment(v, cell_px / 2.0, cell_px / 2.0);
    ss << "</svg>\n";
    return ss.str();
}

namespace {

// Halo fragment (§6.1): ring behind the glyph plus the z label under the filename slot.
std::string halo_fragment(const Deviation &dev, const Visual &v, double cx, double cy,
                          double cell_x, double cell_y, double cell_px) {
    std::ostringstream ss;
    const double radius = std::min(cell_px * 0.46, v.size_px * 1.5 + 6.0);
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
       << "area = loudness (sones) &#183; lightness = sharpness (acum) &#183; ragged edge = "
          "roughness (asper) &#183; slow waves = fluctuation (vacil) &#183; ref_spl "
       << fmt(ref_spl) << " dB SPL</text>\n";
    return ss.str();
}

} // namespace

std::string sheet_svg(const Manifest &manifest, int columns, const Profile &profile,
                      bool with_legend) {
    constexpr double kCellPx = 120.0;
    constexpr double kLabelHeightPx = 11.0;
    constexpr double kGlyphCenterYFraction = 0.42;

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
    constexpr double kGlyphCenterYFraction = 0.42; // leaves room for the label beneath

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
