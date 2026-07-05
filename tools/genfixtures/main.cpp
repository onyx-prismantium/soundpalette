// Deterministic test-audio fixture generator (PLAN.md §11). All randomness comes from a
// xorshift64* PRNG seeded with 0xDEADBEEFCAFEF00D (+ file index for indexed sets), so output
// is byte-identical across runs and machines.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "miniaudio.h"

namespace {

constexpr std::uint64_t kBaseSeed = 0xDEADBEEFCAFEF00DULL;
constexpr double kPi = 3.14159265358979323846;
constexpr ma_uint32 kSampleRate = 48000;

std::uint64_t xorshift64star(std::uint64_t &state) {
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    return state * 0x2545F4914F6CDD1DULL;
}

// Uniform in [-1, 1).
double next_uniform(std::uint64_t &state) {
    std::uint64_t bits = xorshift64star(state) >> 11;                    // top 53 bits
    double u01 = static_cast<double>(bits) * (1.0 / 9007199254740992.0); // / 2^53
    return 2.0 * u01 - 1.0;
}

std::vector<float> white_noise(std::uint64_t seed, std::size_t n, double amplitude) {
    std::vector<float> out(n);
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = static_cast<float>(amplitude * next_uniform(seed));
    }
    return out;
}

// One-pole low-pass: y[n] = a*y[n-1] + (1-a)*x[n], a = exp(-2*pi*fc/fs).
void onepole_lowpass_inplace(std::vector<float> &buf, double fc) {
    const double a = std::exp(-2.0 * kPi * fc / kSampleRate);
    float y_prev = 0.0f;
    for (float &x : buf) {
        y_prev = static_cast<float>(a * y_prev + (1.0 - a) * x);
        x = y_prev;
    }
}

// One-pole high-pass (leaky differentiator): y[n] = a*(y[n-1] + x[n] - x[n-1]), a =
// exp(-2*pi*fc/fs).
void onepole_highpass_inplace(std::vector<float> &buf, double fc) {
    const double a = std::exp(-2.0 * kPi * fc / kSampleRate);
    float y_prev = 0.0f;
    float x_prev = 0.0f;
    for (float &x : buf) {
        float x_cur = x;
        y_prev = static_cast<float>(a * (y_prev + x_cur - x_prev));
        x_prev = x_cur;
        x = y_prev;
    }
}

void apply_exp_decay_inplace(std::vector<float> &buf, double tau_s) {
    for (std::size_t i = 0; i < buf.size(); ++i) {
        double t = static_cast<double>(i) / kSampleRate;
        buf[i] = static_cast<float>(buf[i] * std::exp(-t / tau_s));
    }
}

void peak_normalize_inplace(std::vector<float> &buf, double target_peak) {
    float peak = 0.0f;
    for (float x : buf) {
        peak = std::max(peak, std::fabs(x));
    }
    if (peak <= 0.0f) {
        return;
    }
    const float scale = static_cast<float>(target_peak) / peak;
    for (float &x : buf) {
        x *= scale;
    }
}

std::vector<float> sine(double freq_hz, double amplitude, std::size_t n) {
    std::vector<float> out(n);
    for (std::size_t i = 0; i < n; ++i) {
        double t = static_cast<double>(i) / kSampleRate;
        out[i] = static_cast<float>(amplitude * std::sin(2.0 * kPi * freq_hz * t));
    }
    return out;
}

bool write_wav_mono_f32(const std::filesystem::path &path, const std::vector<float> &samples) {
    ma_encoder_config cfg =
        ma_encoder_config_init(ma_encoding_format_wav, ma_format_f32, 1, kSampleRate);
    ma_encoder encoder;
    if (ma_encoder_init_file(path.string().c_str(), &cfg, &encoder) != MA_SUCCESS) {
        std::fprintf(stderr, "genfixtures: failed to open %s for writing\n", path.string().c_str());
        return false;
    }
    ma_uint64 framesWritten = 0;
    ma_result result =
        ma_encoder_write_pcm_frames(&encoder, samples.data(), samples.size(), &framesWritten);
    ma_encoder_uninit(&encoder);
    if (result != MA_SUCCESS || framesWritten != samples.size()) {
        std::fprintf(stderr, "genfixtures: failed to write %s\n", path.string().c_str());
        return false;
    }
    return true;
}

std::size_t seconds_to_frames(double seconds) {
    return static_cast<std::size_t>(std::llround(seconds * kSampleRate));
}

