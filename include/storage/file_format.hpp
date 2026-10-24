#pragma once

#if !defined(__cplusplus) || __cplusplus < 202302L
#error "FATAL: This engine strictly requires an ISO C++23 compliant compiler."
#endif

#include "exit_codes.hpp"
#include "hardware_config.hpp"
#include "schema.hpp"

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cerrno>
#include <iostream>
#include <memory>
#include <utility>

namespace engine {

class MappedColumnarFile {
private:
    int fd_ = -1;
    void* mapped_addr_ = MAP_FAILED;
    size_t file_size_ = 0uz;
    ColumnarFileHeader* header_ = nullptr;

public:
    MappedColumnarFile() noexcept = default;
    ~MappedColumnarFile() noexcept { close(); }

    MappedColumnarFile(const MappedColumnarFile&) = delete;
    MappedColumnarFile& operator=(const MappedColumnarFile&) = delete;

    MappedColumnarFile(MappedColumnarFile&& other) noexcept
        : fd_(std::exchange(other.fd_, -1)),
          mapped_addr_(std::exchange(other.mapped_addr_, MAP_FAILED)),
          file_size_(std::exchange(other.file_size_, 0uz)),
          header_(std::exchange(other.header_, nullptr)) {}

    MappedColumnarFile& operator=(MappedColumnarFile&& other) noexcept {
        if (this != &other) {
            close();
            fd_ = std::exchange(other.fd_, -1);
            mapped_addr_ = std::exchange(other.mapped_addr_, MAP_FAILED);
            file_size_ = std::exchange(other.file_size_, 0uz);
            header_ = std::exchange(other.header_, nullptr);
        }
        return *this;
    }

    [[nodiscard]] EngineExitCode open_read(const char* path) noexcept {
        close();

        if (path == nullptr || *path == '\0') {
            std::cerr << "[STORAGE ERROR] Target binary path is null or empty.\n";
            return EngineExitCode::ERR_CLI_MISSING_FILE;
        }

        fd_ = ::open(path, O_RDONLY | O_CLOEXEC);
        if (fd_ < 0) {
            const int err = errno;
            std::cerr << "[STORAGE ERROR] Failed to open '" << path << "': " 
                      << std::strerror(err) << " (errno " << err << ")\n";
            return (err == ENOENT) ? EngineExitCode::ERR_IO_NOT_FOUND : EngineExitCode::ERR_IO_READ_FAILED;
        }

        struct stat st{};
        if (::fstat(fd_, &st) != 0) {
            const int err = errno;
            std::cerr << "[STORAGE ERROR] fstat failed on descriptor: " 
                      << std::strerror(err) << " (errno " << err << ")\n";
            close();
            return EngineExitCode::ERR_IO_READ_FAILED;
        }

        if (st.st_size == 0) {
            std::cerr << "[STORAGE ERROR] Target binary file is 0 bytes (empty).\n";
            close();
            return EngineExitCode::ERR_FILE_EMPTY;
        }

        if (st.st_size < static_cast<off_t>(sizeof(ColumnarFileHeader))) {
            std::cerr << "[STORAGE ERROR] File size (" << st.st_size 
                      << " bytes) is smaller than required 128-byte header.\n";
            close();
            return EngineExitCode::ERR_FILE_TRUNCATED;
        }

        file_size_ = static_cast<size_t>(st.st_size);

        mapped_addr_ = ::mmap(nullptr, file_size_, PROT_READ, MAP_PRIVATE, fd_, 0);
        if (mapped_addr_ == MAP_FAILED) {
            const int err = errno;
            std::cerr << "[VM ERROR] mmap (" << file_size_ << " bytes) failed: "
                      << std::strerror(err) << " (errno " << err << ")\n";
            close();
            return EngineExitCode::ERR_MMAP_FAILED;
        }

        if (::posix_madvise(mapped_addr_, file_size_, POSIX_MADV_SEQUENTIAL) != 0) {
            std::cerr << "[VM WARN] posix_madvise(SEQUENTIAL) rejected: " << std::strerror(errno) << "\n";
        }
        if (::posix_madvise(mapped_addr_, file_size_, POSIX_MADV_WILLNEED) != 0) {
            std::cerr << "[VM WARN] posix_madvise(WILLNEED) rejected: " << std::strerror(errno) << "\n";
        }

        return bind_and_validate();
    }

    void close() noexcept {
        g_state.reset();

        if (mapped_addr_ != MAP_FAILED) {
            if (::munmap(mapped_addr_, file_size_) != 0) {
                std::cerr << "[VM WARN] munmap failed: " << std::strerror(errno) << "\n";
            }
            mapped_addr_ = MAP_FAILED;
        }
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        file_size_ = 0uz;
        header_ = nullptr;
    }

