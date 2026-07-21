#pragma once

#include "SeedQueue.hpp"
#include "SeedCodec.hpp"
#include "SearchInstrumentation.hpp"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <csignal>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "CoordinatorCheckpoint.hpp"

// Seed-level network transport. The coordinator performs moves 1..MASTER_DEPTH
// and sends compact, self-contained Seeds to remote GPU hosts. Each remote host
// feeds received seeds into its ordinary local SeedQueue and runs the existing
// Worker/Dispatcher pipeline without network traffic in the hot per-job path.
namespace remote {

#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle INVALID_SOCKET_HANDLE = INVALID_SOCKET;
inline int socketError() { return WSAGetLastError(); }
inline bool interruptedSocketError(int error) { return error == WSAEINTR; }
inline void closeSocket(SocketHandle socket) { ::closesocket(socket); }
class SocketRuntime {
public:
    SocketRuntime() {
        WSADATA data{};
        if (::WSAStartup(MAKEWORD(2, 2), &data) != 0) throw std::runtime_error("WSAStartup failed");
    }
    ~SocketRuntime() { ::WSACleanup(); }
};
inline void ensureSocketRuntime() { static SocketRuntime runtime; (void)runtime; }
#else
using SocketHandle = int;
constexpr SocketHandle INVALID_SOCKET_HANDLE = -1;
inline int socketError() { return errno; }
inline bool interruptedSocketError(int error) { return error == EINTR; }
inline void closeSocket(SocketHandle socket) { ::close(socket); }
inline void ensureSocketRuntime() {}
#endif

constexpr uint32_t WIRE_MAGIC = 0x4c425953; // "LBYS"
constexpr uint16_t WIRE_VERSION = 2;
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

struct HostCapabilities {
    uint32_t workerThreads = 1;
    uint32_t cudaDevices = 0;
    uint64_t cudaMemoryBytes = 0;
};

class TransportError : public std::runtime_error {
public:
    explicit TransportError(const std::string& what) : std::runtime_error(what) {}
};

inline uint64_t hostToNetwork64(uint64_t value) {
#if defined(_WIN32) || __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return (static_cast<uint64_t>(htonl(static_cast<uint32_t>(value))) << 32) |
           htonl(static_cast<uint32_t>(value >> 32));
#else
    return value;
#endif
}

inline uint64_t networkToHost64(uint64_t value) { return hostToNetwork64(value); }

inline std::vector<uint8_t> encodeHello(const HostCapabilities& capabilities) {
    if (capabilities.workerThreads == 0) throw TransportError("host needs a worker thread");
    std::vector<uint8_t> bytes(16);
    uint32_t workers = htonl(capabilities.workerThreads);
    uint32_t devices = htonl(capabilities.cudaDevices);
    uint64_t memory = hostToNetwork64(capabilities.cudaMemoryBytes);
    std::memcpy(bytes.data(), &workers, 4);
    std::memcpy(bytes.data() + 4, &devices, 4);
    std::memcpy(bytes.data() + 8, &memory, 8);
    return bytes;
}

inline HostCapabilities decodeHello(const std::vector<uint8_t>& bytes) {
    if (bytes.size() != 16) throw TransportError("invalid hello payload");
    uint32_t workers, devices;
    uint64_t memory;
    std::memcpy(&workers, bytes.data(), 4);
    std::memcpy(&devices, bytes.data() + 4, 4);
    std::memcpy(&memory, bytes.data() + 8, 8);
    HostCapabilities capabilities{ntohl(workers), ntohl(devices), networkToHost64(memory)};
    if (capabilities.workerThreads == 0) throw TransportError("host needs a worker thread");
    return capabilities;
}

inline void writeAll(SocketHandle fd, const void* data, size_t size) {
    const uint8_t* cursor = static_cast<const uint8_t*>(data);
    while (size) {
#ifdef _WIN32
        int written = ::send(fd, reinterpret_cast<const char*>(cursor), static_cast<int>(size), 0);
#else
        int written = static_cast<int>(::write(fd, cursor, size));
#endif
        const int error = socketError();
        if (written < 0 && interruptedSocketError(error)) continue;
        if (written <= 0)
            throw TransportError("remote socket write failed: " + std::to_string(error));
        cursor += written;
        size -= static_cast<size_t>(written);
    }
}

// Returns false only for an orderly disconnect before any byte was read.
inline bool readAll(SocketHandle fd, void* data, size_t size) {
    uint8_t* cursor = static_cast<uint8_t*>(data);
    bool started = false;
    while (size) {
#ifdef _WIN32
        int received = ::recv(fd, reinterpret_cast<char*>(cursor), static_cast<int>(size), 0);
#else
        int received = static_cast<int>(::read(fd, cursor, size));
#endif
        const int error = socketError();
        if (received < 0 && interruptedSocketError(error)) continue;
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
    explicit FramedSocket(SocketHandle fd, bool ownsFd = true) : fd_(fd), ownsFd_(ownsFd) {
        ensureSocketRuntime();
        if (fd == INVALID_SOCKET_HANDLE) throw TransportError("invalid remote socket");
#ifndef _WIN32
        static std::once_flag ignoreSigPipe;
        std::call_once(ignoreSigPipe, [] { std::signal(SIGPIPE, SIG_IGN); });
#endif
    }
    FramedSocket(const FramedSocket&) = delete;
    FramedSocket& operator=(const FramedSocket&) = delete;
    ~FramedSocket() {
        if (ownsFd_) closeSocket(fd_);
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

    SocketHandle fd() const { return fd_; }

private:
    SocketHandle fd_;
    bool ownsFd_;
    std::mutex sendMutex_;
};

inline void sendHello(FramedSocket& socket, const HostCapabilities& capabilities) {
    socket.send(MessageType::Hello, encodeHello(capabilities));
}

inline HostCapabilities receiveHello(FramedSocket& socket) {
    Message message;
    if (!socket.receive(message)) throw TransportError("worker disconnected before hello");
    if (message.type != MessageType::Hello) throw TransportError("worker did not send hello first");
    return decodeHello(message.payload);
}

inline SocketHandle connectTcp(const std::string& host, const std::string& port) {
    ensureSocketRuntime();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* addresses = nullptr;
    int status = ::getaddrinfo(host.c_str(), port.c_str(), &hints, &addresses);
    if (status != 0) throw TransportError(std::string("cannot resolve remote host: ") + gai_strerror(status));
    SocketHandle connected = INVALID_SOCKET_HANDLE;
    for (addrinfo* address = addresses; address; address = address->ai_next) {
        SocketHandle fd = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd == INVALID_SOCKET_HANDLE) continue;
        if (::connect(fd, address->ai_addr, address->ai_addrlen) == 0) {
            connected = fd;
            break;
        }
        closeSocket(fd);
    }
    ::freeaddrinfo(addresses);
    if (connected == INVALID_SOCKET_HANDLE) throw TransportError("cannot connect to remote worker host");
    return connected;
}

inline SocketHandle listenTcp(const std::string& port, int backlog = 16) {
    ensureSocketRuntime();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    addrinfo* addresses = nullptr;
    int status = ::getaddrinfo(nullptr, port.c_str(), &hints, &addresses);
    if (status != 0) throw TransportError(std::string("cannot resolve listen address: ") + gai_strerror(status));
    SocketHandle listener = INVALID_SOCKET_HANDLE;
    for (addrinfo* address = addresses; address; address = address->ai_next) {
        SocketHandle fd = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd == INVALID_SOCKET_HANDLE) continue;
        int reuse = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
        if (::bind(fd, address->ai_addr, address->ai_addrlen) == 0 && ::listen(fd, backlog) == 0) {
            listener = fd;
            break;
        }
        closeSocket(fd);
    }
    ::freeaddrinfo(addresses);
    if (listener == INVALID_SOCKET_HANDLE) throw TransportError("cannot listen for remote worker hosts");
    return listener;
}

inline SocketHandle acceptTcp(SocketHandle listener) {
    for (;;) {
        SocketHandle fd = ::accept(listener, nullptr, nullptr);
        if (fd != INVALID_SOCKET_HANDLE) return fd;
        if (!interruptedSocketError(socketError()))
            throw TransportError("failed to accept remote worker host");
    }
}

inline std::vector<uint8_t> encodeSeed(const Seed& seed) {
    return seedcodec::encode(seed);
}

inline Seed decodeSeed(const std::vector<uint8_t>& bytes) {
    return seedcodec::decode(bytes);
}

inline std::vector<uint8_t> encodeSeedMetrics(
    uint64_t seedId, const std::array<uint64_t, INSTRUMENTED_DEPTHS>& jobsByDepth) {
    std::vector<uint8_t> bytes((INSTRUMENTED_DEPTHS + 1) * sizeof(uint64_t));
    uint64_t wireId = hostToNetwork64(seedId);
    std::memcpy(bytes.data(), &wireId, sizeof(wireId));
    for (size_t i = 0; i < INSTRUMENTED_DEPTHS; ++i) {
        uint64_t wire = hostToNetwork64(jobsByDepth[i]);
        std::memcpy(bytes.data() + (i + 1) * sizeof(uint64_t), &wire, sizeof(wire));
    }
    return bytes;
}

struct SeedMetrics {
    uint64_t seedId = 0;
    std::array<uint64_t, INSTRUMENTED_DEPTHS> jobs{};
};

inline SeedMetrics
decodeSeedMetrics(const std::vector<uint8_t>& bytes) {
    if (bytes.size() != (INSTRUMENTED_DEPTHS + 1) * sizeof(uint64_t))
        throw TransportError("invalid seed metrics payload size");
    SeedMetrics metrics;
    uint64_t wireId;
    std::memcpy(&wireId, bytes.data(), sizeof(wireId));
    metrics.seedId = networkToHost64(wireId);
    for (size_t i = 0; i < INSTRUMENTED_DEPTHS; ++i) {
        uint64_t wire;
        std::memcpy(&wire, bytes.data() + (i + 1) * sizeof(uint64_t), sizeof(wire));
        metrics.jobs[i] = networkToHost64(wire);
    }
    return metrics;
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
        recordCompletedSeed(0, jobsByDepth);
    }

