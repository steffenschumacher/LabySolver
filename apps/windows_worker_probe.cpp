#include "RemoteTransport.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#ifdef LABY_WITH_CUDA
#include <cuda_runtime_api.h>
#endif

int main(int argc, char** argv) {
    if (argc < 3 || argc > 4) {
        std::cerr << "usage: laby_worker_probe <coordinator-host> <port> [worker-threads]\n";
        return 2;
    }
    try {
        remote::HostCapabilities capabilities;
        capabilities.workerThreads = argc == 4 ? static_cast<uint32_t>(std::stoul(argv[3]))
                                                : std::max(1u, std::thread::hardware_concurrency());
#ifdef LABY_WITH_CUDA
        int devices = 0;
        if (cudaGetDeviceCount(&devices) == cudaSuccess) {
            capabilities.cudaDevices = static_cast<uint32_t>(devices);
            for (int device = 0; device < devices; ++device) {
                cudaDeviceProp properties{};
                if (cudaGetDeviceProperties(&properties, device) == cudaSuccess)
                    capabilities.cudaMemoryBytes += properties.totalGlobalMem;
            }
        }
#endif
        auto socket = std::make_shared<remote::FramedSocket>(remote::connectTcp(argv[1], argv[2]));
        remote::sendHello(*socket, capabilities);
        std::cout << "connected using protocol " << remote::WIRE_VERSION << "; workers="
                  << capabilities.workerThreads << "; CUDA devices=" << capabilities.cudaDevices
                  << "; CUDA bytes=" << capabilities.cudaMemoryBytes << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "worker probe failed: " << error.what() << '\n';
        return 1;
    }
}
