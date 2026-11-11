#include "dlc_gate.hpp"

#include <algorithm>
#include <utility>

namespace anvil_script {

std::string_view gate_id(std::string_view name) {
    if (!name.starts_with(kDlcPrefix)) return {};
    const std::string_view id = name.substr(kDlcPrefix.size());
    if (id.empty() || id.size() > kMaxGateId) return {};
    for (const char c : id) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) return {};
    }
    return id;
}

bool GateTable::answer_for(const std::string& id) const {
    return allowed_ && std::find(active_.begin(), active_.end(), id) != active_.end();
}

int GateTable::intern(std::string_view id) {
    std::lock_guard<std::mutex> guard(lock_);
    const std::uint32_t n = count_.load(std::memory_order_relaxed);
    for (std::uint32_t i = 0; i < n; ++i) {
        if (ids_[i] == id) {
            parsed_[i].fetch_add(1, std::memory_order_relaxed);
            return static_cast<int>(i);
        }
    }
    if (n >= kMaxGates) return -1;
    ids_[n].assign(id);
    answers_[n].store(answer_for(ids_[n]) ? 1 : 0, std::memory_order_relaxed);
    parsed_[n].store(1, std::memory_order_relaxed);
    count_.store(n + 1, std::memory_order_release);
    return static_cast<int>(n);
}

bool GateTable::answer(int index) const {
    if (index < 0 || static_cast<std::uint32_t>(index) >= count_.load(std::memory_order_acquire)) return false;
    return answers_[static_cast<std::size_t>(index)].load(std::memory_order_relaxed) != 0;
}

void GateTable::update(bool allowed, std::vector<std::string> active) {
    std::lock_guard<std::mutex> guard(lock_);
    allowed_ = allowed;
    active_ = std::move(active);
    const std::uint32_t n = count_.load(std::memory_order_relaxed);
    for (std::uint32_t i = 0; i < n; ++i) {
        answers_[i].store(answer_for(ids_[i]) ? 1 : 0, std::memory_order_relaxed);
    }
}

std::string GateTable::id(int index) const {
    std::lock_guard<std::mutex> guard(lock_);
    return index >= 0 && static_cast<std::uint32_t>(index) < count_.load(std::memory_order_relaxed)
               ? ids_[static_cast<std::size_t>(index)]
               : std::string();
}

std::uint64_t GateTable::parsed(int index) const {
    return index >= 0 && static_cast<std::size_t>(index) < size()
               ? parsed_[static_cast<std::size_t>(index)].load(std::memory_order_relaxed)
               : 0;
}

}  // namespace anvil_script
