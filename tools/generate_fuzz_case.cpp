#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <random>
#include <string>
#include <unistd.h>
#include <vector>

#include "common/exit_codes.hpp"
#include "storage/file_format.hpp"

namespace {

[[nodiscard]] inline constexpr uint64_t align_to_64(uint64_t offset) noexcept {
    return (offset + 63uz) & ~63uz;
}

void write_exact(int fd, const void* data, size_t bytes) {
    const auto* ptr = static_cast<const uint8_t*>(data);
    size_t written = 0uz;
    while (written < bytes) {
        ssize_t res = ::write(fd, ptr + written, bytes - written);
        if (res < 0) {
            std::cerr << "Fatal write failure.\n";
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
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0] << " <output_path> <row_count> <mask_pattern_id> [seed]\n"
                  << "Patterns:\n"
                  << "  0: ALL_VALID       (0xFF)\n"
                  << "  1: ALL_DELETED     (0x00)\n"
                  << "  2: ALTERNATING     (0xAA)\n"
                  << "  3: PSEUDO_RANDOM   (Uniform random bits)\n"
                  << "  4: SPARSE_EDGES    (Boundary bits set)\n";
        return 1;
    }

    const std::string out_path = argv[1];
    const uint64_t total_rows = std::stoull(argv[2]);
    const int pattern = std::stoi(argv[3]);
    const uint64_t seed = (argc >= 5) ? std::stoull(argv[4]) : 1337uz;

    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int64_t> int_dist(-500'000, 500'000);
    std::uniform_int_distribution<int64_t> metric_dist(1, 100);
    std::uniform_int_distribution<int> char_dist(65, 90); // 'A' through 'Z'
    std::uniform_int_distribution<int> byte_dist(0, 255);

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

    const uint64_t validity_bytes = engine::calculate_validity_bytes(total_rows);

    uint64_t cursor = sizeof(engine::ColumnarFileHeader);
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

    int fd = ::open(out_path.c_str(), O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0644);
    if (fd < 0) {
        std::cerr << "Failed to create output file: " << out_path << "\n";
        return 1;
    }

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

    // 1. int_col
    std::vector<int64_t> int_col(total_rows);
    for (uint64_t i = 0; i < total_rows; ++i) int_col[i] = int_dist(rng);
    write_exact(fd, int_col.data(), total_rows * sizeof(int64_t));
    pad_zeros_until(fd, int_col_offset + total_rows * sizeof(int64_t), char_col_offset);

    // 2. char_col
    std::vector<char> char_col(total_rows);
    for (uint64_t i = 0; i < total_rows; ++i) char_col[i] = static_cast<char>(char_dist(rng));
    write_exact(fd, char_col.data(), total_rows * sizeof(char));
    pad_zeros_until(fd, char_col_offset + total_rows * sizeof(char), str_col_offset);

    // 3. str_col
    std::vector<engine::StringRef> vocab_refs;
    uint32_t pool_cursor = 0;
    for (const auto& s : sample_strings) {
        vocab_refs.push_back(engine::StringRef{
            .offset = pool_cursor,
            .length = static_cast<uint32_t>(s.size())
        });
        pool_cursor += static_cast<uint32_t>(s.size());
    }
    std::vector<engine::StringRef> str_col(total_rows);
    for (uint64_t i = 0; i < total_rows; ++i) str_col[i] = vocab_refs[i % vocab_refs.size()];
    write_exact(fd, str_col.data(), total_rows * sizeof(engine::StringRef));
    pad_zeros_until(fd, str_col_offset + total_rows * sizeof(engine::StringRef), metric_col_offset);

    // 4. metric_col
    std::vector<int64_t> metric_col(total_rows);
    for (uint64_t i = 0; i < total_rows; ++i) metric_col[i] = metric_dist(rng);
    write_exact(fd, metric_col.data(), total_rows * sizeof(int64_t));
    pad_zeros_until(fd, metric_col_offset + total_rows * sizeof(int64_t), string_pool_offset);

    // 5. String pool
    std::string serialized_pool;
    serialized_pool.reserve(string_pool_size);
    for (const auto& s : sample_strings) serialized_pool.append(s);
    serialized_pool.resize(string_pool_size, '\0');
    write_exact(fd, serialized_pool.data(), serialized_pool.size());
    pad_zeros_until(fd, string_pool_offset + string_pool_size, int_val_offset);

    // 6. Validity masks (all valid for columns)
    std::vector<uint8_t> col_validity(validity_bytes, 0xFF);
    write_exact(fd, col_validity.data(), validity_bytes);
    pad_zeros_until(fd, int_val_offset + validity_bytes, char_val_offset);
    write_exact(fd, col_validity.data(), validity_bytes);
    pad_zeros_until(fd, char_val_offset + validity_bytes, str_val_offset);
    write_exact(fd, col_validity.data(), validity_bytes);
    pad_zeros_until(fd, str_val_offset + validity_bytes, metric_val_offset);
    write_exact(fd, col_validity.data(), validity_bytes);
    pad_zeros_until(fd, metric_val_offset + validity_bytes, live_mask_offset);

    // 7. Live Mask (apply target pattern)
    std::vector<uint8_t> live_mask(validity_bytes, 0xFF);
    for (size_t b = 0; b < validity_bytes; ++b) {
        switch (pattern) {
            case 0: live_mask[b] = 0xFF; break;
            case 1: live_mask[b] = 0x00; break;
            case 2: live_mask[b] = 0xAA; break;
            case 3: live_mask[b] = static_cast<uint8_t>(byte_dist(rng)); break;
            case 4: live_mask[b] = (b % 16 == 0) ? 0x81 : 0x00; break;
            default: live_mask[b] = 0xFF; break;
        }
    }
    write_exact(fd, live_mask.data(), validity_bytes);
    pad_zeros_until(fd, live_mask_offset + validity_bytes, total_file_size);

    ::close(fd);
    return 0;
}
