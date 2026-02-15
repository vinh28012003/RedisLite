#include "store.hpp"

void Store::set(const std::string &key, const std::string &value) {
    data_[key] = value; 
}

std::optional<std::string> Store::get(const std::string &key) const {
    auto it = data_.find(key);
    if (it == data_.end()) return std::nullopt;
    return it->second;
}