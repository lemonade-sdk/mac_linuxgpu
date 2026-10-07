// Client stores into the GPU's BARs on the submission path (hdp_flush.h):
// kernel arguments written into CPU-visible VRAM and published with the
// HDP flush register, as HRX does on Linux, bracketed under the process's
// gate so no store reaches a GPU the driver is about to stop answering.
#include "runtime_state.h"
#include "mac_hsa.h"
#include "../../dext/sources/doorbell_gate.h"
#include <string>

// One per GPU connection, made by the first enable and never freed: a
// submitter may still hold the pointer when the runtime shuts down, and a
// bracket on it then only finds the gate closed for good.
struct mac_hsa_bar_writer_s {
    mlg_doorbell_gate *gate = nullptr;
    std::shared_ptr<mac_hsa::Connection> connection;
};

namespace mac_hsa::detail {
namespace {
std::mutex &writersMutex = *new std::mutex;
std::vector<mac_hsa_bar_writer_s *> &writers = *new std::vector<mac_hsa_bar_writer_s *>;
}
} // namespace mac_hsa::detail

using namespace mac_hsa::detail;
extern "C" {
HSA_API_EXPORT hsa_status_t mac_hsa_bar_writes_enable(hsa_agent_t handle, mac_hsa_bar_writer_t **out) {
    if (!out) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    *out = nullptr;
    std::shared_ptr<mac_hsa::Connection> connection;
    {
        std::lock_guard lock(runtimeMutex);
        if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
        const auto agent = findAgent(handle);
        if (!agent) return HSA_STATUS_ERROR_INVALID_AGENT;
        if (!agent->connection) return HSA_STATUS_ERROR_INVALID_AGENT;
        connection = agent->connection;
    }
    mac_hsa::BarWrites bar;
    std::string why;
    const auto status = connection->enableBarWrites(bar, &why);
    if (status != HSA_STATUS_SUCCESS || !bar.gate) {
        std::fprintf(stderr, "mac_hsa: BAR writes (kernel arguments in VRAM, HDP flush) are not available: %s\n",
                     why.empty() ? "the driver declined" : why.c_str());
        return status != HSA_STATUS_SUCCESS ? status : HSA_STATUS_ERROR;
    }
    try {
        std::lock_guard lock(writersMutex);
        for (auto *writer : writers)
            if (writer->connection == connection) { *out = writer; return HSA_STATUS_SUCCESS; }
        auto *writer = new mac_hsa_bar_writer_s{bar.gate, connection};
        writers.push_back(writer);
        *out = writer;
        return HSA_STATUS_SUCCESS;
    } catch (const std::bad_alloc &) { return HSA_STATUS_ERROR_OUT_OF_RESOURCES; }
}

HSA_API_EXPORT hsa_status_t mac_hsa_bar_write_begin(mac_hsa_bar_writer_t *writer) {
    if (!writer || !writer->gate) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    for (unsigned attempt = 0;; ++attempt) {
        if (mlg_bar_write_begin(writer->gate)) return HSA_STATUS_SUCCESS;
        // Closed: held for a power transition or a reset (wait for it to
        // open), or retired with the device (every BAR mapping is retired
        // before this returns, so a store that still follows lands in host
        // memory, and the caller's submission fails at its doorbell).
        const auto status = writer->connection->barWriteWait(attempt);
        if (status != HSA_STATUS_SUCCESS) return status;
    }
}

HSA_API_EXPORT void mac_hsa_bar_write_end(mac_hsa_bar_writer_t *writer) {
    if (writer && writer->gate) mlg_bar_write_end(writer->gate);
}
} // extern "C"
