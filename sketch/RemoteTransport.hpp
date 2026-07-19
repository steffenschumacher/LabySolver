#pragma once

#include "SeedQueue.hpp"
#include "SearchInstrumentation.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <netdb.h>
#include <netinet/in.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

// Seed-level network transport. The coordinator performs moves 1..MASTER_DEPTH
// and sends compact, self-contained Seeds to remote GPU hosts. Each remote host
// feeds received seeds into its ordinary local SeedQueue and runs the existing
// Worker/Dispatcher pipeline without network traffic in the hot per-job path.
namespace remote {

constexpr uint32_t WIRE_MAGIC = 0x4c425953; // "LBYS"
constexpr uint16_t WIRE_VERSION = 1;
constexpr uint32_t MAX_WIRE_PAYLOAD = 64 * 1024;

enum class MessageType : uint16_t {
    Seed = 1,
    Finished = 2,
    Abort = 3,
    SolutionFound = 4,
    Hello = 5,
    SeedMetrics = 6,
    HostFinished = 7,
};

struct Message {
    MessageType type;
    std::vector<uint8_t> payload;
};

class TransportError : public std::runtime_error {
public:
    explicit TransportError(const std::string& what) : std::runtime_error(what) {}
};

inline uint64_t hostToNetwork64(uint64_t value) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return (static_cast<uint64_t>(htonl(static_cast<uint32_t>(value))) << 32) |
           htonl(static_cast<uint32_t>(value >> 32));
#else
    return value;
#endif
}

inline uint64_t networkToHost64(uint64_t value) { return hostToNetwork64(value); }

inline void writeAll(int fd, const void* data, size_t size) {
    const uint8_t* cursor = static_cast<const uint8_t*>(data);
    while (size) {
        ssize_t written = ::write(fd, cursor, size);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0)
            throw TransportError(std::string("remote socket write failed: ") + std::strerror(errno));
        cursor += written;
        size -= static_cast<size_t>(written);
    }
}

// Returns false only for an orderly disconnect before any byte was read.
inline bool readAll(int fd, void* data, size_t size) {
    uint8_t* cursor = static_cast<uint8_t*>(data);
    bool started = false;
    while (size) {
        ssize_t received = ::read(fd, cursor, size);
        if (received < 0 && errno == EINTR) continue;
        if (received == 0 && !started) return false;
        if (received <= 0) throw TransportError("remote socket disconnected mid-message");
        started = true;
        cursor += received;
        size -= static_cast<size_t>(received);
    }
    return true;
}

// Thread-safe sends allow a coordinator's seed distributor and abort monitor
// to share a connection. Exactly one thread should receive from a connection.
class FramedSocket {
public:
    explicit FramedSocket(int fd, bool ownsFd = true) : fd_(fd), ownsFd_(ownsFd) {
        if (fd < 0) throw TransportError("invalid remote socket");
        static std::once_flag ignoreSigPipe;
        std::call_once(ignoreSigPipe, [] { std::signal(SIGPIPE, SIG_IGN); });
    }
    FramedSocket(const FramedSocket&) = delete;
    FramedSocket& operator=(const FramedSocket&) = delete;
    ~FramedSocket() {
        if (ownsFd_) ::close(fd_);
    }

    void send(MessageType type, const std::vector<uint8_t>& payload = {}) {
        if (payload.size() > MAX_WIRE_PAYLOAD) throw TransportError("remote payload too large");
        uint8_t header[12];
        uint32_t magic = htonl(WIRE_MAGIC);
        uint16_t version = htons(WIRE_VERSION);
        uint16_t wireType = htons(static_cast<uint16_t>(type));
        uint32_t length = htonl(static_cast<uint32_t>(payload.size()));
        std::memcpy(header, &magic, 4);
        std::memcpy(header + 4, &version, 2);
        std::memcpy(header + 6, &wireType, 2);
        std::memcpy(header + 8, &length, 4);
        std::lock_guard<std::mutex> lock(sendMutex_);
        writeAll(fd_, header, sizeof(header));
        if (!payload.empty()) writeAll(fd_, payload.data(), payload.size());
    }

    bool receive(Message& message) {
        uint8_t header[12];
        if (!readAll(fd_, header, sizeof(header))) return false;
        uint32_t magic, length;
        uint16_t version, type;
        std::memcpy(&magic, header, 4);
        std::memcpy(&version, header + 4, 2);
        std::memcpy(&type, header + 6, 2);
        std::memcpy(&length, header + 8, 4);
        magic = ntohl(magic);
        version = ntohs(version);
        type = ntohs(type);
        length = ntohl(length);
        if (magic != WIRE_MAGIC) throw TransportError("invalid remote protocol magic");
        if (version != WIRE_VERSION) throw TransportError("incompatible remote protocol version");
        if (length > MAX_WIRE_PAYLOAD) throw TransportError("invalid remote payload length");
        if (type < static_cast<uint16_t>(MessageType::Seed) ||
            type > static_cast<uint16_t>(MessageType::HostFinished))
            throw TransportError("unknown remote message type");
        message.type = static_cast<MessageType>(type);
        message.payload.resize(length);
        if (length && !readAll(fd_, message.payload.data(), length))
            throw TransportError("remote socket disconnected before payload");
        return true;
    }

