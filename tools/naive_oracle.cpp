#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "storage/file_format.hpp"

namespace {

constexpr uint64_t INITIAL_HASH_SEED   = 0x9e3779b97f4a7c15uz;
constexpr uint64_t NULL_KEY_HASH_SALT   = 0x9e3779b97f4a7c15uz;
constexpr uint64_t VALID_KEY_HASH_SALT  = 0xbf58476d1ce4e5b9uz;

[[nodiscard]] inline constexpr uint64_t splitmix64(uint64_t z) noexcept {
    z = (z ^ (z >> 30uz)) * 0xbf58476d1ce4e5b9uz;
    z = (z ^ (z >> 27uz)) * 0x94d049bb133111ebuz;
    return z ^ (z >> 31uz);
}

[[nodiscard]] inline constexpr uint64_t hash_bytes(std::string_view bytes) noexcept {
    uint64_t h = 0xcbf29ce484222325uz;
    for (char c : bytes) {
        h ^= static_cast<uint8_t>(c);
        h *= 0x100000001b3uz;
    }
    return splitmix64(h);
}

inline bool is_bit_set(const uint8_t* bitmask, size_t row) noexcept {
    return (bitmask[row >> 3] >> (row & 7)) & 1;
}

} // namespace

struct QueryParams {
    std::string filepath;
    int64_t min_int = 0;
    int64_t max_int = 0;
    std::string str_prefix = "";
    std::string group_by = "char";
    bool filter_int = false;
    bool filter_str = false;
};

void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " <dataset.bin> [options]\n"
              << "Options:\n"
              << "  --min-int <val>      Minimum integer filter\n"
              << "  --max-int <val>      Maximum integer filter\n"
              << "  --str-prefix <val>   String prefix filter\n"
              << "  --group-by <mode>    Grouping mode (char, int, str)\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    QueryParams params;
    params.filepath = argv[1];

    for (int i = 2; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--min-int" && i + 1 < argc) {
            params.min_int = std::stoll(argv[++i]);
            params.filter_int = true;
        } else if (arg == "--max-int" && i + 1 < argc) {
            params.max_int = std::stoll(argv[++i]);
            params.filter_int = true;
        } else if (arg == "--str-prefix" && i + 1 < argc) {
            params.str_prefix = argv[++i];
            params.filter_str = true;
        } else if (arg == "--group-by" && i + 1 < argc) {
            params.group_by = argv[++i];
        }
    }

    int fd = ::open(params.filepath.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        std::cerr << "Error: Failed to open file: " << params.filepath << "\n";
        return 1;
    }

    struct stat st{};
    if (::fstat(fd, &st) != 0 || st.st_size < static_cast<off_t>(sizeof(engine::ColumnarFileHeader))) {
        std::cerr << "Error: File truncated or inaccessible\n";
        ::close(fd);
        return 1;
    }

    const size_t file_size = static_cast<size_t>(st.st_size);
    void* mapped = ::mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mapped == MAP_FAILED) {
        std::cerr << "Error: mmap failed\n";
        ::close(fd);
        return 1;
    }

    const auto* header = reinterpret_cast<const engine::ColumnarFileHeader*>(mapped);
    if (header->magic != 0x4854454E || header->version != 3) {
        std::cerr << "Error: Invalid file format or version\n";
        ::munmap(mapped, file_size);
        ::close(fd);
        return 1;
    }

    const uint64_t n = header->row_count;
    const auto* base = static_cast<const uint8_t*>(mapped);

    const auto* col_int = reinterpret_cast<const int64_t*>(base + header->int_col_offset);
    const auto* col_char = reinterpret_cast<const char*>(base + header->char_col_offset);
    const auto* col_str = reinterpret_cast<const engine::StringRef*>(base + header->str_col_offset);
    const auto* col_metric = reinterpret_cast<const int64_t*>(base + header->metric_col_offset);
    const auto* str_pool = reinterpret_cast<const char*>(base + header->string_pool_offset);

    const auto* int_validity = reinterpret_cast<const uint8_t*>(base + header->int_validity_offset);
    const auto* str_validity = reinterpret_cast<const uint8_t*>(base + header->str_validity_offset);
    const auto* metric_validity = reinterpret_cast<const uint8_t*>(base + header->metric_validity_offset);
    const auto* live_mask = reinterpret_cast<const uint8_t*>(base + header->live_mask_offset);

    struct AggResult {
        uint64_t count = 0;
        int64_t sum = 0;
    };
    std::map<uint64_t, AggResult> oracle_groups;

    for (uint64_t i = 0; i < n; ++i) {
        // 1. Live mask filter
        if (!is_bit_set(live_mask, i)) continue;

        // 2. Integer range filter
        const int64_t int_val = col_int[i];
        if (params.filter_int) {
            if (int_val < params.min_int || int_val > params.max_int) {
                continue;
            }
        }

        // 3. String prefix filter
        const engine::StringRef ref = col_str[i];
        std::string_view str_val(str_pool + ref.offset, ref.length);
        if (params.filter_str) {
            if (!str_val.starts_with(params.str_prefix)) {
                continue;
            }
        }

        // 4. Calculate key hash based on grouping mode
        uint64_t key = 0;
        if (params.group_by == "char") {
            key = static_cast<uint64_t>(static_cast<unsigned char>(col_char[i]));
        } else if (params.group_by == "int") {
            uint64_t h = INITIAL_HASH_SEED;
            const bool valid = is_bit_set(int_validity, i);
            const uint64_t val = valid
                ? splitmix64(static_cast<uint64_t>(col_int[i]) ^ VALID_KEY_HASH_SALT)
                : splitmix64(NULL_KEY_HASH_SALT);
            h = std::rotl(h ^ val, 17);
            key = splitmix64(h);
        } else if (params.group_by == "str") {
            uint64_t h = INITIAL_HASH_SEED;
            uint64_t val = splitmix64(NULL_KEY_HASH_SALT);
            if (is_bit_set(str_validity, i) && ref.offset + ref.length <= header->string_pool_size) {
                val = hash_bytes(str_val);
            }
            h = std::rotl(h ^ val, 23);
            key = splitmix64(h);
        }

        const bool metric_valid = is_bit_set(metric_validity, i);
        oracle_groups[key].count++;
        oracle_groups[key].sum += metric_valid ? col_metric[i] : 0;
    }

    // Output: <key_hash>\t<agg_result>\t<agg_count>\n
    for (const auto& [key, agg] : oracle_groups) {
        std::cout << key << "\t" << agg.sum << "\t" << agg.count << "\n";
    }

    ::munmap(mapped, file_size);
    ::close(fd);
    return 0;
}
