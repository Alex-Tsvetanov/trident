// Variable-byte encoding and delta compression for sorted index rows.
// Used by the on-disk store format; the algorithms themselves are independent
// of memory mapping.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "trident/store.hpp"

namespace trident {

// Appends the unsigned value in little-endian seven-bit groups. The high bit of
// every byte but the last is set, which is the usual variable-byte convention.
void write_varint(std::vector<std::uint8_t>& out, std::uint64_t value);

// Reads one varint and advances the cursor. Throws std::runtime_error when the
// input ends mid-value.
std::uint64_t read_varint(const std::uint8_t*& cursor, const std::uint8_t* end);

// Encodes a sorted permutation as successive deltas of the three components in
// permutation order. The first row is absolute; each later row stores the
// component-wise difference from its predecessor. Deltas of zero dominate in a
// sorted index, which is why the compressed form is smaller than 24 bytes per
// triple on real data.
std::vector<std::uint8_t> compress_index(const PermutedIndex& index);

// Inverse of compress_index. The permutation is needed only to place the three
// decoded numbers back into subject/predicate/object positions.
std::vector<Triple> decompress_index(const std::uint8_t* data, std::size_t size,
                                     const std::array<int, 3>& permutation);

}  // namespace trident