    int fd() const { return fd_; }

private:
    int fd_;
    bool ownsFd_;
    std::mutex sendMutex_;
};

inline int connectTcp(const std::string& host, const std::string& port) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* addresses = nullptr;
    int status = ::getaddrinfo(host.c_str(), port.c_str(), &hints, &addresses);
    if (status != 0) throw TransportError(std::string("cannot resolve remote host: ") + gai_strerror(status));
    int connected = -1;
    for (addrinfo* address = addresses; address; address = address->ai_next) {
        int fd = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd < 0) continue;
        if (::connect(fd, address->ai_addr, address->ai_addrlen) == 0) {
            connected = fd;
            break;
        }
        ::close(fd);
    }
    ::freeaddrinfo(addresses);
    if (connected < 0) throw TransportError("cannot connect to remote worker host");
    return connected;
}

inline int listenTcp(const std::string& port, int backlog = 16) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    addrinfo* addresses = nullptr;
    int status = ::getaddrinfo(nullptr, port.c_str(), &hints, &addresses);
    if (status != 0) throw TransportError(std::string("cannot resolve listen address: ") + gai_strerror(status));
    int listener = -1;
    for (addrinfo* address = addresses; address; address = address->ai_next) {
        int fd = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd < 0) continue;
        int reuse = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        if (::bind(fd, address->ai_addr, address->ai_addrlen) == 0 && ::listen(fd, backlog) == 0) {
            listener = fd;
            break;
        }
        ::close(fd);
    }
    ::freeaddrinfo(addresses);
    if (listener < 0) throw TransportError("cannot listen for remote worker hosts");
    return listener;
}

inline int acceptTcp(int listener) {
    for (;;) {
        int fd = ::accept(listener, nullptr, nullptr);
        if (fd >= 0) return fd;
        if (errno != EINTR) throw TransportError("failed to accept remote worker host");
    }
}

// Version 1 intentionally transfers JobState as raw bytes. Coordinator and
// remote binaries must therefore use the same build/JobState ABI. The framing
// remains versioned so a future portable board encoding can replace this.
inline std::vector<uint8_t> encodeSeed(const Seed& seed) {
    constexpr size_t SIZE = sizeof(JobState) + 1 + SEED_MAX_MOVES * 2;
    std::vector<uint8_t> bytes(SIZE);
    size_t offset = 0;
    std::memcpy(bytes.data(), &seed.state, sizeof(JobState));
    offset += sizeof(JobState);
    bytes[offset++] = seed.depth;
    for (size_t i = 0; i < SEED_MAX_MOVES; ++i) {
        bytes[offset++] = seed.moves[i].insertPoint;
        bytes[offset++] = seed.moves[i].orientation;
    }
    return bytes;
}

inline Seed decodeSeed(const std::vector<uint8_t>& bytes) {
    constexpr size_t SIZE = sizeof(JobState) + 1 + SEED_MAX_MOVES * 2;
    if (bytes.size() != SIZE) throw TransportError("invalid seed payload size");
    Seed seed{};
    size_t offset = 0;
    std::memcpy(&seed.state, bytes.data(), sizeof(JobState));
    offset += sizeof(JobState);
    seed.depth = bytes[offset++];
    if (seed.depth > SEED_MAX_MOVES) throw TransportError("invalid seed depth");
    for (size_t i = 0; i < SEED_MAX_MOVES; ++i) {
        seed.moves[i].insertPoint = bytes[offset++];
        seed.moves[i].orientation = bytes[offset++];
    }
    return seed;
}

inline std::vector<uint8_t> encodeSeedMetrics(
    const std::array<uint64_t, INSTRUMENTED_DEPTHS>& jobsByDepth) {
    std::vector<uint8_t> bytes(INSTRUMENTED_DEPTHS * sizeof(uint64_t));
    for (size_t i = 0; i < INSTRUMENTED_DEPTHS; ++i) {
        uint64_t wire = hostToNetwork64(jobsByDepth[i]);
        std::memcpy(bytes.data() + i * sizeof(uint64_t), &wire, sizeof(wire));
    }
    return bytes;
}

inline std::array<uint64_t, INSTRUMENTED_DEPTHS>
decodeSeedMetrics(const std::vector<uint8_t>& bytes) {
    if (bytes.size() != INSTRUMENTED_DEPTHS * sizeof(uint64_t))
        throw TransportError("invalid seed metrics payload size");
    std::array<uint64_t, INSTRUMENTED_DEPTHS> jobs{};
    for (size_t i = 0; i < INSTRUMENTED_DEPTHS; ++i) {
        uint64_t wire;
        std::memcpy(&wire, bytes.data() + i * sizeof(uint64_t), sizeof(wire));
        jobs[i] = networkToHost64(wire);
    }
    return jobs;
}

