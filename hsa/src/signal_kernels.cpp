#include "signal_kernels.h"
#include "code_object.h"
#include "signal_kernels_code.h"
#include <array>

namespace mac_hsa {
namespace {
constexpr auto kTargets = [] {
    std::array<const char *, std::size(signal_kernels::kCodeObjects)> names{};
    for (size_t i = 0; i < names.size(); ++i) names[i] = signal_kernels::kCodeObjects[i].target;
    return names;
}();
}

std::span<const char *const> bundledSignalKernelTargets() { return kTargets; }

bool selectSignalKernels(const IsaTarget &agent, SignalKernelObjects &out, std::string *error) {
    const signal_kernels::CodeObjects *specific = nullptr, *generic = nullptr;
    for (const auto &entry : signal_kernels::kCodeObjects) {
        uint32_t flags = 0, mailboxFlags = 0;
        uint8_t abi = 0, mailboxAbi = 0;
        bool isGeneric = false, mailboxGeneric = false;
        if (!codeObjectHeader(entry.operations, flags, abi) ||
            !codeObjectHeader(entry.mailbox, mailboxFlags, mailboxAbi) ||
            !codeObjectTargetCompatible(flags, abi, agent, nullptr, &isGeneric) ||
            !codeObjectTargetCompatible(mailboxFlags, mailboxAbi, agent, nullptr, &mailboxGeneric) ||
            isGeneric != mailboxGeneric) continue;
        if (!isGeneric && !specific) specific = &entry;
        if (isGeneric && !generic) generic = &entry;
    }
    const auto *chosen = specific ? specific : generic;
    if (!chosen) {
        if (error) {
            *error = "no bundled signal kernel is compatible with " + agent.isaName();
            if (!agent.generic.empty()) *error += " (generic family " + agent.generic + ")";
            *error += "; bundled targets:";
            for (const auto *target : kTargets) *error += std::string(" ") + target;
            *error += ". Add the family to hsa/tools/build-signal-kernels.sh and regenerate.";
        }
        return false;
    }
    out = {chosen->target, chosen->operations, chosen->mailbox, chosen == generic};
    return true;
}
}
