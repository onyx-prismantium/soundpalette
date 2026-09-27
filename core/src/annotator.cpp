#include "soundpalette/annotator.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include "soundpalette/describe.h"
#include "soundpalette/manifest.h"
#include "subprocess.h"

namespace sp {

using json = nlohmann::json;
using ojson = nlohmann::ordered_json;

// ---------------------------------------------------------------------------------------------
// §7.2 pure pieces

std::vector<UcsMatch> stage2_shortlist(const Stage1 &s1, std::size_t max_n) {
    std::vector<std::string> tokens = ucs_tokenize(s1.fx_name);
    for (const std::string &k : s1.keywords) {
        for (std::string &t : ucs_tokenize(k)) {
            tokens.push_back(std::move(t));
        }
    }
    for (std::string &t : ucs_tokenize(s1.description)) {
        tokens.push_back(std::move(t));
    }
    bool known_category = false;
    for (std::string_view c : ucs_categories()) {
        known_category = known_category || c == s1.category;
    }
    std::vector<UcsMatch> ranked =
        ucs_rank(tokens, known_category ? s1.category : std::string_view{});
    if (ranked.size() > max_n) {
        ranked.resize(max_n);
    }
    if (known_category) {
        // pad to >= 3 with the category's first SubCategories (CSV order), score 0
        for (const UcsEntry *e : ucs_in_category(s1.category)) {
            if (ranked.size() >= std::max<std::size_t>(3, std::min<std::size_t>(max_n, 3))) {
                break;
            }
            bool present = std::any_of(ranked.begin(), ranked.end(),
                                       [e](const UcsMatch &m) { return m.entry == e; });
            if (!present) {
                ranked.push_back(UcsMatch{e, 0});
            }
        }
    }
    return ranked;
}

Stage2Decision resolve_stage2(const std::vector<UcsMatch> &shortlist,
                              const std::string &model_cat_id, double model_confidence) {
    Stage2Decision d;
    double conf = std::clamp(model_confidence, 0.0, 1.0);
    if (shortlist.empty()) {
        d.note = "empty shortlist";
        return d;
    }
    for (const UcsMatch &m : shortlist) {
        if (m.entry->cat_id == model_cat_id) {
            d.cat_id = model_cat_id;
            d.confidence = conf;
            d.from_shortlist = true;
            return d;
        }
    }
    const UcsEntry *picked = ucs_find(model_cat_id);
    if (picked && picked->category == shortlist.front().entry->category) {
        d.cat_id = model_cat_id;
        d.confidence = conf;
        d.note = "outside shortlist, same category";
        return d;
    }
    d.cat_id = std::string(shortlist.front().entry->cat_id);
    d.confidence = conf * 0.5;
    d.from_shortlist = true;
    d.note = picked ? "model picked another category; using shortlist top"
                    : "model picked an unknown CatID; using shortlist top";
    return d;
}

// ---------------------------------------------------------------------------------------------
// 16 kHz excerpt

bool write_wav_16k_mono(const AudioBuffer &buffer, const std::filesystem::path &path,
                        std::string &err) {
    const std::vector<float> &in = buffer.samples48k_mono;
    const int factor = 3;
    // Windowed-sinc lowpass at 7 kHz (of 48 kHz), 63 taps, Blackman window.
    const int taps = 63;
    const int half = taps / 2;
    const double fc = 7000.0 / 48000.0;
    const double kPi = 3.14159265358979323846;
    std::vector<double> h(static_cast<std::size_t>(taps));
    double sum = 0.0;
    for (int n = 0; n < taps; ++n) {
        int m = n - half;
        double sinc = m == 0 ? 2.0 * fc : std::sin(2.0 * kPi * fc * m) / (kPi * m);
        double w = 0.42 - 0.5 * std::cos(2.0 * kPi * n / (taps - 1)) +
                   0.08 * std::cos(4.0 * kPi * n / (taps - 1));
        h[static_cast<std::size_t>(n)] = sinc * w;
        sum += h[static_cast<std::size_t>(n)];
    }
    for (double &v : h) {
        v /= sum;
    }
    std::size_t out_n = in.size() / static_cast<std::size_t>(factor);
    std::vector<std::int16_t> out(out_n);
    for (std::size_t o = 0; o < out_n; ++o) {
        long center = static_cast<long>(o) * factor;
        double acc = 0.0;
        for (int n = 0; n < taps; ++n) {
            long idx = center + (n - half);
            if (idx >= 0 && idx < static_cast<long>(in.size())) {
                acc += h[static_cast<std::size_t>(n)] * in[static_cast<std::size_t>(idx)];
            }
        }
        double v = std::clamp(acc, -1.0, 1.0);
        out[o] = static_cast<std::int16_t>(std::lrint(v * 32767.0));
    }
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        err = "cannot write " + path.string();
        return false;
    }
    auto put32 = [&](std::uint32_t v) {
        char b[4] = {static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF),
                     static_cast<char>((v >> 16) & 0xFF), static_cast<char>((v >> 24) & 0xFF)};
        f.write(b, 4);
    };
    auto put16 = [&](std::uint16_t v) {
        char b[2] = {static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF)};
        f.write(b, 2);
    };
    const std::uint32_t data_bytes = static_cast<std::uint32_t>(out.size() * 2);
    f.write("RIFF", 4);
    put32(36 + data_bytes);
    f.write("WAVE", 4);
    f.write("fmt ", 4);
    put32(16);
    put16(1); // PCM
    put16(1); // mono
    put32(16000);
    put32(16000 * 2);
    put16(2);
    put16(16);
    f.write("data", 4);
    put32(data_bytes);
    for (std::int16_t s : out) {
        put16(static_cast<std::uint16_t>(s));
    }
    return static_cast<bool>(f);
}

