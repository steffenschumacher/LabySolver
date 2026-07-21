#pragma once

#include "SeedCodec.hpp"
#include "SearchInstrumentation.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace durable {

class CheckpointError : public std::runtime_error {
public:
    explicit CheckpointError(const std::string& message) : std::runtime_error(message) {}
};

inline uint32_t crc32(const uint8_t* data, size_t size) {
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

inline void appendU32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value >> 24));
    out.push_back(static_cast<uint8_t>(value >> 16));
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value));
}

inline uint32_t readU32(const std::vector<uint8_t>& bytes, size_t& offset) {
    if (offset + 4 > bytes.size()) throw CheckpointError("truncated checkpoint integer");
    uint32_t value = 0;
    for (size_t i = 0; i < 4; ++i) value = (value << 8) | bytes[offset++];
    return value;
}

inline void syncFile(const std::filesystem::path& path) {
#ifdef _WIN32
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE || !::FlushFileBuffers(file)) {
        if (file != INVALID_HANDLE_VALUE) ::CloseHandle(file);
        throw CheckpointError("cannot flush checkpoint file");
    }
    ::CloseHandle(file);
#else
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0 || ::fsync(fd) != 0) {
        if (fd >= 0) ::close(fd);
        throw CheckpointError("cannot flush checkpoint file");
    }
    ::close(fd);
#endif
}

inline void atomicReplace(const std::filesystem::path& temporary,
                          const std::filesystem::path& destination) {
#ifdef _WIN32
    if (!::MoveFileExW(temporary.c_str(), destination.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw CheckpointError("cannot atomically replace checkpoint");
#else
    if (::rename(temporary.c_str(), destination.c_str()) != 0)
        throw CheckpointError("cannot atomically replace checkpoint");
    const auto directory = destination.parent_path().empty() ? std::filesystem::path(".")
                                                              : destination.parent_path();
    int dir = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY);
    if (dir >= 0) {
        ::fsync(dir);
        ::close(dir);
    }
#endif
}

struct Snapshot {
    uint64_t searchFingerprint = 0;
    uint64_t generatedSeeds = 0;
    bool masterFinished = false;
    bool solutionFound = false;
    std::vector<Seed> pending;
};

// Crash-safe durable frontier. Each mutation rewrites a small snapshot to a
// sibling temporary file, flushes it, and atomically replaces the prior file.
// This favours correctness and simple recovery; batching/checkpoint cadence can
// be added later if profiling shows per-seed durability is too expensive.
class CoordinatorCheckpoint final : public MasterSeedPersistence {
public:
    explicit CoordinatorCheckpoint(std::filesystem::path path, uint64_t searchFingerprint = 0)
        : path_(std::move(path)), searchFingerprint_(searchFingerprint) {
        if (std::filesystem::exists(path_)) {
            load();
            if (searchFingerprint != 0 && searchFingerprint_ != searchFingerprint)
                throw CheckpointError("checkpoint belongs to another search");
        }
    }

    bool registerGeneratedSeed(uint64_t ordinal, Seed& seed) override {
        std::lock_guard<std::mutex> lock(mutex_);
        seed.id = ordinal;
        if (ordinal <= generatedSeeds_) return false;
        if (masterFinished_ || solutionFound_)
            throw CheckpointError("cannot extend a terminal checkpoint");
        if (ordinal != generatedSeeds_ + 1)
            throw CheckpointError("non-contiguous generated seed ordinal");
        generatedSeeds_ = ordinal;
        pending_[ordinal] = seed;
        persistLocked();
        return true;
    }

    void markCompleted(uint64_t seedId) {
        if (seedId == 0) return;
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_.erase(seedId)) persistLocked();
    }

    void markMasterFinished() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!masterFinished_) {
            masterFinished_ = true;
            persistLocked();
        }
    }

    void markSolutionFound() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!solutionFound_) {
            solutionFound_ = true;
            persistLocked();
        }
    }

    Snapshot snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        Snapshot result;
        result.searchFingerprint = searchFingerprint_;
        result.generatedSeeds = generatedSeeds_;
        result.masterFinished = masterFinished_;
        result.solutionFound = solutionFound_;
        for (const auto& item : pending_) result.pending.push_back(item.second);
        return result;
    }

