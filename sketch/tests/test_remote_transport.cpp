#include "../RemoteTransport.hpp"
#include "test_util.hpp"

#include <array>
#include <cstring>
#include <memory>
#include <sys/socket.h>
#include <thread>

static std::array<int, 2> socketPair() {
    std::array<int, 2> fds{};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()) != 0) std::abort();
    return fds;
}

static Seed makeSeed(uint8_t id) {
    Seed seed{};
    seed.state.boardBytes[0] = id;
    seed.state.insertPoint = static_cast<uint8_t>(id + 1);
    seed.depth = 4;
    for (size_t i = 0; i < seed.depth; ++i) {
        seed.moves[i].insertPoint = static_cast<uint8_t>(id + i);
        seed.moves[i].orientation = static_cast<uint8_t>(i % 4);
    }
    return seed;
}

int main() {
    // Codec preserves the complete self-contained seed.
    Seed original = makeSeed(7);
    Seed decoded = remote::decodeSeed(remote::encodeSeed(original));
    CHECK(std::memcmp(&original.state, &decoded.state, sizeof(JobState)) == 0);
    CHECK(decoded.depth == original.depth);
    CHECK(std::memcmp(original.moves, decoded.moves, sizeof(original.moves)) == 0);

    // Round-robin distribution to two independent remote queues, including
    // orderly Finished propagation.
    auto pairA = socketPair();
    auto pairB = socketPair();
    auto coordinatorA = std::make_shared<remote::FramedSocket>(pairA[0]);
    auto remoteA = std::make_shared<remote::FramedSocket>(pairA[1]);
    auto coordinatorB = std::make_shared<remote::FramedSocket>(pairB[0]);
    auto remoteB = std::make_shared<remote::FramedSocket>(pairB[1]);

    SeedQueue<32> source, destinationA, destinationB;
    for (uint8_t i = 0; i < 10; ++i) source.push(makeSeed(i));
    source.finished();

    remote::SeedDistributor distributor({coordinatorA, coordinatorB});
    remote::SeedReceiver receiverA(remoteA), receiverB(remoteB);
    uint64_t sent = 0, receivedA = 0, receivedB = 0;
    std::thread sender([&] { sent = distributor.run(source); });
    std::thread readerA([&] { receivedA = receiverA.run(destinationA); });
    std::thread readerB([&] { receivedB = receiverB.run(destinationB); });
    sender.join();
    readerA.join();
    readerB.join();

    CHECK(sent == 10);
    CHECK(receivedA == 5);
    CHECK(receivedB == 5);
    for (uint8_t expected : {0, 2, 4, 6, 8}) {
        Seed seed;
        CHECK(destinationA.pop(seed));
        CHECK(seed.state.boardBytes[0] == expected);
    }
    for (uint8_t expected : {1, 3, 5, 7, 9}) {
        Seed seed;
        CHECK(destinationB.pop(seed));
        CHECK(seed.state.boardBytes[0] == expected);
    }
    Seed none;
    CHECK(!destinationA.pop(none));
    CHECK(!destinationB.pop(none));

    // Abort control messages wake a remote queue rather than masquerading as
    // successful exhaustion.
    auto abortPair = socketPair();
    auto abortCoordinator = std::make_shared<remote::FramedSocket>(abortPair[0]);
    auto abortRemote = std::make_shared<remote::FramedSocket>(abortPair[1]);
    SeedQueue<32> abortQueue;
    remote::SeedReceiver abortReceiver(abortRemote);
    std::thread abortReader([&] { abortReceiver.run(abortQueue); });
    abortCoordinator->send(remote::MessageType::Abort);
    abortReader.join();
    CHECK(!abortQueue.pop(none));

    REPORT();
}
