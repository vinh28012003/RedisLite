#include "store.hpp"

void Store::set(const std::string &key, const std::string &value, std::optional<int64_t> px_millis) {
    Entry entry;
    entry.value = value;

    if (px_millis) {
        entry.expiry = std::chrono::steady_clock::now() + std::chrono::milliseconds(*px_millis);
    }
    // No px_millis -> entry.expiry stays null opt (no TTL)
    // This also clear any previous TTL on the key (Redis spec)

    data_[key] = std::move(entry); 
}

std::optional<std::string> Store::get(const std::string &key) {
    auto it = data_.find(key);
    if (it == data_.end()) return std::nullopt;

    // Lazy expiration - check on access
    if (it->second.is_expired()) {
        data_.erase(it);
        return std::nullopt;
    }
   
    return it->second.value;
}

void Store::evict_expired() {
    int checked = 0;
    auto it = data_.begin();
    while (it != data_.end() && checked < 20) {
        if (it->second.is_expired()) {
            it = data_.erase(it);   // erase returns next valid iterator
        } else {
            ++it;
        }
        checked++;
    }
}

size_t Store::size() const { return data_.size(); }