    void recordCompletedSeed(
        uint64_t seedId,
        const std::array<uint64_t, INSTRUMENTED_DEPTHS>& jobsByDepth) override {
        coordinator_->send(MessageType::SeedMetrics, encodeSeedMetrics(seedId, jobsByDepth));
    }

    void finished() { coordinator_->send(MessageType::HostFinished); }

private:
    std::shared_ptr<FramedSocket> coordinator_;
};

// One instance runs on the coordinator for each bidirectional host socket.
// SeedDistributor is the writer; this is the sole reader.
class MetricsReceiver {
public:
    explicit MetricsReceiver(std::shared_ptr<FramedSocket> host,
                             durable::CoordinatorCheckpoint* checkpoint = nullptr)
        : host_(std::move(host)), checkpoint_(checkpoint) {}

    uint64_t run(SearchInstrumentation& instrumentation) {
        uint64_t samples = 0;
        Message message;
        while (host_->receive(message)) {
            if (message.type == MessageType::SeedMetrics) {
                SeedMetrics metrics = decodeSeedMetrics(message.payload);
                if (checkpoint_) checkpoint_->markCompleted(metrics.seedId);
                instrumentation.recordCompletedSeed(metrics.seedId, metrics.jobs);
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
    durable::CoordinatorCheckpoint* checkpoint_;
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
