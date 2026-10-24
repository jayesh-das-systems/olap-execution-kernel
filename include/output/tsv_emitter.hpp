#pragma once

#if !defined(__cplusplus) || __cplusplus < 202302L
#error "FATAL: This engine strictly requires an ISO C++23 compliant compiler."
#endif

#include "schema.hpp"
#include "exit_codes.hpp"

#include <unistd.h>
#include <charconv>
#include <array>
#include <cstdint>
#include <cstddef>
#include <span>
#include <iostream>
#include <cstring>
#include <cerrno>

namespace engine {

class TsvEmitter {
private:
    static constexpr size_t BUFFER_SIZE = 64uz * 1024uz;
    std::array<char, BUFFER_SIZE> buffer_{};
    size_t cursor_ = 0uz;
    int fd_ = STDOUT_FILENO;

public:
    explicit TsvEmitter(int output_fd = STDOUT_FILENO) noexcept : fd_(output_fd) {}

    ~TsvEmitter() noexcept {
        flush();
    }

    TsvEmitter(const TsvEmitter&) = delete;
    TsvEmitter& operator=(const TsvEmitter&) = delete;

    [[nodiscard]] EngineExitCode emit(std::span<const OutputRow> rows) noexcept {
        for (const auto& row : rows) {
            append_uint64(row.key_hash);
            append_char('\t');

            append_int64(row.agg_result);
            append_char('\t');

            append_uint64(row.agg_count);
            append_char('\n');
        }
        return flush();
    }

    [[nodiscard]] EngineExitCode flush() noexcept {
        if (cursor_ == 0uz) return EngineExitCode::SUCCESS;

        size_t written = 0uz;
        while (written < cursor_) {
            const ssize_t res = ::write(fd_, buffer_.data() + written, cursor_ - written);
            if (res < 0) {
                if (errno == EINTR) continue;
                std::cerr << "[OUTPUT ERROR] Write failure on descriptor " << fd_ 
                          << ": " << std::strerror(errno) << "\n";
                return EngineExitCode::ERR_OUTPUT_WRITE_FAILED;
            }
            written += static_cast<size_t>(res);
        }
        cursor_ = 0uz;
        return EngineExitCode::SUCCESS;
    }

private:
    void append_char(char c) noexcept {
        if (cursor_ >= BUFFER_SIZE) {
            flush();
        }
        buffer_[cursor_++] = c;
    }

    void append_uint64(uint64_t val) noexcept {
        if (cursor_ + 32uz >= BUFFER_SIZE) {
            flush();
        }
        auto [ptr, ec] = std::to_chars(buffer_.data() + cursor_, buffer_.data() + BUFFER_SIZE, val);
        cursor_ = static_cast<size_t>(ptr - buffer_.data());
    }

    void append_int64(int64_t val) noexcept {
        if (cursor_ + 32uz >= BUFFER_SIZE) {
            flush();
        }
        auto [ptr, ec] = std::to_chars(buffer_.data() + cursor_, buffer_.data() + BUFFER_SIZE, val);
        cursor_ = static_cast<size_t>(ptr - buffer_.data());
    }
};

} // namespace engine
