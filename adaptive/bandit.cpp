#include "bandit.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>

#include "../util/json.hpp"

namespace sql::adaptive {

using sql::util::JsonValue;

BanditModel::BanditModel(double epsilon, unsigned seed) : epsilon_(epsilon), rng_(seed) {}

std::string BanditModel::choose(const std::string& context, const std::vector<std::string>& arms) {
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    auto& context_stats = stats_[context];

    bool explore = unit(rng_) < epsilon_;
    if (!explore) {
        // Exploit: best average reward so far among arms with at least one
        // observation. An arm with none loses to any arm that has data --
        // fine, it'll get tried plenty during exploration, and the very
        // first call for a fresh context has nothing to exploit anyway, so
        // falls through to a random pick below.
        const std::string* best = nullptr;
        double best_reward = 0.0;
        for (const auto& arm : arms) {
            auto it = context_stats.find(arm);
            if (it == context_stats.end() || it->second.count == 0) continue;
            if (best == nullptr || it->second.average_reward > best_reward) {
                best = &arm;
                best_reward = it->second.average_reward;
            }
        }
        if (best != nullptr) return *best;
    }

    std::uniform_int_distribution<size_t> pick(0, arms.size() - 1);
    return arms[pick(rng_)];
}

void BanditModel::update(const std::string& context, const std::string& arm, double reward) {
    ArmStats& s = stats_[context][arm];
    s.average_reward = (s.average_reward * static_cast<double>(s.count) + reward) / static_cast<double>(s.count + 1);
    ++s.count;
}

BanditModel load_bandit(const std::string& path, double epsilon) {
    BanditModel model(epsilon);
    std::ifstream file(path);
    if (!file) return model; // no saved state yet -- start fresh, not an error

    std::stringstream ss;
    ss << file.rdbuf();
    JsonValue root;
    try {
        root = sql::util::parse_json(ss.str());
    } catch (const std::exception&) {
        return model; // corrupted/empty file -- start fresh rather than fail the coordinator over this
    }

    std::unordered_map<std::string, std::unordered_map<std::string, BanditModel::ArmStats>> stats;
    if (root.kind == JsonValue::Kind::Object) {
        for (const auto& [context, arms_json] : root.object_val) {
            if (arms_json.kind != JsonValue::Kind::Object) continue;
            for (const auto& [arm, stat_json] : arms_json.object_val) {
                BanditModel::ArmStats s;
                if (const auto* c = stat_json.find("count")) s.count = static_cast<size_t>(c->as_number());
                if (const auto* r = stat_json.find("average_reward")) s.average_reward = r->as_number();
                stats[context][arm] = s;
            }
        }
    }
    model.load_stats(std::move(stats));
    return model;
}

void save_bandit(const std::string& path, const BanditModel& model) {
    JsonValue root = JsonValue::make_object();
    for (const auto& [context, arms] : model.snapshot()) {
        JsonValue arms_json = JsonValue::make_object();
        for (const auto& [arm, s] : arms) {
            JsonValue stat_json = JsonValue::make_object();
            stat_json.object_val["count"] = JsonValue::make_number(static_cast<double>(s.count));
            stat_json.object_val["average_reward"] = JsonValue::make_number(s.average_reward);
            arms_json.object_val[arm] = std::move(stat_json);
        }
        root.object_val[context] = std::move(arms_json);
    }
    std::ofstream file(path);
    if (!file) throw std::runtime_error("adaptive: could not open bandit state file for write: " + path);
    file << sql::util::to_json(root);
}

} // namespace sql::adaptive
