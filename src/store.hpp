#pragma once                                                                                            
                 
#include <string>                                                                                       
#include <unordered_map>                                                                                
#include <optional>
#include <chrono>

struct Entry {
    std::string value;
    std::optional<std::chrono::steady_clock::time_point> expiry;

    bool is_expired() const {
        if (!expiry) return false;
        return std::chrono::steady_clock::now() >= *expiry;
    }
};

class Store {
    std::unordered_map<std::string, Entry> data_;
public:
    void set(const std::string &key, const std::string &value, std::optional<int64_t> px_millis = std::nullopt);
    std::optional<std::string> get(const std::string &key);
    void evict_expired();
};



