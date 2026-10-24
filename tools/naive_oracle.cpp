#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#pragma pack(push, 1)
struct FileHeader {
    char magic[4];          // "OLAP"
    uint32_t version;       // Format version
    uint64_t row_count;     // Total tuples in file
    uint64_t data_offset;   // Offset to raw columnar blocks
};
#pragma pack(pop)

struct QueryParams {
    std::string filepath;
    int64_t min_int = 0;
    int64_t max_int = 0;
    std::string str_prefix = "";
    bool filter_int = false;
    bool filter_str = false;
};

void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " <dataset.bin> [options]\n"
              << "Options:\n"
              << "  --min-int <val>      Minimum integer filter (inclusive)\n"
              << "  --max-int <val>      Maximum integer filter (inclusive)\n"
              << "  --str-prefix <val>   String prefix filter\n";
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
        }
    }

    std::ifstream file(params.filepath, std::ios::binary);
    if (!file) {
        std::cerr << "Error: Failed to open file: " << params.filepath << "\n";
        return 1;
    }

    FileHeader header{};
    file.read(reinterpret_cast<char*>(&header), sizeof(FileHeader));
    if (!file || std::memcmp(header.magic, "OLAP", 4) != 0) {
        std::cerr << "Error: Invalid binary file format or magic header\n";
        return 1;
    }

    const uint64_t n = header.row_count;

    // Direct scalar buffers matching raw columnar storage layout
    std::vector<int64_t> col_int(n);
    std::vector<char> col_char(n);
    std::vector<char> col_str(n * 16);
    const size_t mask_bytes = (n + 7) / 8;
    std::vector<uint8_t> col_mask(mask_bytes);

    file.seekg(header.data_offset);
    file.read(reinterpret_cast<char*>(col_int.data()), n * sizeof(int64_t));
    file.read(reinterpret_cast<char*>(col_char.data()), n * sizeof(char));
    file.read(reinterpret_cast<char*>(col_str.data()), n * 16);
    file.read(reinterpret_cast<char*>(col_mask.data()), mask_bytes);

    if (!file) {
        std::cerr << "Error: Truncated binary data encountered\n";
        return 1;
    }

    struct AggResult {
        uint64_t count = 0;
        int64_t sum = 0;
    };
    // Map with unsigned char key prevents sign-extension index faults
    std::map<unsigned char, AggResult> oracle_groups;

    // Strict scalar reference evaluation loop
    for (uint64_t i = 0; i < n; ++i) {
        // 1. Bitmask check: row >> 3 for byte index, row & 7 for bit index
        const uint8_t byte = col_mask[i >> 3];
        const bool is_live = (byte >> (i & 7)) & 1;
        if (!is_live) {
            continue;
        }

        // 2. Integer range predicate
        const int64_t int_val = col_int[i];
        if (params.filter_int) {
            if (int_val < params.min_int || int_val > params.max_int) {
                continue;
            }
        }

        // 3. Fixed-width 16-byte string prefix check
        if (params.filter_str) {
            const char* str_ptr = &col_str[i * 16];
            if (std::strncmp(str_ptr, params.str_prefix.c_str(), params.str_prefix.size()) != 0) {
                continue;
            }
        }

        // 4. Scalar accumulation into map
        const auto key = static_cast<unsigned char>(col_char[i]);
        oracle_groups[key].count++;
        oracle_groups[key].sum += int_val;
    }

    // Emit TSV output matching query engine output
    std::cout << "GROUP_KEY\tCOUNT\tSUM\n";
    for (const auto& [key, agg] : oracle_groups) {
        std::cout << static_cast<char>(key) << "\t" << agg.count << "\t" << agg.sum << "\n";
    }

    return 0;
}