// ---------------------------------------------------------------------------------------------
// Protocol client

namespace {

class AnnotatorClient {
public:
    enum class Status { kOk, kTimeout, kClosed, kWriteFailed, kNotRunning };

    explicit AnnotatorClient(std::vector<std::string> argv, double timeout_s)
        : argv_(std::move(argv)), timeout_s_(timeout_s) {}
    ~AnnotatorClient() {
        stop();
    }

    bool start(std::string &err) {
        std::lock_guard<std::mutex> lock(restart_mu_);
        return start_locked(err);
    }

    // Kills and respawns; pending callers observe kClosed. Returns false + err if the respawn
    // fails (the run then ends).
    bool restart(std::string &err) {
        std::lock_guard<std::mutex> lock(restart_mu_);
        stop_locked();
        ++restarts_;
        return start_locked(err);
    }

    std::size_t restarts() const {
        return restarts_;
    }
    const AnnotatorInfo &info() const {
        return info_;
    }
    bool supports(const std::string &stage) const {
        return std::find(info_.stages.begin(), info_.stages.end(), stage) != info_.stages.end();
    }

    Status call(ojson request, json &response) {
        std::string id = std::to_string(next_id_.fetch_add(1));
        request["id"] = id;
        std::string line = request.dump();
        std::shared_ptr<Subprocess> proc;
        {
            std::lock_guard<std::mutex> lock(mu_);
            proc = proc_;
            if (!proc || closed_) {
                return Status::kNotRunning;
            }
            pending_.insert(id);
        }
        {
            std::lock_guard<std::mutex> wlock(write_mu_);
            if (!proc->write_line(line)) {
                std::lock_guard<std::mutex> lock(mu_);
                pending_.erase(id);
                return Status::kWriteFailed;
            }
        }
        std::unique_lock<std::mutex> lock(mu_);
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(static_cast<long long>(timeout_s_ * 1000.0));
        for (;;) {
            auto it = responses_.find(id);
            if (it != responses_.end()) {
                response = std::move(it->second);
                responses_.erase(it);
                pending_.erase(id);
                return Status::kOk;
            }
            if (closed_ || proc != proc_) {
                pending_.erase(id);
                return Status::kClosed;
            }
            if (cv_.wait_until(lock, deadline) == std::cv_status::timeout) {
                if (responses_.count(id) == 0) {
                    pending_.erase(id);
                    return Status::kTimeout;
                }
            }
        }
    }

