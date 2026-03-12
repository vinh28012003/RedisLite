#pragma once
#include <string>
#include <cstdint>
#include <cstring>

class ReplicationBacklog {
    std::string buffer_;
    size_t capacity_;
    size_t write_pos_ = 0;
    int64_t start_offset_ = 0;  // global offset of first byte in buffer
    int64_t end_offset_ = 0;    // global offset past last written byte
    bool has_data_ = false;

public:
    explicit ReplicationBacklog(size_t capacity = 1024 * 1024)
        : buffer_(capacity, '\0'), capacity_(capacity) {}

    void feed(const std::string& data, int64_t global_offset) {
        if (data.empty()) return;

        if (!has_data_) {
            start_offset_ = global_offset;
            has_data_ = true;
        }

        const char* src = data.data();
        size_t remaining = data.size();

        while (remaining > 0) {
            size_t chunk = std::min(remaining, capacity_ - write_pos_);
            std::memcpy(&buffer_[write_pos_], src, chunk);
            write_pos_ = (write_pos_ + chunk) % capacity_;
            src += chunk;
            remaining -= chunk;
        }

        end_offset_ = global_offset + static_cast<int64_t>(data.size());

        // If we overwrote old data, advance start_offset
        if (end_offset_ - start_offset_ > static_cast<int64_t>(capacity_)) {
            start_offset_ = end_offset_ - static_cast<int64_t>(capacity_);
        }
    }

    bool contains(int64_t offset) const {
        return has_data_ && offset >= start_offset_ && offset <= end_offset_;
    }

    // Read all bytes from offset to end. Caller must check contains() first.
    std::string read(int64_t from_offset) const {
        if (from_offset >= end_offset_) return "";

        int64_t bytes_to_read = end_offset_ - from_offset;
        int64_t skip = from_offset - start_offset_;

        // Position in circular buffer where from_offset's data starts
        // write_pos_ points to where NEXT write goes = end of data
        // Data starts at (write_pos_ - (end_offset_ - start_offset_)) wrapped
        int64_t data_len = end_offset_ - start_offset_;
        size_t data_start = (write_pos_ + capacity_ - static_cast<size_t>(data_len % capacity_)) % capacity_;
        size_t read_start = (data_start + static_cast<size_t>(skip)) % capacity_;

        std::string result;
        result.reserve(bytes_to_read);

        size_t pos = read_start;
        int64_t left = bytes_to_read;
        while (left > 0) {
            size_t chunk = std::min(static_cast<size_t>(left), capacity_ - pos);
            result.append(&buffer_[pos], chunk);
            pos = (pos + chunk) % capacity_;
            left -= chunk;
        }

        return result;
    }

    // Initialize offset state without data (used after promotion)
    void set_start(int64_t offset) {
        start_offset_ = offset;
        end_offset_ = offset;
        write_pos_ = 0;
        has_data_ = true;
    }

    int64_t start_offset() const { return start_offset_; }
    int64_t end_offset() const { return end_offset_; }
    bool empty() const { return !has_data_; }
};