private:
    static constexpr uint32_t MAGIC = 0x4c425943; // LBYC
    static constexpr uint32_t VERSION = 2;
    static constexpr uint32_t MAX_PENDING = 1000000;

    void persistLocked() {
        std::vector<uint8_t> payload;
        seedcodec::appendU64(payload, searchFingerprint_);
        seedcodec::appendU64(payload, generatedSeeds_);
        payload.push_back(masterFinished_ ? 1 : 0);
        payload.push_back(solutionFound_ ? 1 : 0);
        appendU32(payload, static_cast<uint32_t>(pending_.size()));
        for (const auto& item : pending_) {
            auto encoded = seedcodec::encode(item.second);
            payload.insert(payload.end(), encoded.begin(), encoded.end());
        }
        std::vector<uint8_t> file;
        appendU32(file, MAGIC);
        appendU32(file, VERSION);
        appendU32(file, static_cast<uint32_t>(payload.size()));
        appendU32(file, crc32(payload.data(), payload.size()));
        file.insert(file.end(), payload.begin(), payload.end());

        auto temporary = path_;
        temporary += ".tmp";
        {
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            if (!stream) throw CheckpointError("cannot create checkpoint temporary file");
            stream.write(reinterpret_cast<const char*>(file.data()), file.size());
            stream.flush();
            if (!stream) throw CheckpointError("cannot write checkpoint temporary file");
        }
        syncFile(temporary);
        atomicReplace(temporary, path_);
    }

    void load() {
        std::ifstream stream(path_, std::ios::binary);
        if (!stream) throw CheckpointError("cannot open checkpoint");
        std::vector<uint8_t> file((std::istreambuf_iterator<char>(stream)),
                                  std::istreambuf_iterator<char>());
        if (stream.bad() || file.size() < 16) throw CheckpointError("truncated checkpoint");
        size_t offset = 0;
        const uint32_t magic = readU32(file, offset);
        const uint32_t version = readU32(file, offset);
        const uint32_t length = readU32(file, offset);
        const uint32_t expectedCrc = readU32(file, offset);
        if (magic != MAGIC || version != VERSION) throw CheckpointError("incompatible checkpoint");
        if (length != file.size() - offset) throw CheckpointError("invalid checkpoint length");
        if (crc32(file.data() + offset, length) != expectedCrc)
            throw CheckpointError("checkpoint checksum mismatch");
        std::vector<uint8_t> payload(file.begin() + offset, file.end());
        size_t p = 0;
        searchFingerprint_ = seedcodec::readU64(payload, p);
        generatedSeeds_ = seedcodec::readU64(payload, p);
        if (p + 2 > payload.size()) throw CheckpointError("truncated checkpoint flags");
        masterFinished_ = payload[p++] != 0;
        solutionFound_ = payload[p++] != 0;
        const uint32_t count = readU32(payload, p);
        if (count > MAX_PENDING) throw CheckpointError("checkpoint pending count too large");
        if (payload.size() - p != static_cast<size_t>(count) * seedcodec::ENCODED_SEED_SIZE)
            throw CheckpointError("invalid checkpoint seed data");
        for (uint32_t i = 0; i < count; ++i) {
            std::vector<uint8_t> encoded(payload.begin() + p,
                                         payload.begin() + p + seedcodec::ENCODED_SEED_SIZE);
            p += seedcodec::ENCODED_SEED_SIZE;
            Seed seed = seedcodec::decode(encoded);
            if (seed.id == 0 || seed.id > generatedSeeds_ || pending_.count(seed.id))
                throw CheckpointError("invalid checkpoint seed id");
            pending_.emplace(seed.id, seed);
        }
    }

    std::filesystem::path path_;
    mutable std::mutex mutex_;
    uint64_t searchFingerprint_ = 0;
    uint64_t generatedSeeds_ = 0;
    bool masterFinished_ = false;
    bool solutionFound_ = false;
    std::map<uint64_t, Seed> pending_;
};

inline uint64_t searchFingerprint(const JobState& initial, uint8_t maxDepth, uint8_t seedDepth) {
    uint64_t hash = 1469598103934665603ull;
    const auto* bytes = reinterpret_cast<const uint8_t*>(&initial.board);
    for (size_t i = 0; i < sizeof(initial.board); ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    hash = (hash ^ maxDepth) * 1099511628211ull;
    hash = (hash ^ seedDepth) * 1099511628211ull;
    return hash;
}

// Use for workers running in the coordinator process. Remote completions are
// acknowledged by MetricsReceiver instead.
class CheckpointingInstrumentation final : public SearchInstrumentationSink {
public:
    CheckpointingInstrumentation(SearchInstrumentationSink& downstream,
                                 CoordinatorCheckpoint& checkpoint)
        : downstream_(downstream), checkpoint_(checkpoint) {}

    void recordExpansion(size_t parentDepth, uint64_t children, bool masterProducer) override {
        downstream_.recordExpansion(parentDepth, children, masterProducer);
    }

    void recordCompletedSeed(
        const std::array<uint64_t, INSTRUMENTED_DEPTHS>& jobsByDepth) override {
        downstream_.recordCompletedSeed(jobsByDepth);
    }

    void recordCompletedSeed(
        uint64_t seedId,
        const std::array<uint64_t, INSTRUMENTED_DEPTHS>& jobsByDepth) override {
        checkpoint_.markCompleted(seedId);
        downstream_.recordCompletedSeed(seedId, jobsByDepth);
    }

private:
    SearchInstrumentationSink& downstream_;
    CoordinatorCheckpoint& checkpoint_;
};

} // namespace durable
