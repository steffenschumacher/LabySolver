#pragma once

#include "SeedQueue.hpp"

#include <cstdint>
#include <stdexcept>
#include <vector>

namespace seedcodec {

constexpr size_t ENCODED_SEED_SIZE = 8 + 23 + 5 + 1 + SEED_MAX_MOVES * 2;

class Error : public std::runtime_error {
public:
    explicit Error(const char* message) : std::runtime_error(message) {}
};

inline void appendU64(std::vector<uint8_t>& out, uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8)
        out.push_back(static_cast<uint8_t>(value >> shift));
}

inline uint64_t readU64(const std::vector<uint8_t>& bytes, size_t& offset) {
    if (offset + 8 > bytes.size()) throw Error("truncated uint64");
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i) value = (value << 8) | bytes[offset++];
    return value;
}

inline std::vector<uint8_t> encode(const Seed& seed) {
    if (seed.depth > 7 || seed.depth > SEED_MAX_MOVES) throw Error("invalid seed depth");
    std::vector<uint8_t> out;
    out.reserve(ENCODED_SEED_SIZE);
    appendU64(out, seed.id);
    const auto* board = reinterpret_cast<const uint8_t*>(&seed.state.board);
    out.insert(out.end(), board, board + sizeof(laby::CompactBoardState));
    out.push_back(seed.state.insertPoint);
    out.push_back(seed.state.orientation);
    out.push_back(seed.state.reachableMask);
    out.push_back(seed.state.bugsEatenMask);
    out.push_back(seed.state.offBoard);
    out.push_back(seed.depth);
    for (size_t i = 0; i < SEED_MAX_MOVES; ++i) {
        out.push_back(seed.moves[i].insertPoint);
        out.push_back(seed.moves[i].orientation);
    }
    return out;
}

inline Seed decode(const std::vector<uint8_t>& bytes) {
    if (bytes.size() != ENCODED_SEED_SIZE) throw Error("invalid seed payload size");
    Seed seed{};
    size_t offset = 0;
    seed.id = readU64(bytes, offset);
    auto* board = reinterpret_cast<uint8_t*>(&seed.state.board);
    for (size_t i = 0; i < sizeof(laby::CompactBoardState); ++i) board[i] = bytes[offset++];
    seed.state.insertPoint = bytes[offset++];
    seed.state.orientation = bytes[offset++];
    seed.state.reachableMask = bytes[offset++];
    seed.state.bugsEatenMask = bytes[offset++];
    seed.state.offBoard = bytes[offset++];
    seed.depth = bytes[offset++];
    if (seed.depth > 7 || seed.depth > SEED_MAX_MOVES) throw Error("invalid seed depth");
    for (size_t i = 0; i < SEED_MAX_MOVES; ++i) {
        seed.moves[i].insertPoint = bytes[offset++];
        seed.moves[i].orientation = bytes[offset++];
        if (seed.moves[i].orientation > 3) throw Error("invalid seed orientation");
    }
    return seed;
}

} // namespace seedcodec
