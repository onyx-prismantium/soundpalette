// Recipe schema, validation, and canonical JSON round-tripping (extension §6.3).

#include <cmath>
#include <fstream>

#include <nlohmann/json.hpp>

extern "C" {
#include "sha256.h" // plain C header (extern/sha256), no C++ guards of its own
}

#include "soundpalette/recipe.h"

namespace sp {

namespace {

double round4(double x) {
    return std::round(x * 10000.0) / 10000.0;
}

std::optional<OpType> op_from_name(const std::string &name) {
    if (name == "gain_db") {
        return OpType::kGainDb;
    }
    if (name == "gain_to_lufs") {
        return OpType::kGainToLufs;
    }
    if (name == "low_shelf") {
        return OpType::kLowShelf;
    }
    if (name == "high_shelf") {
        return OpType::kHighShelf;
    }
    if (name == "attack_soften") {
        return OpType::kAttackSoften;
    }
    if (name == "tail_shorten") {
        return OpType::kTailShorten;
    }
    return std::nullopt;
}

} // namespace

bool validate_ops(const std::vector<Op> &ops, std::string &err) {
    // Chain order rule (extension §6.2): gain_to_lufs must be last if present, because EQ and
    // envelope edits change loudness.
    for (std::size_t i = 0; i < ops.size(); ++i) {
        if (ops[i].op == OpType::kGainToLufs && i + 1 != ops.size()) {
            err = "gain_to_lufs must be the last op in the chain";
            return false;
        }
    }
    return true;
}

std::string file_sha256(const std::filesystem::path &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return "";
    }
    SHA256_CTX ctx;
    sha256_init(&ctx);
    char buf[65536];
    while (f.read(buf, sizeof(buf)) || f.gcount() > 0) {
        sha256_update(&ctx, reinterpret_cast<const BYTE *>(buf),
                      static_cast<std::size_t>(f.gcount()));
    }
    unsigned char digest[SHA256_BLOCK_SIZE];
    sha256_final(&ctx, digest);
    static const char *kHex = "0123456789abcdef";
    std::string out;
    out.reserve(SHA256_BLOCK_SIZE * 2);
    for (unsigned char b : digest) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0xF]);
    }
    return out;
}

std::string recipe_to_json(const Recipe &r) {
    using json = nlohmann::ordered_json;
    json j;
    j["recipe_version"] = r.recipe_version;
    j["source"] = {{"path", r.source_path}, {"sha256", r.source_sha256}};
    j["target"] = {{"baseline", r.target_baseline},
                   {"mapping_version", r.target_mapping_version},
                   {"threshold", round4(r.target_threshold)}};
    j["ops"] = json::array();
    for (const Op &op : r.ops) {
        json o;
        o["op"] = op_name(op.op);
        switch (op.op) {
        case OpType::kGainDb:
            o["db"] = round4(op.db);
            break;
        case OpType::kGainToLufs:
            o["target_lufs"] = round4(op.target_lufs);
            o["tp_ceiling_db"] = round4(op.tp_ceiling_db);
            break;
        case OpType::kLowShelf:
        case OpType::kHighShelf:
            o["freq_hz"] = round4(op.freq_hz);
            o["gain_db"] = round4(op.gain_db);
            o["q"] = round4(op.q);
            break;
        case OpType::kAttackSoften:
            o["fade_ms"] = round4(op.fade_ms);
            break;
        case OpType::kTailShorten:
            o["target_tail_s"] = round4(op.target_tail_s);
            break;
        }
        j["ops"].push_back(std::move(o));
    }
    json result;
    result["iterations"] = r.result_iterations;
    result["converged"] = r.result_converged;
    result["limited_by_peak"] = r.result_limited_by_peak;
    result["unresolved"] = r.result_unresolved;
    result["max_z_before"] = round4(r.result_max_z_before);
    result["max_z_after"] = round4(r.result_max_z_after);
    json before = json::object();
    for (const auto &[k, v] : r.result_dims_before) {
        before[k] = round4(v);
    }
    json after = json::object();
    for (const auto &[k, v] : r.result_dims_after) {
        after[k] = round4(v);
    }
    result["dims_before"] = std::move(before);
    result["dims_after"] = std::move(after);
    j["result"] = std::move(result);
    return j.dump(2);
}

std::optional<Recipe> recipe_from_json(const std::string &text, std::string &err) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(text);
    } catch (const std::exception &e) {
        err = e.what();
        return std::nullopt;
    }

    Recipe r;
    try {
        r.recipe_version = j.at("recipe_version").get<int>();
        if (r.recipe_version != 1) {
            err = "unsupported recipe_version";
            return std::nullopt;
        }
        if (j.contains("source")) {
            r.source_path = j["source"].value("path", "");
            r.source_sha256 = j["source"].value("sha256", "");
        }
        if (j.contains("target")) {
            r.target_baseline = j["target"].value("baseline", "");
            r.target_mapping_version = j["target"].value("mapping_version", 1);
            r.target_threshold = j["target"].value("threshold", 2.5);
        }
        for (const auto &o : j.at("ops")) {
            auto type = op_from_name(o.at("op").get<std::string>());
            if (!type.has_value()) {
                err = "unknown op: " + o.at("op").get<std::string>();
                return std::nullopt;
            }
            Op op;
            op.op = *type;
            switch (*type) {
            case OpType::kGainDb:
                op.db = o.at("db").get<double>();
                break;
            case OpType::kGainToLufs:
                op.target_lufs = o.at("target_lufs").get<double>();
                op.tp_ceiling_db = o.value("tp_ceiling_db", -1.0);
                break;
            case OpType::kLowShelf:
            case OpType::kHighShelf:
                op.freq_hz = o.at("freq_hz").get<double>();
                op.gain_db = o.at("gain_db").get<double>();
                op.q = o.value("q", 0.707);
                break;
            case OpType::kAttackSoften:
                op.fade_ms = o.at("fade_ms").get<double>();
                break;
            case OpType::kTailShorten:
                op.target_tail_s = o.at("target_tail_s").get<double>();
                break;
            }
            r.ops.push_back(op);
        }
        if (j.contains("result")) {
            const auto &res = j["result"];
            r.result_iterations = res.value("iterations", 0);
            r.result_converged = res.value("converged", false);
            r.result_limited_by_peak = res.value("limited_by_peak", false);
            if (res.contains("unresolved")) {
                r.result_unresolved = res["unresolved"].get<std::vector<std::string>>();
            }
            r.result_max_z_before = res.value("max_z_before", 0.0);
            r.result_max_z_after = res.value("max_z_after", 0.0);
        }
    } catch (const std::exception &e) {
        err = e.what();
        return std::nullopt;
    }

    if (!validate_ops(r.ops, err)) {
        return std::nullopt;
    }
    return r;
}

} // namespace sp