    [[nodiscard]] const ColumnarFileHeader* header() const noexcept { return header_; }
    [[nodiscard]] size_t size() const noexcept { return file_size_; }

private:
    [[nodiscard]] EngineExitCode bind_and_validate() noexcept {
        if (!is_cache_aligned(mapped_addr_)) {
            std::cerr << "[ALIGNMENT ERROR] Virtual address " << mapped_addr_ 
                      << " violates 64-byte hardware cache line boundary.\n";
            close();
            return EngineExitCode::ERR_ALIGNMENT_FAULT;
        }

        header_ = std::start_lifetime_as<ColumnarFileHeader>(mapped_addr_);

        if (header_->magic != 0x4854454E) { // "NETH"
            std::cerr << "[SCHEMA ERROR] Magic mismatch: expected 0x4854454E ('NETH'), found 0x"
                      << std::hex << header_->magic << std::dec << "\n";
            close();
            return EngineExitCode::ERR_MAGIC_MISMATCH;
        }

        if (header_->version != 3uz) {
            std::cerr << "[SCHEMA ERROR] Unsupported version: engine requires v3, found v" 
                      << header_->version << "\n";
            close();
            return EngineExitCode::ERR_VERSION_UNSUPPORTED;
        }

        const uint64_t row_count = header_->row_count;
        const uint64_t validity_bytes = calculate_validity_bytes(row_count);

        if (header_->string_pool_size > MAX_STRING_POOL_CAPACITY) {
            std::cerr << "[STRING POOL ERROR] String arena length (" << header_->string_pool_size 
                      << ") exceeds 4 GiB 32-bit offset limits.\n";
            close();
            return EngineExitCode::ERR_STRING_POOL_OVERFLOW;
        }

        auto validate_region = [this](std::string_view name, uint64_t offset, uint64_t len) noexcept -> bool {
            if (offset < sizeof(ColumnarFileHeader)) {
                std::cerr << "[BOUNDS ERROR] " << name << " offset (" << offset 
                          << ") overlaps with 128-byte file header.\n";
                return false;
            }
            if (offset > file_size_ || len > (file_size_ - offset)) {
                std::cerr << "[BOUNDS ERROR] " << name << " region [offset " << offset 
                          << ", len " << len << "] exceeds file boundaries (" << file_size_ << " bytes).\n";
                return false;
            }
            return true;
        };

        if (!validate_region("int_col", header_->int_col_offset, row_count * sizeof(int64_t)) ||
            !validate_region("char_col", header_->char_col_offset, row_count * sizeof(char)) ||
            !validate_region("str_col", header_->str_col_offset, row_count * sizeof(StringRef)) ||
            !validate_region("metric_col", header_->metric_col_offset, row_count * sizeof(int64_t)) ||
            !validate_region("string_pool", header_->string_pool_offset, header_->string_pool_size) ||
            !validate_region("int_validity", header_->int_validity_offset, validity_bytes) ||
            !validate_region("char_validity", header_->char_validity_offset, validity_bytes) ||
            !validate_region("str_validity", header_->str_validity_offset, validity_bytes) ||
            !validate_region("metric_validity", header_->metric_validity_offset, validity_bytes) ||
            !validate_region("live_mask", header_->live_mask_offset, validity_bytes)) {
            close();
            return EngineExitCode::ERR_OFFSET_OUT_OF_BOUNDS;
        }

        auto* base_bytes = static_cast<std::byte*>(mapped_addr_);

        g_state.row_count = row_count;
        g_state.head_idx = 0uz;
        g_state.tail_idx = row_count;
        g_state.capacity = row_count;
        g_state.is_read_only = true;

        if (row_count > 0uz) {
            g_state.int_col = std::start_lifetime_as_array<int64_t>(
                base_bytes + header_->int_col_offset, row_count);
            g_state.char_col = std::start_lifetime_as_array<char>(
                base_bytes + header_->char_col_offset, row_count);
            g_state.str_col = std::start_lifetime_as_array<StringRef>(
                base_bytes + header_->str_col_offset, row_count);
            g_state.metric_col = std::start_lifetime_as_array<int64_t>(
                base_bytes + header_->metric_col_offset, row_count);
        }

        if (validity_bytes > 0uz) {
            g_state.int_validity = std::start_lifetime_as_array<uint8_t>(
                base_bytes + header_->int_validity_offset, validity_bytes);
            g_state.char_validity = std::start_lifetime_as_array<uint8_t>(
                base_bytes + header_->char_validity_offset, validity_bytes);
            g_state.str_validity = std::start_lifetime_as_array<uint8_t>(
                base_bytes + header_->str_validity_offset, validity_bytes);
            g_state.metric_validity = std::start_lifetime_as_array<uint8_t>(
                base_bytes + header_->metric_validity_offset, validity_bytes);
            g_state.live_mask = std::start_lifetime_as_array<uint8_t>(
                base_bytes + header_->live_mask_offset, validity_bytes);
        }

        g_state.string_pool_base = (header_->string_pool_size > 0uz)
            ? std::start_lifetime_as_array<char>(base_bytes + header_->string_pool_offset, header_->string_pool_size)
            : nullptr;
        g_state.string_pool_offset = header_->string_pool_size;
        g_state.string_pool_capacity = header_->string_pool_size;

        return EngineExitCode::SUCCESS;
    }
};

} // namespace engine
