#include "common/exit_codes.hpp"
#include "common/hardware_config.hpp"
#include "storage/schema.hpp"

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <string_view>
#include <charconv>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

namespace {

inline constexpr size_t WRITE_CHUNK_ROWS = 65536uz;

[[nodiscard]] inline constexpr uint64_t align_to_64(uint64_t offset) noexcept {
    return (offset + 63uz) & ~63uz;
}

void write_exact(int fd, const void* data, size_t bytes) {
    const auto* ptr = static_cast<const uint8_t*>(data);
    size_t written = 0uz;
    while (written < bytes) {
        ssize_t res = ::write(fd, ptr + written, bytes - written);
        if (res < 0) {
            std::cerr << "Fatal write failure to target binary file.\n";
            std::exit(engine::to_underlying(engine::EngineExitCode::ERR_IO_READ_FAILED));
        }
        written += static_cast<size_t>(res);
    }
}

void pad_zeros_until(int fd, uint64_t current_offset, uint64_t target_offset) {
    if (target_offset <= current_offset) return;
    const uint64_t diff = target_offset - current_offset;
    std::vector<uint8_t> zeros(diff, 0);
    write_exact(fd, zeros.data(), diff);
}

} // namespace

int main(int argc, char* argv[]) {
    uint64_t total_rows = 10'000'000uz;
    std::string out_path = "dataset.bin";

    if (argc >= 2) {
        out_path = argv[1];
    }
    if (argc >= 3) {
        std::string_view count_arg = argv[2];
        auto [ptr, ec] = std::from_chars(count_arg.data(), count_arg.data() + count_arg.size(), total_rows);
        if (ec != std::errc{} || total_rows == 0uz) {
            std::cerr << "Invalid row count specified. Defaulting to 10,000,000.\n";
            total_rows = 10'000'000uz;
        }
    }

    std::cout << "[DATA-GEN] Target File: " << out_path << "\n";
    std::cout << "[DATA-GEN] Generating " << total_rows << " columnar rows...\n";

    // 1. Calculate Schema Offsets and Alignment
    const uint64_t validity_bytes = engine::calculate_validity_bytes(total_rows);
    
    const std::vector<std::string> sample_strings = {
        "APAC_REVENUE_METRIC",
        "EMEA_LATENCY_LOG",
        "US_WEST_ANALYTICS_EVENT",
        "EU_CENTRAL_TELEMETRY_STREAM",
        "DEFAULT_INTERNAL_AUDIT_TAG"
    };

    uint64_t string_pool_size = 0uz;
    for (const auto& s : sample_strings) {
        string_pool_size += s.size();
    }
    string_pool_size = align_to_64(string_pool_size);

    uint64_t cursor = sizeof(engine::ColumnarFileHeader); // 128 bytes
    
    cursor = align_to_64(cursor);
    const uint64_t int_col_offset = cursor;
    cursor += align_to_64(total_rows * sizeof(int64_t));

    const uint64_t char_col_offset = cursor;
    cursor += align_to_64(total_rows * sizeof(char));

    const uint64_t str_col_offset = cursor;
    cursor += align_to_64(total_rows * sizeof(engine::StringRef));

    const uint64_t metric_col_offset = cursor;
    cursor += align_to_64(total_rows * sizeof(int64_t));

    const uint64_t string_pool_offset = cursor;
    cursor += string_pool_size;

    const uint64_t int_val_offset = cursor;
    cursor += align_to_64(validity_bytes);

    const uint64_t char_val_offset = cursor;
    cursor += align_to_64(validity_bytes);

    const uint64_t str_val_offset = cursor;
    cursor += align_to_64(validity_bytes);

    const uint64_t metric_val_offset = cursor;
    cursor += align_to_64(validity_bytes);

    const uint64_t live_mask_offset = cursor;
    cursor += align_to_64(validity_bytes);

    const uint64_t total_file_size = cursor;

    // 2. Open Output File with Direct Unbuffered Creation
    int fd = ::open(out_path.c_str(), O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0644);
    if (fd < 0) {
        std::cerr << "Failed to create output file: " << out_path << "\n";
        return engine::to_underlying(engine::EngineExitCode::ERR_IO_READ_FAILED);
    }

    // 3. Assemble and Write 128-Byte File Header
    engine::ColumnarFileHeader header{};
    header.magic = 0x4854454E; // "NETH"
    header.version = 3;
    header.row_count = total_rows;

    header.int_col_offset = int_col_offset;
    header.char_col_offset = char_col_offset;
    header.str_col_offset = str_col_offset;
    header.metric_col_offset = metric_col_offset;
    header.string_pool_offset = string_pool_offset;
    header.string_pool_size = string_pool_size;

    header.int_validity_offset = int_val_offset;
    header.char_validity_offset = char_val_offset;
    header.str_validity_offset = str_val_offset;
    header.metric_validity_offset = metric_val_offset;
    header.live_mask_offset = live_mask_offset;

    write_exact(fd, &header, sizeof(header));
    pad_zeros_until(fd, sizeof(header), int_col_offset);

    // 4. Stream Column: int_col
    std::cout << "[DATA-GEN] Streaming int64 column...\n";
    {
        std::vector<int64_t> chunk(WRITE_CHUNK_ROWS);
        uint64_t written_rows = 0uz;
        while (written_rows < total_rows) {
            const size_t n = std::min(WRITE_CHUNK_ROWS, total_rows - written_rows);
            for (size_t i = 0uz; i < n; ++i) {
                chunk[i] = static_cast<int64_t>((written_rows + i) % 100'000uz);
            }
            write_exact(fd, chunk.data(), n * sizeof(int64_t));
            written_rows += n;
        }
        pad_zeros_until(fd, int_col_offset + total_rows * sizeof(int64_t), char_col_offset);
    }

    // 5. Stream Column: char_col
    std::cout << "[DATA-GEN] Streaming char column...\n";
    {
        std::vector<char> chunk(WRITE_CHUNK_ROWS);
        uint64_t written_rows = 0uz;
        while (written_rows < total_rows) {
            const size_t n = std::min(WRITE_CHUNK_ROWS, total_rows - written_rows);
            for (size_t i = 0uz; i < n; ++i) {
                chunk[i] = static_cast<char>('A' + ((written_rows + i) % 26uz));
            }
            write_exact(fd, chunk.data(), n * sizeof(char));
            written_rows += n;
        }
        pad_zeros_until(fd, char_col_offset + total_rows * sizeof(char), str_col_offset);
    }

    // 6. Stream Column: str_col
    std::cout << "[DATA-GEN] Streaming StringRef column...\n";
    {
        std::vector<engine::StringRef> vocab_refs;
        uint32_t pool_cursor = 0;
        for (const auto& s : sample_strings) {
            vocab_refs.push_back(engine::StringRef{
                .offset = pool_cursor,
                .length = static_cast<uint32_t>(s.size())
            });
            pool_cursor += static_cast<uint32_t>(s.size());
        }

        std::vector<engine::StringRef> chunk(WRITE_CHUNK_ROWS);
        uint64_t written_rows = 0uz;
        while (written_rows < total_rows) {
            const size_t n = std::min(WRITE_CHUNK_ROWS, total_rows - written_rows);
            for (size_t i = 0uz; i < n; ++i) {
                chunk[i] = vocab_refs[(written_rows + i) % vocab_refs.size()];
            }
            write_exact(fd, chunk.data(), n * sizeof(engine::StringRef));
            written_rows += n;
        }
        pad_zeros_until(fd, str_col_offset + total_rows * sizeof(engine::StringRef), metric_col_offset);
    }

    // 7. Stream Column: metric_col
    std::cout << "[DATA-GEN] Streaming metric column...\n";
    {
        std::vector<int64_t> chunk(WRITE_CHUNK_ROWS);
        uint64_t written_rows = 0uz;
        while (written_rows < total_rows) {
            const size_t n = std::min(WRITE_CHUNK_ROWS, total_rows - written_rows);
            for (size_t i = 0uz; i < n; ++i) {
                chunk[i] = static_cast<int64_t>(((written_rows + i) % 500uz) + 1uz);
            }
            write_exact(fd, chunk.data(), n * sizeof(int64_t));
            written_rows += n;
        }
        pad_zeros_until(fd, metric_col_offset + total_rows * sizeof(int64_t), string_pool_offset);
    }

    // 8. Stream String Pool Arena
    std::cout << "[DATA-GEN] Writing string pool arena...\n";
    {
        std::string serialized_pool;
        serialized_pool.reserve(string_pool_size);
        for (const auto& s : sample_strings) {
            serialized_pool.append(s);
        }
        serialized_pool.resize(string_pool_size, '\0');
        write_exact(fd, serialized_pool.data(), serialized_pool.size());
        pad_zeros_until(fd, string_pool_offset + string_pool_size, int_val_offset);
    }

    // 9. Stream Validity Masks & Live Mask
    std::cout << "[DATA-GEN] Writing validity and live bit-vectors...\n";
    {
        std::vector<uint8_t> valid_mask(validity_bytes, 0xFF);

        write_exact(fd, valid_mask.data(), validity_bytes);
        pad_zeros_until(fd, int_val_offset + validity_bytes, char_val_offset);

        write_exact(fd, valid_mask.data(), validity_bytes);
        pad_zeros_until(fd, char_val_offset + validity_bytes, str_val_offset);

        write_exact(fd, valid_mask.data(), validity_bytes);
        pad_zeros_until(fd, str_val_offset + validity_bytes, metric_val_offset);

        write_exact(fd, valid_mask.data(), validity_bytes);
        pad_zeros_until(fd, metric_val_offset + validity_bytes, live_mask_offset);

        write_exact(fd, valid_mask.data(), validity_bytes);
        pad_zeros_until(fd, live_mask_offset + validity_bytes, total_file_size);
    }

    ::close(fd);

    const double mb = static_cast<double>(total_file_size) / (1024.0 * 1024.0);
    std::cout << "[DATA-GEN] Successfully created " << out_path << " (" << mb << " MiB, " << total_rows << " rows)\n";

    return engine::to_underlying(engine::EngineExitCode::SUCCESS);
}