#include "trident/codec.hpp"

#include <stdexcept>

namespace trident {

void write_varint(std::vector<std::uint8_t>& out, std::uint64_t value) {
    while (value >= 0x80) {
        out.push_back(static_cast<std::uint8_t>(value | 0x80));
        value >>= 7;
    }
    out.push_back(static_cast<std::uint8_t>(value));
}

std::uint64_t read_varint(const std::uint8_t*& cursor, const std::uint8_t* end) {
    std::uint64_t value = 0;
    int shift = 0;
    while (cursor < end) {
        std::uint8_t byte = *cursor++;
        value |= static_cast<std::uint64_t>(byte & 0x7f) << shift;
        if ((byte & 0x80) == 0) return value;
        shift += 7;
        if (shift > 63) throw std::runtime_error("trident: varint too long");
    }
    throw std::runtime_error("trident: truncated varint");
}

std::vector<std::uint8_t> compress_index(const PermutedIndex& index) {
    std::vector<std::uint8_t> out;
    out.reserve(index.size() * 6);
    write_varint(out, index.size());
    const std::array<int, 3>& perm = index.permutation();
    std::uint64_t prev[3] = {0, 0, 0};
    for (std::size_t i = 0; i < index.size(); ++i) {
        const Triple& t = index[i];
        for (int c = 0; c < 3; ++c) {
            std::uint64_t raw = t[perm[c]].raw();
            write_varint(out, raw - prev[c]);
            prev[c] = raw;
        }
    }
    return out;
}

std::vector<Triple> decompress_index(const std::uint8_t* data, std::size_t size,
                                     const std::array<int, 3>& permutation) {
    const std::uint8_t* cursor = data;
    const std::uint8_t* end = data + size;
    std::uint64_t count = read_varint(cursor, end);
    std::vector<Triple> triples;
    triples.reserve(static_cast<std::size_t>(count));
    std::uint64_t prev[3] = {0, 0, 0};
    for (std::uint64_t i = 0; i < count; ++i) {
        TermId parts[3];
        for (int c = 0; c < 3; ++c) {
            prev[c] += read_varint(cursor, end);
            parts[permutation[c]] = TermId(prev[c]);
        }
        triples.push_back(Triple{parts[0], parts[1], parts[2]});
    }
    if (cursor != end) {
        throw std::runtime_error("trident: trailing bytes in compressed index");
    }
    return triples;
}

}  // namespace trident