    void stop() {
        std::lock_guard<std::mutex> lock(restart_mu_);
        stop_locked();
    }

private:
    bool start_locked(std::string &err) {
        auto proc = std::make_shared<Subprocess>();
        if (!proc->start(argv_, err)) {
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(mu_);
            proc_ = proc;
            closed_ = false;
            responses_.clear();
        }
        dispatcher_ = std::thread([this, proc] { dispatch_loop(proc); });
        // handshake
        ojson hello = ojson::object();
        hello["v"] = kAnnotatorProtocolVersion;
        hello["hello"] = true;
        json reply;
        Status st;
        {
            // temporarily use a shorter deadline for the hello
            double saved = timeout_s_;
            timeout_s_ = std::min(timeout_s_, 60.0);
            st = call_hello(hello, reply);
            timeout_s_ = saved;
        }
        if (st != Status::kOk) {
            err = st == Status::kTimeout ? "annotator did not answer the hello handshake"
                                         : "annotator exited during the hello handshake";
            stop_locked();
            return false;
        }
        info_.name = reply.value("name", "");
        info_.model = reply.value("model", "");
        info_.prompt_version = reply.value("prompt_version", "");
        info_.stages.clear();
        if (reply.contains("stages") && reply["stages"].is_array()) {
            for (const auto &s : reply["stages"]) {
                if (s.is_string()) {
                    info_.stages.push_back(s.get<std::string>());
                }
            }
        }
        return true;
    }

    Status call_hello(const ojson &hello, json &reply) {
        std::shared_ptr<Subprocess> proc;
        {
            std::lock_guard<std::mutex> lock(mu_);
            proc = proc_;
        }
        {
            std::lock_guard<std::mutex> wlock(write_mu_);
            if (!proc->write_line(hello.dump())) {
                return Status::kWriteFailed;
            }
        }
        std::unique_lock<std::mutex> lock(mu_);
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(static_cast<long long>(timeout_s_ * 1000.0));
        for (;;) {
            auto it = responses_.find("hello");
            if (it != responses_.end()) {
                reply = std::move(it->second);
                responses_.erase(it);
                return Status::kOk;
            }
            if (closed_) {
                return Status::kClosed;
            }
            if (cv_.wait_until(lock, deadline) == std::cv_status::timeout &&
                responses_.count("hello") == 0) {
                return Status::kTimeout;
            }
        }
    }

    void stop_locked() {
        std::shared_ptr<Subprocess> proc;
        {
            std::lock_guard<std::mutex> lock(mu_);
            proc = proc_;
            closed_ = true;
            cv_.notify_all();
        }
        if (proc) {
            proc->kill();
        }
        if (dispatcher_.joinable()) {
            dispatcher_.join();
        }
        std::lock_guard<std::mutex> lock(mu_);
        proc_.reset();
        responses_.clear();
        cv_.notify_all();
    }

    void dispatch_loop(std::shared_ptr<Subprocess> proc) {
        std::string line;
        for (;;) {
            if (!proc->read_line(line, 0.25)) {
                if (proc->eof()) {
                    break;
                }
                std::lock_guard<std::mutex> lock(mu_);
                if (closed_ || proc_ != proc) {
                    break;
                }
                continue;
            }
            json j;
            try {
                j = json::parse(line);
            } catch (const std::exception &) {
                continue; // not JSON: ignore (annotators must log to stderr, not stdout)
            }
            if (!j.is_object()) {
                continue;
            }
            std::string key;
            if (j.value("hello", false)) {
                key = "hello";
            } else if (j.contains("id")) {
                key = j["id"].is_string() ? j["id"].get<std::string>() : j["id"].dump();
            } else {
                continue;
            }
            std::lock_guard<std::mutex> lock(mu_);
            responses_[key] = std::move(j);
            cv_.notify_all();
        }
        std::lock_guard<std::mutex> lock(mu_);
        if (proc_ == proc) {
            closed_ = true;
        }
        cv_.notify_all();
    }

