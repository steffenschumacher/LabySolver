#include "../CoordinatorCheckpoint.hpp"
#include "../Master.hpp"
#include "test_util.hpp"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <vector>

std::vector<Move> candidateMoves(const JobState&) { return {{0, 0}, {1, 0}}; }
void applyMove(JobState& out, const JobState& parent, Move move) {
    out = parent;
    uint8_t depth = parent.boardBytes[7];
    out.boardBytes[depth] = move.insertPoint;
    out.boardBytes[7] = depth + 1;
    out.insertPoint = move.insertPoint;
}
bool allBugsEaten(const JobState&) { return false; }
void launchCudaBatch(JobState* states, size_t count) {
    for (size_t i = 0; i < count; ++i) states[i].offBoard = 0;
}

static std::vector<Seed> generate(MasterSeedPersistence* persistence) {
    NodePool pool(100);
    Dispatcher dispatcher(1);
    SeedQueue<32> queue;
    SearchGlobals globals;
    JobState initial{};
    MasterSearchConfig config;
    config.maxDepth = 4;
    config.seedDepth = 2;
    dispatcher.start();
    Master master(dispatcher, pool, queue, globals, initial, nullptr, config, persistence);
    master.run();
    dispatcher.stop();
    std::vector<Seed> seeds;
    Seed seed;
    while (queue.pop(seed)) seeds.push_back(seed);
    return seeds;
}

int main() {
    namespace fs = std::filesystem;
    const std::vector<Seed> baseline = generate(nullptr);
    CHECK(baseline.size() == 4);
    const auto directory = fs::temp_directory_path() /
        ("labysolver-master-recovery-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(directory);
    const fs::path path = directory / "coordinator.chk";

    // Pretend power failed after the first two seed checkpoints and before the
    // shallow deterministic master traversal completed.
    {
        durable::CoordinatorCheckpoint partial(path);
        for (uint64_t ordinal = 1; ordinal <= 2; ++ordinal) {
            Seed seed = baseline[ordinal - 1];
            CHECK(partial.registerGeneratedSeed(ordinal, seed));
        }
    }

    durable::CoordinatorCheckpoint recovered(path);
    const std::vector<Seed> newlyGenerated = generate(&recovered);
    CHECK(newlyGenerated.size() == 2); // ordinals 1-2 skipped, 3-4 emitted
    auto snapshot = recovered.snapshot();
    CHECK(snapshot.generatedSeeds == 4);
    CHECK(snapshot.pending.size() == 4);
    CHECK(snapshot.masterFinished);
    for (size_t i = 0; i < snapshot.pending.size(); ++i) {
        CHECK(snapshot.pending[i].id == i + 1);
        CHECK(std::memcmp(&snapshot.pending[i].state, &baseline[i].state, sizeof(JobState)) == 0);
    }

    fs::remove_all(directory);
    REPORT();
    return 0;
}
