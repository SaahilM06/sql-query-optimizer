#pragma once

#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace sql::adaptive {

// A simple epsilon-greedy multi-armed bandit, contextualized by a coarse
// discrete context key (e.g. "products:orders" for which relations a join
// involves) -- not a full contextual bandit with continuous features
// (LinUCB); ROADMAP.md's own algorithm list puts epsilon-greedy first, and
// this is that. Each (context, arm) pair accumulates a running average
// reward; choose() picks the best-known arm with probability (1-epsilon),
// a uniformly random arm otherwise, so it keeps exploring instead of
// locking onto an early (possibly noisy) winner forever.
class BanditModel {
public:
    struct ArmStats {
        size_t count = 0;
        double average_reward = 0.0;
    };

    explicit BanditModel(double epsilon = 0.2, unsigned seed = 1);

    // Picks one of `arms` for `context`. `arms` must be non-empty.
    std::string choose(const std::string& context, const std::vector<std::string>& arms);

    // Records an observed reward (e.g. -latency_ms, matching ROADMAP.md's
    // "reward = negative query latency") for the (context, arm) pair.
    void update(const std::string& context, const std::string& arm, double reward);

    const std::unordered_map<std::string, std::unordered_map<std::string, ArmStats>>& snapshot() const {
        return stats_;
    }
    void load_stats(std::unordered_map<std::string, std::unordered_map<std::string, ArmStats>> stats) {
        stats_ = std::move(stats);
    }

private:
    double epsilon_;
    std::mt19937 rng_;
    std::unordered_map<std::string, std::unordered_map<std::string, ArmStats>> stats_;
};

// Loads saved bandit state from `path` (a fresh, empty model if the file
// doesn't exist or is unreadable/corrupt -- never an error, since "no
// learning yet" is a perfectly valid starting state).
BanditModel load_bandit(const std::string& path, double epsilon = 0.2);
void save_bandit(const std::string& path, const BanditModel& model);

} // namespace sql::adaptive
