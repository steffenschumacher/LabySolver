#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <type_traits>

namespace laby {

constexpr uint8_t BOARD_WIDTH = 5;
constexpr uint8_t BOARD_HEIGHT = 7;
constexpr uint8_t BOARD_CELLS = BOARD_WIDTH * BOARD_HEIGHT;
constexpr uint8_t SPARE_POSITION = BOARD_CELLS;
constexpr uint8_t UNUSED_GOAL_POSITION = BOARD_CELLS + 1;
constexpr uint8_t POSITION_COUNT = 5; // ladybug plus four ordered goals

enum Opening : uint8_t {
    North = 0x1,
    East = 0x2,
    South = 0x4,
    West = 0x8,
};

constexpr uint8_t rotateClockwise(uint8_t mask) {
    mask &= 0x0f;
    return static_cast<uint8_t>(((mask << 1) & 0x0f) | ((mask >> 3) & 0x01));
}

constexpr uint8_t cellIndex(uint8_t x, uint8_t y) {
    return static_cast<uint8_t>(y * BOARD_WIDTH + x);
}

// Canonical CPU search state. Its byte representation is explicit and stable:
// 18 bytes of tile nibbles, 30 packed occupant bits, and one metadata byte.
// Scheduler ownership, parentage, level rules, and transient CUDA output do
// not belong here.
struct CompactBoardState {
    std::array<uint8_t, 18> tiles;
    std::array<uint8_t, 4> positions;
    uint8_t progressAndDepth;

    uint8_t tile(uint8_t index) const {
        if (index > SPARE_POSITION) throw std::out_of_range("tile index");
        const uint8_t byte = tiles[index / 2];
        return static_cast<uint8_t>((index & 1) ? byte >> 4 : byte & 0x0f);
    }

    void setTile(uint8_t index, uint8_t mask) {
        if (index > SPARE_POSITION || mask > 0x0f) throw std::out_of_range("tile value");
        uint8_t& byte = tiles[index / 2];
        if (index & 1)
            byte = static_cast<uint8_t>((byte & 0x0f) | (mask << 4));
        else
            byte = static_cast<uint8_t>((byte & 0xf0) | mask);
    }

    uint8_t position(uint8_t slot) const {
        if (slot >= POSITION_COUNT) throw std::out_of_range("position slot");
        uint32_t packed = 0;
        for (size_t i = 0; i < positions.size(); ++i)
            packed |= static_cast<uint32_t>(positions[i]) << (8 * i);
        return static_cast<uint8_t>((packed >> (6 * slot)) & 0x3f);
    }

    void setPosition(uint8_t slot, uint8_t value) {
        if (slot >= POSITION_COUNT || value > UNUSED_GOAL_POSITION)
            throw std::out_of_range("position value");
        uint32_t packed = 0;
        for (size_t i = 0; i < positions.size(); ++i)
            packed |= static_cast<uint32_t>(positions[i]) << (8 * i);
        const uint32_t shift = 6 * slot;
        packed = (packed & ~(uint32_t{0x3f} << shift)) | (static_cast<uint32_t>(value) << shift);
        for (size_t i = 0; i < positions.size(); ++i)
            positions[i] = static_cast<uint8_t>(packed >> (8 * i));
    }

    uint8_t ladybug() const { return position(0); }
    void setLadybug(uint8_t value) { setPosition(0, value); }
    uint8_t goal(uint8_t index) const { return position(static_cast<uint8_t>(index + 1)); }
    void setGoal(uint8_t index, uint8_t value) { setPosition(static_cast<uint8_t>(index + 1), value); }

    uint8_t nextGoal() const { return progressAndDepth & 0x07; }
    void setNextGoal(uint8_t value) {
        if (value > 4) throw std::out_of_range("next goal");
        progressAndDepth = static_cast<uint8_t>((progressAndDepth & 0xf8) | value);
    }
    uint8_t depth() const { return static_cast<uint8_t>((progressAndDepth >> 3) & 0x07); }
    void setDepth(uint8_t value) {
        if (value > 7) throw std::out_of_range("depth");
        progressAndDepth = static_cast<uint8_t>((progressAndDepth & 0xc7) | (value << 3));
    }

    uint8_t goalCount() const {
        uint8_t count = 0;
        while (count < 4 && goal(count) != UNUSED_GOAL_POSITION) ++count;
        return count;
    }
    bool won() const { return nextGoal() == goalCount(); }

    friend bool operator==(const CompactBoardState& a, const CompactBoardState& b) {
        return a.tiles == b.tiles && a.positions == b.positions &&
               a.progressAndDepth == b.progressAndDepth;
    }
    friend bool operator!=(const CompactBoardState& a, const CompactBoardState& b) { return !(a == b); }
};

static_assert(sizeof(CompactBoardState) == 23, "compact state must remain 23 bytes");
static_assert(std::is_trivially_copyable_v<CompactBoardState>);
static_assert(std::has_unique_object_representations_v<CompactBoardState>);

} // namespace laby