// Exact-RMS calibration for the psycho fixtures (extension-3 §8): target dB SPL S under the
// §4 convention (ref_spl 75) means RMS dBFS = S - 98.
void rms_normalize_inplace(std::vector<float> &buf, double target_rms_dbfs) {
    double sumsq = 0.0;
    for (float x : buf) {
        sumsq += static_cast<double>(x) * static_cast<double>(x);
    }
    const double rms = std::sqrt(sumsq / static_cast<double>(buf.size()));
    if (rms <= 0.0) {
        return;
    }
    const double target = std::pow(10.0, target_rms_dbfs / 20.0);
    const float scale = static_cast<float>(target / rms);
    for (float &x : buf) {
        x *= scale;
    }
}

// RBJ constant-0dB-peak bandpass biquad, direct form 1; two cascaded = the §8 4th-order.
void biquad_bandpass_inplace(std::vector<float> &buf, double f0, double q) {
    const double w0 = 2.0 * kPi * f0 / kSampleRate;
    const double alpha = std::sin(w0) / (2.0 * q);
    const double b0 = alpha, b1 = 0.0, b2 = -alpha;
    const double a0 = 1.0 + alpha, a1 = -2.0 * std::cos(w0), a2 = 1.0 - alpha;
    double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;
    for (float &x : buf) {
        const double x0 = x;
        const double y0 = (b0 * x0 + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2) / a0;
        x2 = x1;
        x1 = x0;
        y2 = y1;
        y1 = y0;
        x = static_cast<float>(y0);
    }
}

// Calibrated psychoacoustic fixtures (extension-3 §8): definitional reference signals for
// 1 sone / 1 acum / 1 asper / 1 vacil plus level steps, at exact RMS under ref_spl = 75.
bool generate_psycho(const std::filesystem::path &outdir) {
    bool ok = true;

    // tone1k_{40,50,60}db.wav: 1 kHz sine, RMS -58/-48/-38 dBFS, 2 s.
    const double levels[3][2] = {{40.0, -58.0}, {50.0, -48.0}, {60.0, -38.0}};
    for (const auto &lv : levels) {
        std::vector<float> buf = sine(1000.0, 1.0, seconds_to_frames(2.0));
        rms_normalize_inplace(buf, lv[1]);
        char name[32];
        std::snprintf(name, sizeof(name), "tone1k_%.0fdb.wav", lv[0]);
        ok &= write_wav_mono_f32(outdir / name, buf);
    }

    // am70_60db.wav / am4_60db.wav: 1 kHz carrier, 100 % AM (m = 1), RMS -38 dBFS.
    const double am_specs[2][2] = {{70.0, 2.0}, {4.0, 4.0}}; // {mod Hz, seconds}
    for (const auto &am : am_specs) {
        const std::size_t n = seconds_to_frames(am[1]);
        std::vector<float> buf(n);
        for (std::size_t i = 0; i < n; ++i) {
            const double t = static_cast<double>(i) / kSampleRate;
            buf[i] = static_cast<float>((1.0 + std::cos(2.0 * kPi * am[0] * t)) *
                                        std::sin(2.0 * kPi * 1000.0 * t));
        }
        rms_normalize_inplace(buf, -38.0);
        char name[32];
        std::snprintf(name, sizeof(name), "am%.0f_60db.wav", am[0]);
        ok &= write_wav_mono_f32(outdir / name, buf);
    }

    // nbnoise1k_60db.wav: white noise band-passed 920-1080 Hz (one critical band at 1 kHz;
    // f0 = 1 kHz, Q = 1000/160, two biquads = 4th order), RMS -38 dBFS, 2 s.
    {
        std::vector<float> buf = white_noise(kBaseSeed + 300, seconds_to_frames(2.0), 1.0);
        biquad_bandpass_inplace(buf, 1000.0, 1000.0 / 160.0);
        biquad_bandpass_inplace(buf, 1000.0, 1000.0 / 160.0);
        rms_normalize_inplace(buf, -38.0);
        ok &= write_wav_mono_f32(outdir / "nbnoise1k_60db.wav", buf);
    }

    return ok;
}

