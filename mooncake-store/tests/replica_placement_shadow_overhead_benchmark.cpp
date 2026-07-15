// Experiment-only benchmark. This file is not part of the proposed PR stack.

#include "master_service.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace mooncake::test {
namespace {

constexpr size_t kWarmupIterations = 20000;
constexpr size_t kIterations = 100000;
constexpr size_t kSamples = 25;

ReplicaPlacementShadowConfig EvaluatorConfig() {
    ReplicaPlacementShadowConfig config;
    config.policy.targets[static_cast<size_t>(ReplicaTemperature::COLD)] = {
        ReplicaTierTarget{1, true}, ReplicaTierTarget{0, false},
        ReplicaTierTarget{0, false}, ReplicaTierTarget{0, false}};
    config.policy.targets[static_cast<size_t>(ReplicaTemperature::WARM)] = {
        ReplicaTierTarget{1, true}, ReplicaTierTarget{1, false},
        ReplicaTierTarget{0, false}, ReplicaTierTarget{0, false}};
    config.policy.targets[static_cast<size_t>(ReplicaTemperature::HOT)] = {
        ReplicaTierTarget{2, true}, ReplicaTierTarget{1, false},
        ReplicaTierTarget{0, false}, ReplicaTierTarget{0, false}};
    config.policy.min_complete_replicas = 1;
    config.policy.max_total_replicas = 4;
    config.warm_threshold = 2;
    config.hot_threshold = 3;
    config.signal_ttl = std::chrono::seconds(30);
    config.sketch_width = 4096;
    config.sketch_depth = 4;
    return config;
}

ReplicaPlacementSignalSnapshot AvailableSnapshot() {
    ReplicaPlacementSignalSnapshot snapshot;
    snapshot.generation = 1;
    snapshot.ready = true;
    snapshot.observed_at = std::chrono::steady_clock::now();
    for (auto& tier : snapshot.tiers) {
        tier.allocation_state = ReplicaTierSignalState::AVAILABLE;
        tier.health_state = ReplicaTierSignalState::AVAILABLE;
    }
    return snapshot;
}

enum class Mode { OFF, EXTERNAL_SNAPSHOT, AUTO_COLLECTOR };

const char* ModeName(Mode mode) {
    switch (mode) {
        case Mode::OFF:
            return "off";
        case Mode::EXTERNAL_SNAPSHOT:
            return "external_snapshot";
        case Mode::AUTO_COLLECTOR:
            return "auto_collector";
    }
    return "invalid";
}

struct Context {
    Mode mode;
    std::unique_ptr<MasterService> service;
    UUID client_id;
    std::string key;
    std::vector<double> samples;
};

Context MakeContext(Mode mode) {
    MasterServiceConfig config;
    if (mode != Mode::OFF) {
        MasterReplicaPlacementShadowConfig shadow;
        shadow.evaluator = EvaluatorConfig();
        shadow.auto_collect_master_signals = mode == Mode::AUTO_COLLECTOR;
        shadow.signal_refresh_interval = std::chrono::seconds(1);
        config.replica_placement_shadow_config = shadow;
    }
    auto service = std::make_unique<MasterService>(config);
    if (mode == Mode::EXTERNAL_SNAPSHOT &&
        service->PublishReplicaPlacementSignalSnapshot(AvailableSnapshot()) !=
            ReplicaPlacementSignalPublishStatus::PUBLISHED) {
        throw std::runtime_error("failed to publish external snapshot");
    }

    Segment segment;
    segment.id = generate_uuid();
    segment.name = std::string("overhead_") + ModeName(mode);
    segment.base = 0x650000000;
    segment.size = 16 * 1024 * 1024;
    segment.te_endpoint = segment.name;
    const UUID client_id = generate_uuid();
    if (!service->MountSegment(segment, client_id).has_value()) {
        throw std::runtime_error("failed to mount segment");
    }
    const std::string key = std::string("benchmark_key_") + ModeName(mode);
    ReplicateConfig replicate;
    replicate.replica_num = 1;
    if (!service->PutStart(client_id, key, "benchmark", 1024, replicate)
             .has_value() ||
        !service->PutEnd(client_id, key, "benchmark", ReplicaType::MEMORY)
             .has_value()) {
        throw std::runtime_error("failed to prepare object");
    }
    return Context{mode, std::move(service), client_id, key, {}};
}

uint64_t RunGets(Context& context, size_t iterations) {
    uint64_t successes = 0;
    for (size_t i = 0; i < iterations; ++i) {
        successes += context.service->GetReplicaList(context.key, "benchmark")
                         .has_value();
    }
    return successes;
}

double Percentile(std::vector<double> values, double percentile) {
    std::sort(values.begin(), values.end());
    const size_t index = static_cast<size_t>(
        percentile * static_cast<double>(values.size() - 1));
    return values[index];
}

}  // namespace
}  // namespace mooncake::test

int main() {
    using namespace mooncake::test;
    std::array<Context, 3> contexts = {MakeContext(Mode::OFF),
                                       MakeContext(Mode::EXTERNAL_SNAPSHOT),
                                       MakeContext(Mode::AUTO_COLLECTOR)};

    for (auto& context : contexts) {
        if (RunGets(context, kWarmupIterations) != kWarmupIterations) return 2;
    }

    uint64_t successes = 0;
    std::cout << "mode,sample,iterations,total_ns,ns_per_get\n";
    for (size_t sample = 0; sample < kSamples; ++sample) {
        for (size_t offset = 0; offset < contexts.size(); ++offset) {
            Context& context = contexts[(sample + offset) % contexts.size()];
            const auto begin = std::chrono::steady_clock::now();
            successes += RunGets(context, kIterations);
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - begin)
                    .count();
            const double ns_per_get =
                static_cast<double>(elapsed) / kIterations;
            context.samples.push_back(ns_per_get);
            std::cout << ModeName(context.mode) << ',' << sample << ','
                      << kIterations << ',' << elapsed << ',' << std::fixed
                      << std::setprecision(3) << ns_per_get << '\n';
        }
    }

    const double baseline_p50 = Percentile(contexts[0].samples, 0.50);
    std::cout << "summary,mode,p50_ns_per_get,p95_ns_per_get,"
                 "overhead_vs_off_p50_percent\n";
    for (const auto& context : contexts) {
        const double p50 = Percentile(context.samples, 0.50);
        const double p95 = Percentile(context.samples, 0.95);
        const double overhead = (p50 / baseline_p50 - 1.0) * 100.0;
        std::cout << "summary," << ModeName(context.mode) << ',' << p50 << ','
                  << p95 << ',' << overhead << '\n';
    }
    std::cout << "successes," << successes << "\n";
    return successes == contexts.size() * kSamples * kIterations ? 0 : 3;
}