    std::vector<std::string> argv_;
    double timeout_s_;
    std::mutex restart_mu_;
    std::mutex write_mu_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::shared_ptr<Subprocess> proc_;
    std::thread dispatcher_;
    std::unordered_map<std::string, json> responses_;
    std::unordered_set<std::string> pending_;
    bool closed_ = true;
    std::atomic<std::uint64_t> next_id_{1};
    std::size_t restarts_ = 0;
    AnnotatorInfo info_;
};

std::string clip_sentence(std::string s, std::size_t max_chars) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\n')) {
        s.pop_back();
    }
    while (!s.empty() && (s.front() == ' ' || s.front() == '\n')) {
        s.erase(s.begin());
    }
    if (s.size() <= max_chars) {
        return s;
    }
    std::size_t cut = s.rfind(' ', max_chars - 1);
    if (cut == std::string::npos || cut < max_chars / 2) {
        cut = max_chars - 1;
    }
    s.resize(cut);
    while (!s.empty() && (s.back() == ',' || s.back() == ' ' || s.back() == ';')) {
        s.pop_back();
    }
    return s + ".";
}

std::string limit_words(const std::string &s, std::size_t max_words) {
    std::string out;
    std::size_t words = 0;
    std::string cur;
    auto flush = [&] {
        if (!cur.empty() && words < max_words) {
            out += (out.empty() ? "" : " ") + cur;
            ++words;
        }
        cur.clear();
    };
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '\n') {
            flush();
        } else {
            cur.push_back(c);
        }
    }
    flush();
    return out;
}

ojson categories_json() {
    static const ojson cached = [] {
        ojson arr = ojson::array();
        for (std::string_view c : ucs_categories()) {
            ojson o = ojson::object();
            o["name"] = std::string(c);
            ojson subs = ojson::array();
            for (const UcsEntry *e : ucs_in_category(c)) {
                subs.push_back(std::string(e->sub_category));
            }
            o["sub_categories"] = std::move(subs);
            arr.push_back(std::move(o));
        }
        return arr;
    }();
    return cached;
}

} // namespace

namespace {
constexpr std::size_t kMaxRestarts = 5; // a crash-looping annotator ends the run, not the process
} // namespace

std::vector<std::string> annotator_argv(const AnnotatorOptions &options) {
    std::string cmd = options.command;
    if (cmd.empty()) {
        if (const char *env = std::getenv("SP_ANNOTATOR")) {
            cmd = env;
        }
    }
    if (cmd.empty()) {
        cmd = kDefaultAnnotatorCommand;
    }
    return split_command_line(cmd);
}