// Installed on a remote host's Worker objects. Per-expansion events remain
// local; only one aggregate record per completed seed crosses the network.
class RemoteMetricsSink : public SearchInstrumentationSink {
public:
    explicit RemoteMetricsSink(std::shared_ptr<FramedSocket> coordinator)
        : coordinator_(std::move(coordinator)) {}

    void recordExpansion(size_t, uint64_t, bool) override {}

    void recordCompletedSeed(
        const std::array<uint64_t, INSTRUMENTED_DEPTHS>& jobsByDepth) override {
        coordinator_->send(MessageType::SeedMetrics, encodeSeedMetrics(jobsByDepth));
    }

    void finished() { coordinator_->send(MessageType::HostFinished); }

private:
    std::shared_ptr<FramedSocket> coordinator_;
};

// One instance runs on the coordinator for each bidirectional host socket.
// SeedDistributor is the writer; this is the sole reader.
class MetricsReceiver {
public:
    explicit MetricsReceiver(std::shared_ptr<FramedSocket> host) : host_(std::move(host)) {}

    uint64_t run(SearchInstrumentation& instrumentation) {
        uint64_t samples = 0;
        Message message;
        while (host_->receive(message)) {
            if (message.type == MessageType::SeedMetrics) {
                instrumentation.recordCompletedSeed(decodeSeedMetrics(message.payload));
                ++samples;
            } else if (message.type == MessageType::HostFinished) {
                return samples;
            } else if (message.type == MessageType::SolutionFound) {
                // The solution value-type is not defined yet; leave handling
                // to the supervisor once SearchGlobals is made serializable.
                continue;
            } else {
                throw TransportError("unexpected remote-host control message");
            }
        }
        throw TransportError("remote host disconnected before HostFinished");
    }

private:
    std::shared_ptr<FramedSocket> host_;
};

// Runs on the coordinator. SeedQueue backpressure propagates through blocking
// socket writes when remote hosts cannot consume quickly enough.
class SeedDistributor {
public:
    struct Peer {
        std::shared_ptr<FramedSocket> socket;
        size_t workerThreads = 1;
    };

    explicit SeedDistributor(std::vector<std::shared_ptr<FramedSocket>> peers)
    {
        for (auto& peer : peers) addPeer({std::move(peer), 1});
        validate();
    }

    explicit SeedDistributor(std::vector<Peer> peers) {
        for (auto& peer : peers) addPeer(std::move(peer));
        validate();
    }

private:
    void addPeer(Peer peer) {
        if (!peer.socket || peer.workerThreads == 0)
            throw TransportError("remote peer must advertise at least one worker thread");
        const size_t id = peers_.size();
        peers_.push_back(std::move(peer.socket));
        // Repeating the peer in the schedule gives it one initial seed per
        // worker thread and proportional subsequent prefetch.
        for (size_t i = 0; i < peer.workerThreads; ++i) schedule_.push_back(id);
    }

    void validate() {
        if (peers_.empty()) throw TransportError("remote distributor needs at least one peer");
    }

public:

    uint64_t run(SeedQueue<32>& source) {
        uint64_t sent = 0;
        Seed seed;
        while (source.pop(seed)) {
            peers_[schedule_[next_]]->send(MessageType::Seed, encodeSeed(seed));
            next_ = (next_ + 1) % schedule_.size();
            ++sent;
        }
        broadcast(MessageType::Finished);
        return sent;
    }

    void abort() { broadcast(MessageType::Abort); }

private:
    void broadcast(MessageType type) {
        for (const auto& peer : peers_) peer->send(type);
    }
    std::vector<std::shared_ptr<FramedSocket>> peers_;
    std::vector<size_t> schedule_;
    size_t next_ = 0;
};

// Runs on each remote host and bridges network messages into the unchanged
// local worker queue.
class SeedReceiver {
public:
    explicit SeedReceiver(std::shared_ptr<FramedSocket> coordinator)
        : coordinator_(std::move(coordinator)) {}

    uint64_t run(SeedQueue<32>& destination) {
        uint64_t received = 0;
        Message message;
        try {
            while (coordinator_->receive(message)) {
                switch (message.type) {
                case MessageType::Seed:
                    if (!destination.push(decodeSeed(message.payload))) return received;
                    ++received;
                    break;
                case MessageType::Finished:
                    destination.finished();
                    return received;
                case MessageType::Abort:
                    destination.abort();
                    return received;
                default:
                    throw TransportError("unexpected coordinator message");
                }
            }
        } catch (...) {
            destination.abort();
            throw;
        }
        destination.abort(); // disconnect is failure/abort, never normal finish
        return received;
    }

    void reportSolution(const std::vector<uint8_t>& solutionPayload) {
        coordinator_->send(MessageType::SolutionFound, solutionPayload);
    }

private:
    std::shared_ptr<FramedSocket> coordinator_;
};

} // namespace remote
