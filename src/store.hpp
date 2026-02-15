#pragma once                                                                                            
                 
#include <string>                                                                                       
#include <unordered_map>                                                                                
#include <optional>

class Store {
    std::unordered_map<std::string, std::string> data_;
public:
    void set(const std::string& key, const std::string& value);
    std::optional<std::string> get(const std::string& key) const;
};