AnnotateReport annotate_paths(Library &library, const std::vector<std::string> &rel_paths,
                              const AnnotatorOptions &options) {
    AnnotateReport report;
    report.requested = rel_paths.size();
    report.files.resize(rel_paths.size());
    if (rel_paths.empty()) {
        return report;
    }

    AnnotatorClient client(annotator_argv(options), options.timeout_s);
    std::string err;
    if (!client.start(err)) {
        report.fatal = err;
        report.errors = rel_paths.size();
        for (std::size_t i = 0; i < rel_paths.size(); ++i) {
            report.files[i].path = rel_paths[i];
            report.files[i].error = err;
        }
        return report;
    }
    report.annotator = client.info();

    std::error_code ec;
    std::filesystem::path tmp_dir =
        std::filesystem::temp_directory_path(ec) /
        ("soundpalette-annotate-" + std::to_string(static_cast<long long>(std::time(nullptr))) +
         "-" +
         std::to_string(static_cast<unsigned long long>(
             std::hash<std::thread::id>{}(std::this_thread::get_id()) & 0xffff)));
    std::filesystem::create_directories(tmp_dir, ec);

    std::mutex lib_mu;
    std::mutex report_mu;
    std::atomic<std::size_t> next{0};
    std::atomic<std::size_t> done{0};
    std::atomic<bool> fatal{false};

    auto set_result = [&](std::size_t i, AnnotateFileResult r) {
        std::lock_guard<std::mutex> lock(report_mu);
        report.files[i] = std::move(r);
        if (options.on_progress) {
            options.on_progress(done.fetch_add(1) + 1, rel_paths.size(), report.files[i].path,
                                report.files[i].ok ? report.files[i].status
                                                   : "error: " + report.files[i].error);
        }
    };

    auto worker = [&](unsigned int worker_id) {
        for (;;) {
            std::size_t i = next.fetch_add(1);
            if (i >= rel_paths.size() || fatal) {
                break;
            }
            AnnotateFileResult res;
            res.path = rel_paths[i];
            if (options.should_cancel && options.should_cancel()) {
                res.ok = true;
                res.status = "skipped: cancelled";
                set_result(i, std::move(res));
                continue;
            }

            std::optional<LibraryRow> row;
            {
                std::lock_guard<std::mutex> lock(lib_mu);
                row = library.get(res.path);
            }
            if (!row) {
                res.error = "not in the library";
                set_result(i, std::move(res));
                continue;
            }
            if (row->annotation && row->annotation->locked) {
                res.ok = true;
                res.status = "skipped: locked";
                res.annotation = *row->annotation;
                set_result(i, std::move(res));
                continue;
            }
            if (row->annotation && !options.force &&
                static_cast<int>(row->annotation->source) >
                    static_cast<int>(AnnotationSource::kModel)) {
                res.ok = true;
                res.status = "skipped: precedence";
                res.annotation = *row->annotation;
                set_result(i, std::move(res));
                continue;
            }
            if (!row->entry.error.empty()) {
                res.error = "analysis error: " + row->entry.error;
                set_result(i, std::move(res));
                continue;
            }

            std::string derr;
            auto buffer = decode_file(library.root() / res.path, derr);
            if (!buffer) {
                res.error = "decode: " + derr;
                set_result(i, std::move(res));
                continue;
            }
            std::filesystem::path wav =
                tmp_dir / (std::to_string(i) + "-" + std::to_string(worker_id) + ".wav");
            if (!write_wav_16k_mono(*buffer, wav, derr)) {
                res.error = derr;
                set_result(i, std::move(res));
                continue;
            }

            // stage 1
            ojson req = ojson::object();
            req["v"] = kAnnotatorProtocolVersion;
            req["stage"] = "describe";
            req["path"] = res.path;
            req["sha256"] = row->sha256;
            ojson audio = ojson::object();
            audio["path"] = wav.generic_string();
            audio["sample_rate"] = 16000;
            audio["duration_s"] = buffer->duration_s;
            audio["truncated"] = buffer->truncated;
            req["audio"] = std::move(audio);
            ojson analysis = ojson::object();
            analysis["lufs_i"] =
                std::isfinite(row->entry.loudness.lufs_i) ? row->entry.loudness.lufs_i : -900.0;
            analysis["true_peak_db"] = row->entry.loudness.true_peak_db;
            analysis["silent"] = row->entry.loudness.silent;
            analysis["duration_s"] = row->entry.duration_s;
            analysis["describe"] =
                describe_words(row->entry.features, row->entry.loudness, row->entry.psycho);
            ojson words = ojson::array();
            for (const std::string &w :
                 describe_dim_words(row->entry.features, row->entry.loudness, row->entry.psycho)) {
                words.push_back(w);
            }
            analysis["words"] = std::move(words);
            ojson psycho = ojson::object();
            psycho["sones_n5"] = row->entry.psycho.sones_n5;
            psycho["sharpness_acum"] = row->entry.psycho.sharpness_acum;
            psycho["roughness_asper"] = row->entry.psycho.roughness_asper;
            psycho["fluctuation_vacil"] = row->entry.psycho.fluctuation_vacil;
            analysis["psycho"] = std::move(psycho);
            req["analysis"] = std::move(analysis);
            req["categories"] = categories_json();

            json resp;
            AnnotatorClient::Status st = AnnotatorClient::Status::kClosed;
            for (int attempt = 0; attempt < 2; ++attempt) {
                st = client.call(req, resp);
                if (st == AnnotatorClient::Status::kOk || st == AnnotatorClient::Status::kTimeout) {
                    break;
                }
                // closed / not running: (re)start once and retry
                std::string rerr;
                if (client.restarts() >= kMaxRestarts) {
                    fatal = true;
                    res.error = "annotator keeps exiting (" + std::to_string(client.restarts()) +
                                " restarts); giving up";
                    break;
                }
                if (!client.restart(rerr)) {
                    fatal = true;
                    res.error = "annotator restart failed: " + rerr;
                    break;
                }
            }
            std::filesystem::remove(wav, ec);
            if (st == AnnotatorClient::Status::kTimeout) {
                {
                    std::lock_guard<std::mutex> lock(report_mu);
                    ++report.timeouts;
                }
                res.error = "timeout after " + std::to_string(static_cast<int>(options.timeout_s)) +
                            " s (annotator restarted)";
                std::string rerr;
                if (!client.restart(rerr)) {
                    fatal = true;
                    res.error += "; restart failed: " + rerr;
                }
                set_result(i, std::move(res));
                continue;
            }
            if (st != AnnotatorClient::Status::kOk) {
                if (res.error.empty()) {
                    res.error = "annotator unavailable";
                }
                set_result(i, std::move(res));
                continue;
            }
            if (resp.contains("error")) {
                res.error =
                    "annotator: " + (resp["error"].is_string() ? resp["error"].get<std::string>()
                                                               : resp["error"].dump());
                set_result(i, std::move(res));
                continue;
            }

            Stage1 s1;
            s1.description = clip_sentence(resp.value("description", ""), 140);
            s1.fx_name = limit_words(resp.value("fx_name", ""), 6);
            s1.category = resp.value("category", "");
            s1.confidence = resp.value("confidence", 0.0);
            if (resp.contains("keywords") && resp["keywords"].is_array()) {
                for (const auto &k : resp["keywords"]) {
                    if (k.is_string() && s1.keywords.size() < 10) {
                        std::string kw = k.get<std::string>();
                        std::transform(kw.begin(), kw.end(), kw.begin(), [](unsigned char c) {
                            return static_cast<char>(std::tolower(c));
                        });
                        if (!kw.empty()) {
                            s1.keywords.push_back(kw);
                        }
                    }
                }
            }
            if (s1.description.empty() && s1.fx_name.empty() && s1.keywords.empty()) {
                res.error = "annotator returned an empty answer";
                set_result(i, std::move(res));
                continue;
            }

            // stage 2
            std::vector<UcsMatch> shortlist = stage2_shortlist(s1, options.max_shortlist);
            Stage2Decision decision;
            if (shortlist.empty()) {
                decision.note = "no candidates";
            } else if (!client.supports("choose")) {
                decision.cat_id = std::string(shortlist.front().entry->cat_id);
                decision.confidence = s1.confidence * 0.5;
                decision.from_shortlist = true;
                decision.note = "annotator has no choose stage";
            } else {
                ojson req2 = ojson::object();
                req2["v"] = kAnnotatorProtocolVersion;
                req2["stage"] = "choose";
                req2["path"] = res.path;
                req2["sha256"] = row->sha256;
                req2["description"] = s1.description;
                req2["fx_name"] = s1.fx_name;
                req2["keywords"] = s1.keywords;
                ojson cands = ojson::array();
                for (const UcsMatch &m : shortlist) {
                    ojson c = ojson::object();
                    c["cat_id"] = std::string(m.entry->cat_id);
                    c["category"] = std::string(m.entry->category);
                    c["sub_category"] = std::string(m.entry->sub_category);
                    c["explanation"] = std::string(m.entry->explanation);
                    cands.push_back(std::move(c));
                }
                req2["candidates"] = std::move(cands);
                json resp2;
                AnnotatorClient::Status st2 = client.call(req2, resp2);
                if (st2 == AnnotatorClient::Status::kTimeout) {
                    {
                        std::lock_guard<std::mutex> lock(report_mu);
                        ++report.timeouts;
                    }
                    res.error = "timeout in stage 2 (annotator restarted)";
                    std::string rerr;
                    if (!client.restart(rerr)) {
                        fatal = true;
                    }
                    set_result(i, std::move(res));
                    continue;
                }
                if (st2 != AnnotatorClient::Status::kOk || resp2.contains("error")) {
                    // degrade gracefully: keep the description, take the shortlist top
                    decision.cat_id = std::string(shortlist.front().entry->cat_id);
                    decision.confidence = s1.confidence * 0.5;
                    decision.from_shortlist = true;
                    decision.note = "stage 2 failed; using shortlist top";
                } else {
                    decision = resolve_stage2(shortlist, resp2.value("cat_id", ""),
                                              resp2.value("confidence", s1.confidence));
                }
            }

            Annotation a;
            a.cat_id = decision.cat_id;
            a.fx_name = s1.fx_name;
            a.description = s1.description;
            a.keywords = s1.keywords;
            a.confidence = decision.confidence;
            a.source = AnnotationSource::kModel;
            a.model = client.info().model;
            a.prompt_version = client.info().prompt_version;
            {
                ojson cj = ojson::array();
                for (const UcsMatch &m : shortlist) {
                    ojson c = ojson::object();
                    c["cat_id"] = std::string(m.entry->cat_id);
                    c["score"] = m.score;
                    cj.push_back(std::move(c));
                }
                a.candidates_json = cj.dump();
            }
            res.annotation = a;
            res.ok = true;
            if (options.dry_run) {
                res.status = "dry-run";
            } else {
                std::lock_guard<std::mutex> lock(lib_mu);
                switch (library.set_annotation(res.path, a, options.force)) {
                case SetResult::kWritten:
                    res.status = "written";
                    break;
                case SetResult::kSkippedLocked:
                    res.status = "skipped: locked";
                    break;
                case SetResult::kSkippedLowerPrecedence:
                    res.status = "skipped: precedence";
                    break;
                case SetResult::kUnknownPath:
                    res.ok = false;
                    res.error = "row vanished";
                    break;
                }
            }
            if (!decision.note.empty()) {
                res.status += " (" + decision.note + ")";
            }
            set_result(i, std::move(res));
        }
    };

    unsigned int threads = static_cast<unsigned int>(std::max(1, options.inflight));
    threads = std::min<unsigned int>(threads, static_cast<unsigned int>(rel_paths.size()));
    std::vector<std::thread> pool;
    for (unsigned int t = 0; t < threads; ++t) {
        pool.emplace_back(worker, t);
    }
    for (auto &th : pool) {
        th.join();
    }
    client.stop();
    std::filesystem::remove_all(tmp_dir, ec);

    report.restarts = client.restarts();
    if (fatal && report.fatal.empty()) {
        for (const AnnotateFileResult &r : report.files) {
            if (!r.ok && !r.error.empty()) {
                report.fatal = r.error;
                break;
            }
        }
    }
    for (const AnnotateFileResult &r : report.files) {
        if (!r.ok) {
            ++report.errors;
        } else if (r.status.rfind("written", 0) == 0 || r.status.rfind("dry-run", 0) == 0) {
            ++report.annotated;
        } else if (r.status.rfind("skipped: locked", 0) == 0) {
            ++report.skipped_locked;
        } else if (r.status.rfind("skipped: precedence", 0) == 0) {
            ++report.skipped_precedence;
        }
    }
    return report;
}

std::optional<AnnotatorInfo> probe_annotator(const AnnotatorOptions &options, std::string &err) {
    AnnotatorClient client(annotator_argv(options), std::min(options.timeout_s, 60.0));
    if (!client.start(err)) {
        return std::nullopt;
    }
    AnnotatorInfo info = client.info();
    client.stop();
    return info;
}

} // namespace sp