bool generate_dark_file(const std::filesystem::path &outdir, int i) {
    const std::size_t n = seconds_to_frames(1.5);
    std::vector<float> buf = white_noise(kBaseSeed + static_cast<std::uint64_t>(i), n, 1.0);
    onepole_lowpass_inplace(buf, 300.0);
    onepole_lowpass_inplace(buf, 300.0);
    apply_exp_decay_inplace(buf, 0.15 + 0.02 * i);
    peak_normalize_inplace(buf, 0.4);
    char name[32];
    std::snprintf(name, sizeof(name), "dark_%02d.wav", i);
    return write_wav_mono_f32(outdir / name, buf);
}

bool generate_perf_file(const std::filesystem::path &outdir, int i) {
    const std::size_t n = seconds_to_frames(1.0);
    std::vector<float> buf = white_noise(kBaseSeed + static_cast<std::uint64_t>(i), n, 0.25);
    char name[32];
    std::snprintf(name, sizeof(name), "p_%03d.wav", i);
    return write_wav_mono_f32(outdir / name, buf);
}

} // namespace

// fixable_outlier.wav (extension §7): offends tier-one-fixable dims vs the fixture baseline.
bool generate_fixable_outlier(const std::filesystem::path &outdir) {
    // Mapping v2 revision (extension-3; see NOTES.md): bright01 is now DIN 45692 sharpness,
    // so the offense must live in the sharpness domain while staying spectrally compact
    // enough that ton01 (flatness, a v1 dim) stays in-palette — broadband constructions leak
    // an unresolvable ton01 offense — and wide enough to straddle the 4 kHz shelf corner (a
    // shelf only moves sharpness when it can TILT the occupied spectrum). A wide band of noise
    // around 4 kHz is sharp (acum well above the fixture family), tonal by flatness, and a -12
    // dB-clamped high shelf can pull the sharpness back in: genuinely off-palette yet inside the
    // tier-one solver's reach.
    std::vector<float> buf = white_noise(kBaseSeed, seconds_to_frames(3.0), 1.0);
    biquad_bandpass_inplace(buf, 4000.0, 0.8);
    biquad_bandpass_inplace(buf, 4000.0, 0.8);
    apply_exp_decay_inplace(buf, 0.3);
    peak_normalize_inplace(buf, 0.8);
    return write_wav_mono_f32(outdir / "fixable_outlier.wav", buf);
}

