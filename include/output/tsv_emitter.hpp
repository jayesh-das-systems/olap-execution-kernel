#pragma once

#if !defined(__cplusplus) || __cplusplus < 202302L
#error "FATAL: Strict ISO C++23 required (-std=c++23)."
#endif

#include "common/exit_codes.hpp"
#include "storage/schema.hpp"

#include <unistd.h>
#include <array>
#include <charconv>
#include <span>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cerrno>

namespace engine {

class TsvEmitter {
private:
    static constexpr size_t BUFFER_SIZE = 65536uz; // 64 KiB I/O buffer
    std::array<char, BUFFER_SIZE> buffer_{};
    size_t cursor_ = 0uz;
    int out_fd_ = STDOUT_FILENO;

public:
    explicit TsvEmitter(int fd = STDOUT_FILENO) noexcept : out_fd_(fd) {}

    ~TsvEmitter() noexcept {
        (void)flush();
    }

    TsvEmitter(const TsvEmitter&) = delete;
    TsvEmitter& operator=(const TsvEmitter&) = delete;

    TsvEmitter(TsvEmitter&&) = delete;
    TsvEmitter& operator=(TsvEmitter&&) = delete;

    [[nodiscard]] EngineExitCode emit(std::span<const OutputRow> rows) noexcept {
        for (const auto& row : rows) {
            // Format: <key_hash>\t<agg_result>\t<agg_count>\n
            append_uint64(row.key_hash);
            append_char('\t');
            append_int64(row.agg_result);
            append_char('\t');
            append_uint64(row.agg_count);
            append_char('\n');

            if (cursor_ >= BUFFER_SIZE - 256uz) {
                const EngineExitCode status = flush();
                if (status != EngineExitCode::SUCCESS) {
                    return status;
                }
            }
        }
        return flush();
    }

    [[nodiscard]] EngineExitCode flush() noexcept {
        if (cursor_ == 0uz) {
            return EngineExitCode::SUCCESS;
        }

        size_t written = 0uz;
        const char* ptr = buffer_.data();

        while (written < cursor_) {
            ssize_t res = ::write(out_fd_, ptr + written, cursor_ - written);
            if (res < 0) {
                if (errno == EINTR) {
                    continue; // Interrupted by signal, retry
                }
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
            (void)flush();
        }
        buffer_[cursor_++] = c;
    }

    void append_uint64(uint64_t val) noexcept {
        if (cursor_ + 32uz >= BUFFER_SIZE) {
            (void)flush();
        }
        auto [ptr, ec] = std::to_chars(buffer_.data() + cursor_, buffer_.data() + BUFFER_SIZE, val);
        if (ec == std::errc{}) {
            cursor_ = static_cast<size_t>(ptr - buffer_.data());
        }
    }

    void append_int64(int64_t val) noexcept {
        if (cursor_ + 32uz >= BUFFER_SIZE) {
            (void)flush();
        }
        auto [ptr, ec] = std::to_chars(buffer_.data() + cursor_, buffer_.data() + BUFFER_SIZE, val);
        if (ec == std::errc{}) {
            cursor_ = static_cast<size_t>(ptr - buffer_.data());
        }
    }
};

} // namespace engine