#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#pragma pack(push, 1)
struct FileHeader {
    char magic[4] = {'O', 'L', 'A', 'P'};
    uint32_t version = 1;
    uint64_t row_count = 0;
    uint64_t data_offset = sizeof(FileHeader);
};
#pragma pack(pop)

enum class MaskPattern {
    ALL_VALID,       // All bits 1 (0xFF)
    ALL_DELETED,     // All bits 0 (0x00)
    ALTERNATING,     // 0xAA (every second row live)
    RANDOM,          // Pseudo-random bit distribution
    SPARSE_EDGES     // Only first/last bits of chunks set
};

int main(int argc, char* argv[]) {
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0] << " <output_path> <row_count> <mask_pattern_id> [seed]\n"
                  << "Patterns: 0=ALL_VALID, 1=ALL_DELETED, 2=ALTERNATING, 3=RANDOM, 4=SPARSE_EDGES\n";
        return 1;
    }

    const std::string out_path = argv[1];
    const uint64_t n = std::stoull(argv[2]);
    const int pattern = std::stoi(argv[3]);
    const uint32_t seed = (argc >= 5) ? std::stoul(argv[4]) : 1337;

    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int64_t> int_dist(-1000000, 1000000);
    // Include full 0-255 range to explicitly test signed-char indexing bugs
    std::uniform_int_distribution<int> char_dist(0, 255);
    std::uniform_int_distribution<int> byte_dist(0, 255);

    std::vector<int64_t> col_int(n);
    std::vector<char> col_char(n);
    std::vector<char> col_str(n * 16, 0);
    const size_t mask_bytes = (n + 7) / 8;
    std::vector<uint8_t> col_mask(mask_bytes, 0);

    const std::vector<std::string> prefixes = {"APAC", "EMEA", "AMER", "LATAM", "NASA", "CORP"};

    for (uint64_t i = 0; i < n; ++i) {
        col_int[i] = int_dist(rng);
        col_char[i] = static_cast<char>(char_dist(rng));

        const std::string& pfx = prefixes[i % prefixes.size()];
        std::memcpy(&col_str[i * 16], pfx.data(), pfx.size());
    }

    // Configure live masks
    for (size_t b = 0; b < mask_bytes; ++b) {
        switch (pattern) {
            case 0: col_mask[b] = 0xFF; break;
            case 1: col_mask[b] = 0x00; break;
            case 2: col_mask[b] = 0xAA; break;
            case 3: col_mask[b] = static_cast<uint8_t>(byte_dist(rng)); break;
            case 4: col_mask[b] = (b % 128 == 0 || b % 128 == 127) ? 0x81 : 0x00; break;
            default: col_mask[b] = 0xFF; break;
        }
    }

    FileHeader header;
    header.row_count = n;

    std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::cerr << "Failed to open output: " << out_path << "\n";
        return 1;
    }

    out.write(reinterpret_cast<const char*>(&header), sizeof(FileHeader));
    out.write(reinterpret_cast<const char*>(col_int.data()), n * sizeof(int64_t));
    out.write(reinterpret_cast<const char*>(col_char.data()), n * sizeof(char));
    out.write(reinterpret_cast<const char*>(col_str.data()), n * 16);
    out.write(reinterpret_cast<const char*>(col_mask.data()), mask_bytes);

    return 0;
}