// catfx profile-set fixtures (extension-2 §8): two clearly separated categories.
bool generate_catfx(const std::filesystem::path &outdir) {
    bool ok = true;
    std::error_code ec;
    std::filesystem::create_directories(outdir / "catfx" / "ui", ec);
    std::filesystem::create_directories(outdir / "catfx" / "combat", ec);
    for (int i = 0; i < 8; ++i) {
        // ui_00..07: bright, instant, dry ticks.
        std::vector<float> buf = white_noise(kBaseSeed + 100 + static_cast<std::uint64_t>(i),
                                             seconds_to_frames(0.12), 1.0);
        onepole_highpass_inplace(buf, 2500.0);
        onepole_highpass_inplace(buf, 2500.0);
        apply_exp_decay_inplace(buf, (10.0 + i) / 1000.0);
        peak_normalize_inplace(buf, 0.5);
        char name[32];
        std::snprintf(name, sizeof(name), "ui_%02d.wav", i);
        ok &= write_wav_mono_f32(outdir / "catfx" / "ui" / name, buf);
    }
    for (int i = 0; i < 8; ++i) {
        // hit_00..07: dark, thumpy impacts.
        std::vector<float> buf = white_noise(kBaseSeed + 200 + static_cast<std::uint64_t>(i),
                                             seconds_to_frames(0.9), 1.0);
        onepole_lowpass_inplace(buf, 250.0);
        onepole_lowpass_inplace(buf, 250.0);
        apply_exp_decay_inplace(buf, (120.0 + 10.0 * i) / 1000.0);
        peak_normalize_inplace(buf, 0.6);
        char name[32];
        std::snprintf(name, sizeof(name), "hit_%02d.wav", i);
        ok &= write_wav_mono_f32(outdir / "catfx" / "combat" / name, buf);
    }
    return ok;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(
            stderr, "usage: genfixtures <outdir> [--perf200] [--extra <dir>] [--profile-set <dir>] "
                    "[--psycho <dir>]\n");
        return 2;
    }
    std::filesystem::path outdir = argv[1];
    bool perf200 = false;
    std::string extra_dir;
    std::string profile_set_dir;
    std::string psycho_dir;
    for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) == "--extra") {
            extra_dir = i + 1 < argc ? argv[++i] : "fixtures_m9";
        } else if (std::string(argv[i]) == "--profile-set") {
            profile_set_dir = i + 1 < argc ? argv[++i] : "fixtures_m10";
        } else if (std::string(argv[i]) == "--psycho") {
            psycho_dir = i + 1 < argc ? argv[++i] : "fixtures_psycho";
        } else if (std::string(argv[i]) == "--perf200") {
            perf200 = true;
        }
    }

    std::error_code ec;
    std::filesystem::create_directories(outdir, ec);
    std::filesystem::create_directories(outdir / "darkset", ec);

    bool ok = true;

    // sine440_1s.wav: 440 Hz sine, amplitude 0.5, 1.000 s
    ok &= write_wav_mono_f32(outdir / "sine440_1s.wav", sine(440.0, 0.5, seconds_to_frames(1.0)));

    // sine997_cal.wav: 997 Hz sine, peak amplitude 0.1001 (~ -23 dBFS RMS), 2 s
    ok &=
        write_wav_mono_f32(outdir / "sine997_cal.wav", sine(997.0, 0.1001, seconds_to_frames(2.0)));

    // noise_white_1s.wav: white noise, amplitude 0.25, 1 s
    ok &= write_wav_mono_f32(outdir / "noise_white_1s.wav",
                             white_noise(kBaseSeed, seconds_to_frames(1.0), 0.25));

    // click.wav: 10 ms silence, 0.9 constant for 5 ms, exponential decay tau = 30 ms, total 0.5 s
    {
        const std::size_t n_total = seconds_to_frames(0.5);
        const std::size_t n_silence = seconds_to_frames(0.010);
        const std::size_t n_const = seconds_to_frames(0.005);
        std::vector<float> buf(n_total, 0.0f);
        for (std::size_t i = n_silence; i < n_silence + n_const && i < n_total; ++i) {
            buf[i] = 0.9f;
        }
        for (std::size_t i = n_silence + n_const; i < n_total; ++i) {
            double t = static_cast<double>(i - (n_silence + n_const)) / kSampleRate;
            buf[i] = static_cast<float>(0.9 * std::exp(-t / 0.030));
        }
        ok &= write_wav_mono_f32(outdir / "click.wav", buf);
    }

    // decay_t60.wav: 1 kHz sine, instant onset, exponential decay tau = 0.0724 s (T60 = 0.5 s), 2 s
    {
        std::vector<float> buf = sine(1000.0, 1.0, seconds_to_frames(2.0));
        apply_exp_decay_inplace(buf, 0.0724);
        ok &= write_wav_mono_f32(outdir / "decay_t60.wav", buf);
    }

    // darkset/dark_00..19.wav
    for (int i = 0; i < 20; ++i) {
        ok &= generate_dark_file(outdir / "darkset", i);
    }

    // bright_outlier.wav: white noise -> HPF 3kHz x2 -> exp decay tau=0.05s, peak 0.5, 0.4 s
    {
        std::vector<float> buf = white_noise(kBaseSeed, seconds_to_frames(0.4), 1.0);
        onepole_highpass_inplace(buf, 3000.0);
        onepole_highpass_inplace(buf, 3000.0);
        apply_exp_decay_inplace(buf, 0.05);
        peak_normalize_inplace(buf, 0.5);
        ok &= write_wav_mono_f32(outdir / "bright_outlier.wav", buf);
    }

    if (perf200) {
        std::filesystem::create_directories(outdir / "perf", ec);
        for (int i = 0; i < 200; ++i) {
            ok &= generate_perf_file(outdir / "perf", i);
        }
    }

    if (!profile_set_dir.empty()) {
        std::filesystem::path pdir(profile_set_dir);
        std::filesystem::create_directories(pdir, ec);
        ok &= generate_catfx(pdir);
    }

    if (!extra_dir.empty()) {
        std::filesystem::path extra(extra_dir);
        std::filesystem::create_directories(extra, ec);
        ok &= generate_fixable_outlier(extra);
    }

    if (!psycho_dir.empty()) {
        std::filesystem::path pdir(psycho_dir);
        std::filesystem::create_directories(pdir, ec);
        ok &= generate_psycho(pdir);
    }

    if (!ok) {
        return 2;
    }
    std::fprintf(stderr, "genfixtures: wrote fixtures to %s\n", outdir.string().c_str());
    return 0;
}
