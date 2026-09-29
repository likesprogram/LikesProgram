#include <LikesProgram/Http/Http3Qpack.hpp>

#include <algorithm>
#include <array>
#include <iterator>
#include <limits>
#include <set>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace LikesProgram {
    namespace Http {
        namespace {
            constexpr std::uint64_t kMaxQuicStreamId =
                (std::uint64_t{ 1 } << 62) - 1;

            struct QpackHuffmanCode {
                std::uint32_t code;
                std::uint8_t bits;
            };

            // RFC 7541 Appendix B; QPACK reuses this HPACK code.
            constexpr std::array<QpackHuffmanCode, 257> kQpackHuffmanCodes = {{
                { 0x1ff8ULL, 13 },
                { 0x7fffd8ULL, 23 },
                { 0xfffffe2ULL, 28 },
                { 0xfffffe3ULL, 28 },
                { 0xfffffe4ULL, 28 },
                { 0xfffffe5ULL, 28 },
                { 0xfffffe6ULL, 28 },
                { 0xfffffe7ULL, 28 },
                { 0xfffffe8ULL, 28 },
                { 0xffffeaULL, 24 },
                { 0x3ffffffcULL, 30 },
                { 0xfffffe9ULL, 28 },
                { 0xfffffeaULL, 28 },
                { 0x3ffffffdULL, 30 },
                { 0xfffffebULL, 28 },
                { 0xfffffecULL, 28 },
                { 0xfffffedULL, 28 },
                { 0xfffffeeULL, 28 },
                { 0xfffffefULL, 28 },
                { 0xffffff0ULL, 28 },
                { 0xffffff1ULL, 28 },
                { 0xffffff2ULL, 28 },
                { 0x3ffffffeULL, 30 },
                { 0xffffff3ULL, 28 },
                { 0xffffff4ULL, 28 },
                { 0xffffff5ULL, 28 },
                { 0xffffff6ULL, 28 },
                { 0xffffff7ULL, 28 },
                { 0xffffff8ULL, 28 },
                { 0xffffff9ULL, 28 },
                { 0xffffffaULL, 28 },
                { 0xffffffbULL, 28 },
                { 0x14ULL, 6 },
                { 0x3f8ULL, 10 },
                { 0x3f9ULL, 10 },
                { 0xffaULL, 12 },
                { 0x1ff9ULL, 13 },
                { 0x15ULL, 6 },
                { 0xf8ULL, 8 },
                { 0x7faULL, 11 },
                { 0x3faULL, 10 },
                { 0x3fbULL, 10 },
                { 0xf9ULL, 8 },
                { 0x7fbULL, 11 },
                { 0xfaULL, 8 },
                { 0x16ULL, 6 },
                { 0x17ULL, 6 },
                { 0x18ULL, 6 },
                { 0x0ULL, 5 },
                { 0x1ULL, 5 },
                { 0x2ULL, 5 },
                { 0x19ULL, 6 },
                { 0x1aULL, 6 },
                { 0x1bULL, 6 },
                { 0x1cULL, 6 },
                { 0x1dULL, 6 },
                { 0x1eULL, 6 },
                { 0x1fULL, 6 },
                { 0x5cULL, 7 },
                { 0xfbULL, 8 },
                { 0x7ffcULL, 15 },
                { 0x20ULL, 6 },
                { 0xffbULL, 12 },
                { 0x3fcULL, 10 },
                { 0x1ffaULL, 13 },
                { 0x21ULL, 6 },
                { 0x5dULL, 7 },
                { 0x5eULL, 7 },
                { 0x5fULL, 7 },
                { 0x60ULL, 7 },
                { 0x61ULL, 7 },
                { 0x62ULL, 7 },
                { 0x63ULL, 7 },
                { 0x64ULL, 7 },
                { 0x65ULL, 7 },
                { 0x66ULL, 7 },
                { 0x67ULL, 7 },
                { 0x68ULL, 7 },
                { 0x69ULL, 7 },
                { 0x6aULL, 7 },
                { 0x6bULL, 7 },
                { 0x6cULL, 7 },
                { 0x6dULL, 7 },
                { 0x6eULL, 7 },
                { 0x6fULL, 7 },
                { 0x70ULL, 7 },
                { 0x71ULL, 7 },
                { 0x72ULL, 7 },
                { 0xfcULL, 8 },
                { 0x73ULL, 7 },
                { 0xfdULL, 8 },
                { 0x1ffbULL, 13 },
                { 0x7fff0ULL, 19 },
                { 0x1ffcULL, 13 },
                { 0x3ffcULL, 14 },
                { 0x22ULL, 6 },
                { 0x7ffdULL, 15 },
                { 0x3ULL, 5 },
                { 0x23ULL, 6 },
                { 0x4ULL, 5 },
                { 0x24ULL, 6 },
                { 0x5ULL, 5 },
                { 0x25ULL, 6 },
                { 0x26ULL, 6 },
                { 0x27ULL, 6 },
                { 0x6ULL, 5 },
                { 0x74ULL, 7 },
                { 0x75ULL, 7 },
                { 0x28ULL, 6 },
                { 0x29ULL, 6 },
                { 0x2aULL, 6 },
                { 0x7ULL, 5 },
                { 0x2bULL, 6 },
                { 0x76ULL, 7 },
                { 0x2cULL, 6 },
                { 0x8ULL, 5 },
                { 0x9ULL, 5 },
                { 0x2dULL, 6 },
                { 0x77ULL, 7 },
                { 0x78ULL, 7 },
                { 0x79ULL, 7 },
                { 0x7aULL, 7 },
                { 0x7bULL, 7 },
                { 0x7ffeULL, 15 },
                { 0x7fcULL, 11 },
                { 0x3ffdULL, 14 },
                { 0x1ffdULL, 13 },
                { 0xffffffcULL, 28 },
                { 0xfffe6ULL, 20 },
                { 0x3fffd2ULL, 22 },
                { 0xfffe7ULL, 20 },
                { 0xfffe8ULL, 20 },
                { 0x3fffd3ULL, 22 },
                { 0x3fffd4ULL, 22 },
                { 0x3fffd5ULL, 22 },
                { 0x7fffd9ULL, 23 },
                { 0x3fffd6ULL, 22 },
                { 0x7fffdaULL, 23 },
                { 0x7fffdbULL, 23 },
                { 0x7fffdcULL, 23 },
                { 0x7fffddULL, 23 },
                { 0x7fffdeULL, 23 },
                { 0xffffebULL, 24 },
                { 0x7fffdfULL, 23 },
                { 0xffffecULL, 24 },
                { 0xffffedULL, 24 },
                { 0x3fffd7ULL, 22 },
                { 0x7fffe0ULL, 23 },
                { 0xffffeeULL, 24 },
                { 0x7fffe1ULL, 23 },
                { 0x7fffe2ULL, 23 },
                { 0x7fffe3ULL, 23 },
                { 0x7fffe4ULL, 23 },
                { 0x1fffdcULL, 21 },
                { 0x3fffd8ULL, 22 },
                { 0x7fffe5ULL, 23 },
                { 0x3fffd9ULL, 22 },
                { 0x7fffe6ULL, 23 },
                { 0x7fffe7ULL, 23 },
                { 0xffffefULL, 24 },
                { 0x3fffdaULL, 22 },
                { 0x1fffddULL, 21 },
                { 0xfffe9ULL, 20 },
                { 0x3fffdbULL, 22 },
                { 0x3fffdcULL, 22 },
                { 0x7fffe8ULL, 23 },
                { 0x7fffe9ULL, 23 },
                { 0x1fffdeULL, 21 },
                { 0x7fffeaULL, 23 },
                { 0x3fffddULL, 22 },
                { 0x3fffdeULL, 22 },
                { 0xfffff0ULL, 24 },
                { 0x1fffdfULL, 21 },
                { 0x3fffdfULL, 22 },
                { 0x7fffebULL, 23 },
                { 0x7fffecULL, 23 },
                { 0x1fffe0ULL, 21 },
                { 0x1fffe1ULL, 21 },
                { 0x3fffe0ULL, 22 },
                { 0x1fffe2ULL, 21 },
                { 0x7fffedULL, 23 },
                { 0x3fffe1ULL, 22 },
                { 0x7fffeeULL, 23 },
                { 0x7fffefULL, 23 },
                { 0xfffeaULL, 20 },
                { 0x3fffe2ULL, 22 },
                { 0x3fffe3ULL, 22 },
                { 0x3fffe4ULL, 22 },
                { 0x7ffff0ULL, 23 },
                { 0x3fffe5ULL, 22 },
                { 0x3fffe6ULL, 22 },
                { 0x7ffff1ULL, 23 },
                { 0x3ffffe0ULL, 26 },
                { 0x3ffffe1ULL, 26 },
                { 0xfffebULL, 20 },
                { 0x7fff1ULL, 19 },
                { 0x3fffe7ULL, 22 },
                { 0x7ffff2ULL, 23 },
                { 0x3fffe8ULL, 22 },
                { 0x1ffffecULL, 25 },
                { 0x3ffffe2ULL, 26 },
                { 0x3ffffe3ULL, 26 },
                { 0x3ffffe4ULL, 26 },
                { 0x7ffffdeULL, 27 },
                { 0x7ffffdfULL, 27 },
                { 0x3ffffe5ULL, 26 },
                { 0xfffff1ULL, 24 },
                { 0x1ffffedULL, 25 },
                { 0x7fff2ULL, 19 },
                { 0x1fffe3ULL, 21 },
                { 0x3ffffe6ULL, 26 },
                { 0x7ffffe0ULL, 27 },
                { 0x7ffffe1ULL, 27 },
                { 0x3ffffe7ULL, 26 },
                { 0x7ffffe2ULL, 27 },
                { 0xfffff2ULL, 24 },
                { 0x1fffe4ULL, 21 },
                { 0x1fffe5ULL, 21 },
                { 0x3ffffe8ULL, 26 },
                { 0x3ffffe9ULL, 26 },
                { 0xffffffdULL, 28 },
                { 0x7ffffe3ULL, 27 },
                { 0x7ffffe4ULL, 27 },
                { 0x7ffffe5ULL, 27 },
                { 0xfffecULL, 20 },
                { 0xfffff3ULL, 24 },
                { 0xfffedULL, 20 },
                { 0x1fffe6ULL, 21 },
                { 0x3fffe9ULL, 22 },
                { 0x1fffe7ULL, 21 },
                { 0x1fffe8ULL, 21 },
                { 0x7ffff3ULL, 23 },
                { 0x3fffeaULL, 22 },
                { 0x3fffebULL, 22 },
                { 0x1ffffeeULL, 25 },
                { 0x1ffffefULL, 25 },
                { 0xfffff4ULL, 24 },
                { 0xfffff5ULL, 24 },
                { 0x3ffffeaULL, 26 },
                { 0x7ffff4ULL, 23 },
                { 0x3ffffebULL, 26 },
                { 0x7ffffe6ULL, 27 },
                { 0x3ffffecULL, 26 },
                { 0x3ffffedULL, 26 },
                { 0x7ffffe7ULL, 27 },
                { 0x7ffffe8ULL, 27 },
                { 0x7ffffe9ULL, 27 },
                { 0x7ffffeaULL, 27 },
                { 0x7ffffebULL, 27 },
                { 0xffffffeULL, 28 },
                { 0x7ffffecULL, 27 },
                { 0x7ffffedULL, 27 },
                { 0x7ffffeeULL, 27 },
                { 0x7ffffefULL, 27 },
                { 0x7fffff0ULL, 27 },
                { 0x3ffffeeULL, 26 },
                { 0x3fffffffULL, 30 },
            }};

            struct QpackHuffmanNode {
                std::int16_t zero = -1;
                std::int16_t one = -1;
                std::int16_t symbol = -1;
            };

            const std::array<QpackHuffmanNode, 513>& QpackHuffmanTree() {
                static const auto tree = [] {
                    std::array<QpackHuffmanNode, 513> nodes{};
                    std::size_t used = 1;
                    for (std::size_t symbol = 0;
                         symbol < kQpackHuffmanCodes.size(); ++symbol) {
                        const auto& code = kQpackHuffmanCodes[symbol];
                        std::size_t node = 0;
                        for (std::uint8_t offset = 0; offset < code.bits; ++offset) {
                            const std::uint8_t shift = static_cast<std::uint8_t>(
                                code.bits - offset - 1);
                            const bool one = ((code.code >> shift) & 0x01) != 0;
                            if (node >= nodes.size()) return nodes;
                            auto& next = one ? nodes[node].one : nodes[node].zero;
                            if (next < 0) {
                                if (used >= nodes.size()) return nodes;
                                next = static_cast<std::int16_t>(used++);
                            }
                            node = static_cast<std::size_t>(next);
                        }
                        if (node >= nodes.size()) return nodes;
                        nodes[node].symbol = static_cast<std::int16_t>(symbol);
                    }
                    return nodes;
                }();
                return tree;
            }

            std::uint64_t QpackPrefixMask(std::uint8_t prefixBits) noexcept {
                return prefixBits == 8
                    ? std::uint64_t{ 0xFF }
                    : (std::uint64_t{ 1 } << prefixBits) - 1;
            }
        }

        Result<std::pair<std::uint64_t, std::size_t>>
            ParseHttp3QpackPrefixedInteger(
                const std::uint8_t* data,
                std::size_t size,
                std::uint8_t prefixBits) {
            if (data == nullptr || size == 0) {
                return Status::InvalidArgument(u"HTTP/3 QPACK integer is empty");
            }
            if (prefixBits == 0 || prefixBits > 8) {
                return Status::InvalidArgument(
                    u"HTTP/3 QPACK integer prefix width must be between 1 and 8");
            }

            const std::uint64_t prefixMask = QpackPrefixMask(prefixBits);
            std::uint64_t value = data[0] & prefixMask;
            if (value < prefixMask) return std::make_pair(value, std::size_t{ 1 });

            std::uint32_t shift = 0;
            for (std::size_t index = 1; index < size; ++index) {
                const std::uint8_t byte = data[index];
                const std::uint64_t chunk = byte & 0x7F;
                if (shift >= 62
                    || chunk > (kMaxQuicStreamId - value) >> shift) {
                    return Status(StatusCode::OutOfRange,
                        u"HTTP/3 QPACK integer exceeds 62-bit range");
                }
                value += chunk << shift;
                if ((byte & 0x80) == 0) {
                    return std::make_pair(value, index + 1);
                }
                shift += 7;
            }

            return Status::InvalidArgument(u"HTTP/3 QPACK integer is incomplete");
        }

        Result<std::vector<std::uint8_t>> BuildHttp3QpackPrefixedInteger(
            std::uint64_t value,
            std::uint8_t prefixBits,
            std::uint8_t firstBytePrefix) {
            if (prefixBits == 0 || prefixBits > 8) {
                return Status::InvalidArgument(
                    u"HTTP/3 QPACK integer prefix width must be between 1 and 8");
            }
            if (value > kMaxQuicStreamId) {
                return Status(StatusCode::OutOfRange,
                    u"HTTP/3 QPACK integer exceeds 62-bit range");
            }

            const std::uint64_t prefixMask = QpackPrefixMask(prefixBits);
            if ((firstBytePrefix & prefixMask) != 0) {
                return Status::InvalidArgument(
                    u"HTTP/3 QPACK instruction flags overlap the integer prefix");
            }

            std::vector<std::uint8_t> output;
            if (value < prefixMask) {
                output.push_back(static_cast<std::uint8_t>(
                    firstBytePrefix | static_cast<std::uint8_t>(value)));
                return output;
            }

            output.push_back(static_cast<std::uint8_t>(
                firstBytePrefix | static_cast<std::uint8_t>(prefixMask)));
            value -= prefixMask;
            do {
                std::uint8_t byte = static_cast<std::uint8_t>(value & 0x7F);
                value >>= 7;
                if (value != 0) byte |= 0x80;
                output.push_back(byte);
            } while (value != 0);
            return output;
        }

        Result<std::pair<Http3QpackStringLiteral, std::size_t>>
            ParseHttp3QpackStringLiteral(
                const std::uint8_t* data,
                std::size_t size,
                std::uint8_t prefixBits) {
            if (data == nullptr || size == 0) {
                return Status::InvalidArgument(u"HTTP/3 QPACK string is empty");
            }
            if (prefixBits < 2 || prefixBits > 8) {
                return Status::InvalidArgument(
                    u"HTTP/3 QPACK string prefix width must be between 2 and 8");
            }

            const std::uint8_t huffmanMask =
                static_cast<std::uint8_t>(std::uint8_t{ 1 } << (prefixBits - 1));
            auto length = ParseHttp3QpackPrefixedInteger(
                data, size, static_cast<std::uint8_t>(prefixBits - 1));
            if (!length.IsOk()) return length.GetStatus();
            const std::size_t payloadOffset = length.Value().second;
            if (length.Value().first > std::numeric_limits<std::size_t>::max()
                || payloadOffset > size
                || length.Value().first > size - payloadOffset) {
                return Status::InvalidArgument(
                    u"HTTP/3 QPACK string payload length exceeds input");
            }

            Http3QpackStringLiteral literal;
            literal.huffmanEncoded = (data[0] & huffmanMask) != 0;
            const std::size_t payloadSize =
                static_cast<std::size_t>(length.Value().first);
            literal.bytes.assign(data + payloadOffset,
                data + payloadOffset + payloadSize);
            return std::make_pair(std::move(literal), payloadOffset + payloadSize);
        }

        Result<std::vector<std::uint8_t>> BuildHttp3QpackStringLiteral(
            const Http3QpackStringLiteral& literal,
            std::uint8_t prefixBits,
            std::uint8_t firstBytePrefix) {
            if (prefixBits < 2 || prefixBits > 8) {
                return Status::InvalidArgument(
                    u"HTTP/3 QPACK string prefix width must be between 2 and 8");
            }
            if (literal.bytes.size() > kMaxQuicStreamId) {
                return Status(StatusCode::OutOfRange,
                    u"HTTP/3 QPACK string length exceeds 62-bit range");
            }

            const std::uint8_t huffmanMask =
                static_cast<std::uint8_t>(std::uint8_t{ 1 } << (prefixBits - 1));
            const std::uint8_t integerMask =
                static_cast<std::uint8_t>(huffmanMask - 1);
            if ((firstBytePrefix & integerMask) != 0
                || (firstBytePrefix & huffmanMask) != 0) {
                return Status::InvalidArgument(
                    u"HTTP/3 QPACK string flags overlap its prefix");
            }

            auto length = BuildHttp3QpackPrefixedInteger(
                literal.bytes.size(), static_cast<std::uint8_t>(prefixBits - 1),
                static_cast<std::uint8_t>(firstBytePrefix
                    | (literal.huffmanEncoded ? huffmanMask : 0)));
            if (!length.IsOk()) return length.GetStatus();
            auto output = length.MoveValue();
            output.insert(output.end(), literal.bytes.begin(), literal.bytes.end());
            return output;
        }

        Result<std::vector<std::uint8_t>> DecodeHttp3QpackHuffman(
            const std::uint8_t* data,
            std::size_t size,
            std::size_t maxDecodedBytes) {
            if (data == nullptr && size != 0) {
                return Status::InvalidArgument(
                    u"HTTP/3 QPACK Huffman input is null");
            }

            std::vector<std::uint8_t> output;
            output.reserve((std::min)(size, maxDecodedBytes));
            const auto& tree = QpackHuffmanTree();
            std::uint32_t trailingBits = 0;
            std::uint8_t bitCount = 0;
            std::int16_t node = 0;
            for (std::size_t byteIndex = 0; byteIndex < size; ++byteIndex) {
                const std::uint8_t byte = data[byteIndex];
                for (int bit = 7; bit >= 0; --bit) {
                    const bool one = ((byte >> bit) & 0x01) != 0;
                    node = one ? tree[node].one : tree[node].zero;
                    if (node < 0) {
                        return Status::InvalidArgument(
                            u"HTTP/3 QPACK Huffman code is invalid");
                    }
                    trailingBits = static_cast<std::uint32_t>(
                        (trailingBits << 1) | (one ? 1 : 0));
                    ++bitCount;
                    const std::int16_t symbol = tree[node].symbol;
                    if (symbol < 0) continue;
                    if (symbol == 256) {
                        return Status::InvalidArgument(
                            u"HTTP/3 QPACK Huffman payload contains EOS");
                    }
                    if (output.size() >= maxDecodedBytes) {
                        return Status(StatusCode::ResourceExhausted,
                            u"HTTP/3 QPACK Huffman output exceeds its limit");
                    }
                    output.push_back(static_cast<std::uint8_t>(symbol));
                    node = 0;
                    trailingBits = 0;
                    bitCount = 0;
                }
            }

            // RFC 7541 permits at most seven trailing one bits, which are the
            // most significant prefix of the EOS code.
            if (bitCount != 0
                && (bitCount > 7
                    || trailingBits
                        != (static_cast<std::uint32_t>(1) << bitCount) - 1)) {
                return Status::InvalidArgument(
                    u"HTTP/3 QPACK Huffman padding is invalid");
            }
            return output;
        }

        Result<std::pair<Http3QpackEncoderInstruction, std::size_t>>
            ParseHttp3QpackEncoderInstruction(
                const std::uint8_t* data,
                std::size_t size) {
            if (data == nullptr || size == 0) {
                return Status::InvalidArgument(
                    u"HTTP/3 QPACK encoder instruction is empty");
            }

            Http3QpackEncoderInstruction instruction;
            const std::uint8_t firstByte = data[0];
            if ((firstByte & 0xE0) == 0x20) {
                auto capacity = ParseHttp3QpackPrefixedInteger(data, size, 5);
                if (!capacity.IsOk()) return capacity.GetStatus();
                instruction.type =
                    Http3QpackEncoderInstructionType::SetDynamicTableCapacity;
                instruction.value = capacity.Value().first;
                return std::make_pair(std::move(instruction), capacity.Value().second);
            }
            if ((firstByte & 0x80) != 0) {
                auto index = ParseHttp3QpackPrefixedInteger(data, size, 6);
                if (!index.IsOk()) return index.GetStatus();
                auto fieldValue = ParseHttp3QpackStringLiteral(
                    data + index.Value().second, size - index.Value().second, 8);
                if (!fieldValue.IsOk()) return fieldValue.GetStatus();
                instruction.type =
                    Http3QpackEncoderInstructionType::InsertWithNameReference;
                instruction.value = index.Value().first;
                instruction.staticTable = (firstByte & 0x40) != 0;
                instruction.fieldValue = std::move(fieldValue.Value().first);
                return std::make_pair(std::move(instruction),
                    index.Value().second + fieldValue.Value().second);
            }
            if ((firstByte & 0xC0) == 0x40) {
                auto name = ParseHttp3QpackStringLiteral(data, size, 6);
                if (!name.IsOk()) return name.GetStatus();
                auto fieldValue = ParseHttp3QpackStringLiteral(
                    data + name.Value().second, size - name.Value().second, 8);
                if (!fieldValue.IsOk()) return fieldValue.GetStatus();
                instruction.type =
                    Http3QpackEncoderInstructionType::InsertWithLiteralName;
                instruction.name = std::move(name.Value().first);
                instruction.fieldValue = std::move(fieldValue.Value().first);
                return std::make_pair(std::move(instruction),
                    name.Value().second + fieldValue.Value().second);
            }

            auto index = ParseHttp3QpackPrefixedInteger(data, size, 5);
            if (!index.IsOk()) return index.GetStatus();
            instruction.type = Http3QpackEncoderInstructionType::Duplicate;
            instruction.value = index.Value().first;
            return std::make_pair(std::move(instruction), index.Value().second);
        }

        Result<std::vector<std::uint8_t>> BuildHttp3QpackEncoderInstruction(
            const Http3QpackEncoderInstruction& instruction) {
            switch (instruction.type) {
            case Http3QpackEncoderInstructionType::SetDynamicTableCapacity:
                return BuildHttp3QpackPrefixedInteger(instruction.value, 5, 0x20);
            case Http3QpackEncoderInstructionType::InsertWithNameReference: {
                auto index = BuildHttp3QpackPrefixedInteger(instruction.value, 6,
                    static_cast<std::uint8_t>(0x80
                        | (instruction.staticTable ? 0x40 : 0)));
                if (!index.IsOk()) return index.GetStatus();
                auto fieldValue = BuildHttp3QpackStringLiteral(
                    instruction.fieldValue, 8);
                if (!fieldValue.IsOk()) return fieldValue.GetStatus();
                auto output = index.MoveValue();
                output.insert(output.end(), fieldValue.Value().begin(),
                    fieldValue.Value().end());
                return output;
            }
            case Http3QpackEncoderInstructionType::InsertWithLiteralName: {
                auto name = BuildHttp3QpackStringLiteral(instruction.name, 6, 0x40);
                if (!name.IsOk()) return name.GetStatus();
                auto fieldValue = BuildHttp3QpackStringLiteral(
                    instruction.fieldValue, 8);
                if (!fieldValue.IsOk()) return fieldValue.GetStatus();
                auto output = name.MoveValue();
                output.insert(output.end(), fieldValue.Value().begin(),
                    fieldValue.Value().end());
                return output;
            }
            case Http3QpackEncoderInstructionType::Duplicate:
                return BuildHttp3QpackPrefixedInteger(instruction.value, 5, 0x00);
            }
            return Status::InvalidArgument(
                u"HTTP/3 QPACK encoder instruction type is not supported");
        }

        Result<std::pair<Http3QpackDecoderInstruction, std::size_t>>
            ParseHttp3QpackDecoderInstruction(
                const std::uint8_t* data,
                std::size_t size) {
            if (data == nullptr || size == 0) {
                return Status::InvalidArgument(
                    u"HTTP/3 QPACK decoder instruction is empty");
            }

            Http3QpackDecoderInstruction instruction;
            const std::uint8_t firstByte = data[0];
            std::uint8_t prefixBits = 6;
            if ((firstByte & 0x80) != 0) {
                instruction.type =
                    Http3QpackDecoderInstructionType::SectionAcknowledgment;
                prefixBits = 7;
            } else if ((firstByte & 0xC0) == 0x40) {
                instruction.type =
                    Http3QpackDecoderInstructionType::StreamCancellation;
            } else {
                instruction.type =
                    Http3QpackDecoderInstructionType::InsertCountIncrement;
            }

            auto value = ParseHttp3QpackPrefixedInteger(data, size, prefixBits);
            if (!value.IsOk()) return value.GetStatus();
            if (instruction.type
                    == Http3QpackDecoderInstructionType::InsertCountIncrement
                && value.Value().first == 0) {
                return Status::InvalidArgument(
                    u"HTTP/3 QPACK insert count increment must be non-zero");
            }
            instruction.value = value.Value().first;
            return std::make_pair(std::move(instruction), value.Value().second);
        }

        Result<std::vector<std::uint8_t>> BuildHttp3QpackDecoderInstruction(
            const Http3QpackDecoderInstruction& instruction) {
            switch (instruction.type) {
            case Http3QpackDecoderInstructionType::SectionAcknowledgment:
                return BuildHttp3QpackPrefixedInteger(instruction.value, 7, 0x80);
            case Http3QpackDecoderInstructionType::StreamCancellation:
                return BuildHttp3QpackPrefixedInteger(instruction.value, 6, 0x40);
            case Http3QpackDecoderInstructionType::InsertCountIncrement:
                if (instruction.value == 0) {
                    return Status::InvalidArgument(
                        u"HTTP/3 QPACK insert count increment must be non-zero");
                }
                return BuildHttp3QpackPrefixedInteger(instruction.value, 6, 0x00);
            }
            return Status::InvalidArgument(
                u"HTTP/3 QPACK decoder instruction type is not supported");
        }

        namespace {
            Result<std::uint64_t> QpackMaxEntries(
                std::size_t maxDynamicTableCapacity) {
                const std::uint64_t capacity = static_cast<std::uint64_t>(
                    maxDynamicTableCapacity);
                return capacity / 32;
            }

            Result<std::uint64_t> QpackFullRange(
                std::uint64_t maxEntries) {
                if (maxEntries == 0
                    || maxEntries > kMaxQuicStreamId / 2) {
                    return Status(StatusCode::OutOfRange,
                        u"HTTP/3 QPACK dynamic table capacity has no valid insert-count range");
                }
                return maxEntries * 2;
            }
        }

        Result<std::pair<Http3QpackFieldSectionPrefix, std::size_t>>
            ParseHttp3QpackFieldSectionPrefix(
                const std::uint8_t* data,
                std::size_t size,
                std::size_t maxDynamicTableCapacity,
                std::uint64_t totalNumberOfInserts) {
            if (data == nullptr || size == 0) {
                return Status::InvalidArgument(
                    u"HTTP/3 QPACK field-section prefix is empty");
            }

            auto maxEntries = QpackMaxEntries(maxDynamicTableCapacity);
            if (!maxEntries.IsOk()) return maxEntries.GetStatus();

            auto encoded = ParseHttp3QpackPrefixedInteger(data, size, 8);
            if (!encoded.IsOk()) return encoded.GetStatus();
            std::uint64_t requiredInsertCount = 0;
            if (encoded.Value().first != 0) {
                auto fullRange = QpackFullRange(maxEntries.Value());
                if (!fullRange.IsOk()) return fullRange.GetStatus();
                if (encoded.Value().first > fullRange.Value()) {
                    return Status(StatusCode::OutOfRange,
                        u"HTTP/3 QPACK encoded insert count exceeds its full range");
                }
                if (totalNumberOfInserts
                    > std::numeric_limits<std::uint64_t>::max()
                        - maxEntries.Value()) {
                    return Status(StatusCode::OutOfRange,
                        u"HTTP/3 QPACK insert count reconstruction overflows");
                }
                const std::uint64_t maxValue =
                    totalNumberOfInserts + maxEntries.Value();
                const std::uint64_t maxWrapped =
                    maxValue - (maxValue % fullRange.Value());
                if (maxWrapped
                    > std::numeric_limits<std::uint64_t>::max()
                        - (encoded.Value().first - 1)) {
                    return Status(StatusCode::OutOfRange,
                        u"HTTP/3 QPACK required insert count overflows");
                }
                requiredInsertCount =
                    maxWrapped + encoded.Value().first - 1;
                if (requiredInsertCount > maxValue) {
                    if (requiredInsertCount <= fullRange.Value()) {
                        return Status::InvalidArgument(
                            u"HTTP/3 QPACK encoded insert count cannot be reconstructed");
                    }
                    requiredInsertCount -= fullRange.Value();
                }
                if (requiredInsertCount == 0) {
                    return Status::InvalidArgument(
                        u"HTTP/3 QPACK zero required insert count must be encoded as zero");
                }
            }

            const std::size_t baseOffset = encoded.Value().second;
            if (baseOffset >= size) {
                return Status::InvalidArgument(
                    u"HTTP/3 QPACK field-section base is incomplete");
            }
            auto delta = ParseHttp3QpackPrefixedInteger(
                data + baseOffset, size - baseOffset, 7);
            if (!delta.IsOk()) return delta.GetStatus();
            const bool sign = (data[baseOffset] & 0x80) != 0;
            std::uint64_t base = 0;
            if (!sign) {
                if (requiredInsertCount
                    > std::numeric_limits<std::uint64_t>::max()
                        - delta.Value().first) {
                    return Status(StatusCode::OutOfRange,
                        u"HTTP/3 QPACK field-section base overflows");
                }
                base = requiredInsertCount + delta.Value().first;
            } else {
                if (requiredInsertCount <= delta.Value().first) {
                    return Status::InvalidArgument(
                        u"HTTP/3 QPACK field-section base would be negative");
                }
                base = requiredInsertCount - delta.Value().first - 1;
            }

            return std::make_pair(
                Http3QpackFieldSectionPrefix{ requiredInsertCount, base },
                baseOffset + delta.Value().second);
        }

        Result<std::vector<std::uint8_t>> BuildHttp3QpackFieldSectionPrefix(
            const Http3QpackFieldSectionPrefix& prefix,
            std::size_t maxDynamicTableCapacity) {
            auto maxEntries = QpackMaxEntries(maxDynamicTableCapacity);
            if (!maxEntries.IsOk()) return maxEntries.GetStatus();

            std::uint64_t encodedInsertCount = 0;
            if (prefix.requiredInsertCount != 0) {
                auto fullRange = QpackFullRange(maxEntries.Value());
                if (!fullRange.IsOk()) return fullRange.GetStatus();
                encodedInsertCount =
                    (prefix.requiredInsertCount % fullRange.Value()) + 1;
            }

            auto encoded = BuildHttp3QpackPrefixedInteger(
                encodedInsertCount, 8);
            if (!encoded.IsOk()) return encoded.GetStatus();

            const bool sign = prefix.base < prefix.requiredInsertCount;
            std::uint64_t delta = 0;
            if (sign) {
                delta = prefix.requiredInsertCount - prefix.base - 1;
            } else {
                delta = prefix.base - prefix.requiredInsertCount;
            }
            auto encodedBase = BuildHttp3QpackPrefixedInteger(
                delta, 7, static_cast<std::uint8_t>(sign ? 0x80 : 0x00));
            if (!encodedBase.IsOk()) return encodedBase.GetStatus();
            encoded.Value().insert(encoded.Value().end(),
                encodedBase.Value().begin(), encodedBase.Value().end());
            return encoded;
        }

        Result<std::pair<Http3QpackFieldLine, std::size_t>>
            ParseHttp3QpackFieldLine(
                const std::uint8_t* data,
                std::size_t size) {
            if (data == nullptr || size == 0) {
                return Status::InvalidArgument(
                    u"HTTP/3 QPACK field-line representation is empty");
            }

            Http3QpackFieldLine fieldLine;
            const std::uint8_t firstByte = data[0];
            if ((firstByte & 0x80) != 0) {
                auto index = ParseHttp3QpackPrefixedInteger(data, size, 6);
                if (!index.IsOk()) return index.GetStatus();
                fieldLine.type = Http3QpackFieldLineType::Indexed;
                fieldLine.staticTable = (firstByte & 0x40) != 0;
                fieldLine.index = index.Value().first;
                return std::make_pair(std::move(fieldLine), index.Value().second);
            }
            if ((firstByte & 0xC0) == 0x40) {
                auto index = ParseHttp3QpackPrefixedInteger(data, size, 4);
                if (!index.IsOk()) return index.GetStatus();
                const std::size_t valueOffset = index.Value().second;
                auto value = ParseHttp3QpackStringLiteral(
                    data + valueOffset, size - valueOffset, 8);
                if (!value.IsOk()) return value.GetStatus();
                fieldLine.type = Http3QpackFieldLineType::LiteralWithNameReference;
                fieldLine.neverIndex = (firstByte & 0x20) != 0;
                fieldLine.staticTable = (firstByte & 0x10) != 0;
                fieldLine.index = index.Value().first;
                fieldLine.value = std::move(value.Value().first);
                return std::make_pair(std::move(fieldLine),
                    valueOffset + value.Value().second);
            }
            if ((firstByte & 0xF0) == 0x10) {
                auto index = ParseHttp3QpackPrefixedInteger(data, size, 4);
                if (!index.IsOk()) return index.GetStatus();
                fieldLine.type = Http3QpackFieldLineType::IndexedPostBase;
                fieldLine.index = index.Value().first;
                return std::make_pair(std::move(fieldLine), index.Value().second);
            }
            if ((firstByte & 0xE0) == 0x20) {
                auto name = ParseHttp3QpackStringLiteral(data, size, 4);
                if (!name.IsOk()) return name.GetStatus();
                const std::size_t valueOffset = name.Value().second;
                auto value = ParseHttp3QpackStringLiteral(
                    data + valueOffset, size - valueOffset, 8);
                if (!value.IsOk()) return value.GetStatus();
                fieldLine.type = Http3QpackFieldLineType::LiteralWithLiteralName;
                fieldLine.neverIndex = (firstByte & 0x10) != 0;
                fieldLine.name = std::move(name.Value().first);
                fieldLine.value = std::move(value.Value().first);
                return std::make_pair(std::move(fieldLine),
                    valueOffset + value.Value().second);
            }
            if ((firstByte & 0xF0) == 0x00) {
                auto index = ParseHttp3QpackPrefixedInteger(data, size, 3);
                if (!index.IsOk()) return index.GetStatus();
                const std::size_t valueOffset = index.Value().second;
                auto value = ParseHttp3QpackStringLiteral(
                    data + valueOffset, size - valueOffset, 8);
                if (!value.IsOk()) return value.GetStatus();
                fieldLine.type =
                    Http3QpackFieldLineType::LiteralWithPostBaseNameReference;
                fieldLine.neverIndex = (firstByte & 0x08) != 0;
                fieldLine.index = index.Value().first;
                fieldLine.value = std::move(value.Value().first);
                return std::make_pair(std::move(fieldLine),
                    valueOffset + value.Value().second);
            }
            return Status::InvalidArgument(
                u"HTTP/3 QPACK field-line representation type is not supported");
        }

        Result<std::vector<std::uint8_t>> BuildHttp3QpackFieldLine(
            const Http3QpackFieldLine& fieldLine) {
            switch (fieldLine.type) {
            case Http3QpackFieldLineType::Indexed:
                if (fieldLine.neverIndex) {
                    return Status::InvalidArgument(
                        u"HTTP/3 QPACK indexed field-line cannot be never-indexed");
                }
                return BuildHttp3QpackPrefixedInteger(fieldLine.index, 6,
                    static_cast<std::uint8_t>(0x80
                        | (fieldLine.staticTable ? 0x40 : 0x00)));
            case Http3QpackFieldLineType::IndexedPostBase:
                if (fieldLine.staticTable || fieldLine.neverIndex) {
                    return Status::InvalidArgument(
                        u"HTTP/3 QPACK post-base indexed field-line flags are invalid");
                }
                return BuildHttp3QpackPrefixedInteger(fieldLine.index, 4, 0x10);
            case Http3QpackFieldLineType::LiteralWithNameReference: {
                auto index = BuildHttp3QpackPrefixedInteger(fieldLine.index, 4,
                    static_cast<std::uint8_t>(0x40
                        | (fieldLine.neverIndex ? 0x20 : 0x00)
                        | (fieldLine.staticTable ? 0x10 : 0x00)));
                if (!index.IsOk()) return index.GetStatus();
                auto value = BuildHttp3QpackStringLiteral(fieldLine.value, 8);
                if (!value.IsOk()) return value.GetStatus();
                auto output = index.MoveValue();
                output.insert(output.end(), value.Value().begin(), value.Value().end());
                return output;
            }
            case Http3QpackFieldLineType::LiteralWithPostBaseNameReference: {
                if (fieldLine.staticTable) {
                    return Status::InvalidArgument(
                        u"HTTP/3 QPACK post-base name reference cannot use the static table");
                }
                auto index = BuildHttp3QpackPrefixedInteger(fieldLine.index, 3,
                    static_cast<std::uint8_t>(fieldLine.neverIndex ? 0x08 : 0x00));
                if (!index.IsOk()) return index.GetStatus();
                auto value = BuildHttp3QpackStringLiteral(fieldLine.value, 8);
                if (!value.IsOk()) return value.GetStatus();
                auto output = index.MoveValue();
                output.insert(output.end(), value.Value().begin(), value.Value().end());
                return output;
            }
            case Http3QpackFieldLineType::LiteralWithLiteralName: {
                if (fieldLine.staticTable) {
                    return Status::InvalidArgument(
                        u"HTTP/3 QPACK literal-name field-line cannot use the static table");
                }
                auto name = BuildHttp3QpackStringLiteral(fieldLine.name, 4,
                    static_cast<std::uint8_t>(0x20
                        | (fieldLine.neverIndex ? 0x10 : 0x00)));
                if (!name.IsOk()) return name.GetStatus();
                auto value = BuildHttp3QpackStringLiteral(fieldLine.value, 8);
                if (!value.IsOk()) return value.GetStatus();
                auto output = name.MoveValue();
                output.insert(output.end(), value.Value().begin(), value.Value().end());
                return output;
            }
            }
            return Status::InvalidArgument(
                u"HTTP/3 QPACK field-line representation type is not supported");
        }

        namespace {
            using QpackStaticEntryView =
                std::pair<std::string_view, std::string_view>;

            constexpr std::array<QpackStaticEntryView, 99> kQpackStaticTable = {{
                { ":authority", "" },
                { ":path", "/" },
                { "age", "0" },
                { "content-disposition", "" },
                { "content-length", "0" },
                { "cookie", "" },
                { "date", "" },
                { "etag", "" },
                { "if-modified-since", "" },
                { "if-none-match", "" },
                { "last-modified", "" },
                { "link", "" },
                { "location", "" },
                { "referer", "" },
                { "set-cookie", "" },
                { ":method", "CONNECT" },
                { ":method", "DELETE" },
                { ":method", "GET" },
                { ":method", "HEAD" },
                { ":method", "OPTIONS" },
                { ":method", "POST" },
                { ":method", "PUT" },
                { ":scheme", "http" },
                { ":scheme", "https" },
                { ":status", "103" },
                { ":status", "200" },
                { ":status", "304" },
                { ":status", "404" },
                { ":status", "503" },
                { "accept", "*/*" },
                { "accept", "application/dns-message" },
                { "accept-encoding", "gzip, deflate, br" },
                { "accept-ranges", "bytes" },
                { "access-control-allow-headers", "cache-control" },
                { "access-control-allow-headers", "content-type" },
                { "access-control-allow-origin", "*" },
                { "cache-control", "max-age=0" },
                { "cache-control", "max-age=2592000" },
                { "cache-control", "max-age=604800" },
                { "cache-control", "no-cache" },
                { "cache-control", "no-store" },
                { "cache-control", "public, max-age=31536000" },
                { "content-encoding", "br" },
                { "content-encoding", "gzip" },
                { "content-type", "application/dns-message" },
                { "content-type", "application/javascript" },
                { "content-type", "application/json" },
                { "content-type", "application/x-www-form-urlencoded" },
                { "content-type", "image/gif" },
                { "content-type", "image/jpeg" },
                { "content-type", "image/png" },
                { "content-type", "text/css" },
                { "content-type", "text/html; charset=utf-8" },
                { "content-type", "text/plain" },
                { "content-type", "text/plain;charset=utf-8" },
                { "range", "bytes=0-" },
                { "strict-transport-security", "max-age=31536000" },
                { "strict-transport-security", "max-age=31536000; includesubdomains" },
                { "strict-transport-security", "max-age=31536000; includesubdomains; preload" },
                { "vary", "accept-encoding" },
                { "vary", "origin" },
                { "x-content-type-options", "nosniff" },
                { "x-xss-protection", "1; mode=block" },
                { ":status", "100" },
                { ":status", "204" },
                { ":status", "206" },
                { ":status", "302" },
                { ":status", "400" },
                { ":status", "403" },
                { ":status", "421" },
                { ":status", "425" },
                { ":status", "500" },
                { "accept-language", "" },
                { "access-control-allow-credentials", "FALSE" },
                { "access-control-allow-credentials", "TRUE" },
                { "access-control-allow-headers", "*" },
                { "access-control-allow-methods", "get" },
                { "access-control-allow-methods", "get, post, options" },
                { "access-control-allow-methods", "options" },
                { "access-control-expose-headers", "content-length" },
                { "access-control-request-headers", "content-type" },
                { "access-control-request-method", "get" },
                { "access-control-request-method", "post" },
                { "alt-svc", "clear" },
                { "authorization", "" },
                { "content-security-policy", "script-src 'none'; object-src 'none'; base-uri 'none'" },
                { "early-data", "1" },
                { "expect-ct", "" },
                { "forwarded", "" },
                { "if-range", "" },
                { "origin", "" },
                { "purpose", "prefetch" },
                { "server", "" },
                { "timing-allow-origin", "*" },
                { "upgrade-insecure-requests", "1" },
                { "user-agent", "" },
                { "x-forwarded-for", "" },
                { "x-frame-options", "deny" },
                { "x-frame-options", "sameorigin" }
            }};
        }

        std::size_t Http3QpackStaticTableSize() noexcept {
            return kQpackStaticTable.size();
        }

        Result<Http3QpackStaticEntry> GetHttp3QpackStaticEntry(
            std::uint64_t index) {
            if (index >= kQpackStaticTable.size()) {
                return Status::NotFound(
                    u"HTTP/3 QPACK static table index is unavailable");
            }
            const auto& entry = kQpackStaticTable[static_cast<std::size_t>(index)];
            return Http3QpackStaticEntry{
                index, std::string(entry.first), std::string(entry.second) };
        }

        namespace {
            Http3QpackStringLiteral QpackRawLiteral(
                const std::string& value) {
                Http3QpackStringLiteral literal;
                literal.bytes.assign(value.begin(), value.end());
                return literal;
            }

            Result<std::uint64_t> QpackAbsoluteIndex(
                const Http3QpackFieldLine& fieldLine,
                const Http3QpackFieldSectionPrefix& prefix) {
                std::uint64_t absoluteIndex = 0;
                if (fieldLine.type == Http3QpackFieldLineType::IndexedPostBase
                    || fieldLine.type
                        == Http3QpackFieldLineType::LiteralWithPostBaseNameReference) {
                    if (prefix.base
                        > std::numeric_limits<std::uint64_t>::max()
                            - fieldLine.index) {
                        return Status(StatusCode::OutOfRange,
                            u"HTTP/3 QPACK post-base index overflows");
                    }
                    absoluteIndex = prefix.base + fieldLine.index;
                } else {
                    if (prefix.base <= fieldLine.index) {
                        return Status::InvalidArgument(
                            u"HTTP/3 QPACK relative index is below table base");
                    }
                    absoluteIndex = prefix.base - fieldLine.index - 1;
                }
                if (absoluteIndex >= prefix.requiredInsertCount) {
                    return Status::InvalidArgument(
                        u"HTTP/3 QPACK dynamic reference exceeds required insert count");
                }
                return absoluteIndex;
            }
        }

        Result<Http3QpackResolvedField> ResolveHttp3QpackFieldLine(
            const Http3QpackFieldLine& fieldLine,
            const Http3QpackFieldSectionPrefix& prefix,
            const Http3QpackDynamicTable& dynamicTable) {
            Http3QpackResolvedField resolved;
            resolved.neverIndex = fieldLine.neverIndex;

            if (fieldLine.type == Http3QpackFieldLineType::LiteralWithLiteralName) {
                if (fieldLine.staticTable) {
                    return Status::InvalidArgument(
                        u"HTTP/3 QPACK literal-name field cannot reference a table");
                }
                resolved.name = fieldLine.name;
                resolved.value = fieldLine.value;
                return resolved;
            }

            if (fieldLine.type != Http3QpackFieldLineType::Indexed
                && fieldLine.type != Http3QpackFieldLineType::IndexedPostBase
                && fieldLine.type
                    != Http3QpackFieldLineType::LiteralWithNameReference
                && fieldLine.type
                    != Http3QpackFieldLineType::LiteralWithPostBaseNameReference) {
                return Status::InvalidArgument(
                    u"HTTP/3 QPACK field-line type cannot be resolved");
            }
            if ((fieldLine.type == Http3QpackFieldLineType::Indexed
                    || fieldLine.type == Http3QpackFieldLineType::IndexedPostBase)
                && fieldLine.neverIndex) {
                return Status::InvalidArgument(
                    u"HTTP/3 QPACK indexed field cannot be never-indexed");
            }
            if ((fieldLine.type == Http3QpackFieldLineType::IndexedPostBase
                    || fieldLine.type
                        == Http3QpackFieldLineType::LiteralWithPostBaseNameReference)
                && fieldLine.staticTable) {
                return Status::InvalidArgument(
                    u"HTTP/3 QPACK post-base field cannot reference the static table");
            }

            const bool indexedField =
                fieldLine.type == Http3QpackFieldLineType::Indexed
                || fieldLine.type == Http3QpackFieldLineType::IndexedPostBase;
            const bool staticReference =
                (fieldLine.type == Http3QpackFieldLineType::Indexed
                    && fieldLine.staticTable)
                || (fieldLine.type
                        == Http3QpackFieldLineType::LiteralWithNameReference
                    && fieldLine.staticTable);
            resolved.referencedTable = true;
            resolved.staticTable = staticReference;

            if (staticReference) {
                auto entry = GetHttp3QpackStaticEntry(fieldLine.index);
                if (!entry.IsOk()) return entry.GetStatus();
                resolved.referencedIndex = fieldLine.index;
                resolved.name = QpackRawLiteral(entry.Value().name);
                resolved.value = indexedField
                    ? QpackRawLiteral(entry.Value().value)
                    : fieldLine.value;
                return resolved;
            }

            auto absoluteIndex = QpackAbsoluteIndex(fieldLine, prefix);
            if (!absoluteIndex.IsOk()) return absoluteIndex.GetStatus();
            auto entry = dynamicTable.GetAbsolute(absoluteIndex.Value());
            if (!entry.IsOk()) return entry.GetStatus();
            resolved.referencedIndex = absoluteIndex.Value();
            resolved.name.bytes = entry.Value().name;
            resolved.value = indexedField
                ? Http3QpackStringLiteral{ false, entry.Value().value }
                : fieldLine.value;
            return resolved;
        }

        Result<Http3QpackParsedFieldSection> ParseHttp3QpackFieldSection(
            const std::uint8_t* data,
            std::size_t size,
            const Http3QpackDynamicTable& dynamicTable,
            Http3QpackFieldSectionLimits limits) {
            if (data == nullptr || size == 0) {
                return Status::InvalidArgument(
                    u"HTTP/3 QPACK field section is empty");
            }
            if (size > limits.maxEncodedBytes) {
                return Status(StatusCode::ResourceExhausted,
                    u"HTTP/3 QPACK encoded field section exceeds its byte limit");
            }

            const auto tableSnapshot = dynamicTable.Snapshot();
            auto prefix = ParseHttp3QpackFieldSectionPrefix(
                data, size, dynamicTable.MaxCapacity(), tableSnapshot.insertCount);
            if (!prefix.IsOk()) return prefix.GetStatus();
            if (prefix.Value().first.requiredInsertCount
                > tableSnapshot.insertCount) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 QPACK field section is blocked on dynamic inserts");
            }

            Http3QpackParsedFieldSection section;
            section.prefix = prefix.Value().first;
            section.encodedBytes = size;
            std::size_t offset = prefix.Value().second;
            bool hasDynamicReference = false;
            std::uint64_t largestDynamicIndex = 0;

            while (offset < size) {
                if (section.fields.size() >= limits.maxFields) {
                    return Status(StatusCode::ResourceExhausted,
                        u"HTTP/3 QPACK field count exceeds its limit");
                }
                auto fieldLine = ParseHttp3QpackFieldLine(
                    data + offset, size - offset);
                if (!fieldLine.IsOk()) return fieldLine.GetStatus();
                if (fieldLine.Value().second == 0
                    || fieldLine.Value().second > size - offset) {
                    return Status::Internal(
                        u"HTTP/3 QPACK field-line parser made no progress");
                }
                auto resolved = ResolveHttp3QpackFieldLine(
                    fieldLine.Value().first, section.prefix, dynamicTable);
                if (!resolved.IsOk()) return resolved.GetStatus();

                const std::size_t nameBytes = resolved.Value().name.bytes.size();
                const std::size_t valueBytes = resolved.Value().value.bytes.size();
                if (nameBytes > std::numeric_limits<std::size_t>::max()
                    - valueBytes) {
                    return Status(StatusCode::ResourceExhausted,
                        u"HTTP/3 QPACK resolved field bytes exceed their limit");
                }
                const std::size_t fieldBytes = nameBytes + valueBytes;
                if (fieldBytes > limits.maxResolvedBytes
                    || section.resolvedBytes
                        > limits.maxResolvedBytes - fieldBytes) {
                    return Status(StatusCode::ResourceExhausted,
                        u"HTTP/3 QPACK resolved field bytes exceed their limit");
                }
                section.resolvedBytes += fieldBytes;
                section.hasHuffman = section.hasHuffman
                    || resolved.Value().name.huffmanEncoded
                    || resolved.Value().value.huffmanEncoded;
                if (resolved.Value().referencedTable
                    && !resolved.Value().staticTable) {
                    if (!hasDynamicReference
                        || resolved.Value().referencedIndex > largestDynamicIndex) {
                        largestDynamicIndex = resolved.Value().referencedIndex;
                    }
                    hasDynamicReference = true;
                }
                section.fields.push_back(resolved.MoveValue());
                offset += fieldLine.Value().second;
            }

            const std::uint64_t expectedRequiredInsertCount =
                hasDynamicReference ? largestDynamicIndex + 1 : 0;
            if (section.prefix.requiredInsertCount
                != expectedRequiredInsertCount) {
                return Status::InvalidArgument(
                    u"HTTP/3 QPACK required insert count does not match references");
            }
            return section;
        }

        Result<Http3QpackParsedFieldSection> DecodeHttp3QpackFieldSection(
            const Http3QpackParsedFieldSection& section,
            std::size_t maxDecodedBytes) {
            Http3QpackParsedFieldSection decoded = section;
            decoded.resolvedBytes = 0;
            decoded.hasHuffman = false;

            auto decodeLiteral = [&](Http3QpackStringLiteral& literal)
                    -> Result<void> {
                const std::size_t remaining =
                    maxDecodedBytes - decoded.resolvedBytes;
                if (literal.huffmanEncoded) {
                    auto bytes = DecodeHttp3QpackHuffman(
                        literal.bytes.data(), literal.bytes.size(), remaining);
                    if (!bytes.IsOk()) return bytes.GetStatus();
                    literal.bytes = bytes.MoveValue();
                    literal.huffmanEncoded = false;
                }
                else if (literal.bytes.size() > remaining) {
                    return Status(StatusCode::ResourceExhausted,
                        u"HTTP/3 QPACK decoded field section exceeds its byte limit");
                }
                decoded.resolvedBytes += literal.bytes.size();
                return {};
            };

            for (auto& field : decoded.fields) {
                auto name = decodeLiteral(field.name);
                if (!name.IsOk()) return name.GetStatus();
                auto value = decodeLiteral(field.value);
                if (!value.IsOk()) return value.GetStatus();
            }
            return decoded;
        }

        Result<std::vector<HttpHeader>> DecodeHttp3QpackHeaderBlock(
            const Http3QpackParsedFieldSection& section,
            HttpHeaderBlockOptions options,
            std::size_t maxDecodedBytes) {
            auto decoded = DecodeHttp3QpackFieldSection(
                section, maxDecodedBytes);
            if (!decoded.IsOk()) return decoded.GetStatus();

            std::vector<HttpHeader> headers;
            headers.reserve(decoded.Value().fields.size());
            for (const auto& field : decoded.Value().fields) {
                headers.push_back({
                    std::string(field.name.bytes.begin(), field.name.bytes.end()),
                    std::string(field.value.bytes.begin(), field.value.bytes.end())
                });
            }
            auto validation = ValidateHttp3HeaderBlock(headers, options);
            if (!validation.IsOk()) return validation.GetStatus();
            return headers;
        }

        struct Http3QpackSectionTracker::Impl {
            explicit Impl(Http3QpackSectionTrackerLimits configuredLimits)
                : limits(configuredLimits) { }

            ~Impl() {
                ReleaseBudgetSlots();
            }

            Http3QpackSectionTrackerLimits limits;
            std::uint64_t knownInsertCount = 0;
            std::size_t outstandingSections = 0;
            std::size_t acknowledgedSections = 0;
            std::unordered_map<std::uint64_t, std::deque<std::uint64_t>> sections;
            std::unordered_set<std::uint64_t> blockedStreams;
            std::set<std::uint64_t> pendingUnblockedStreams; // 去重并稳定排序的待唤醒 stream
            Http3QpackResourceBudget* resourceBudget = nullptr;

            bool IsBlocked(std::uint64_t streamId) const noexcept {
                return blockedStreams.find(streamId) != blockedStreams.end();
            }

            void ReleaseBudgetSlots() noexcept {
                if (resourceBudget == nullptr) return;
                for (const auto streamId : blockedStreams) {
                    (void)resourceBudget->CloseBlockedStream(streamId);
                }
            }
        };

        Http3QpackSectionTracker::Http3QpackSectionTracker(
            Http3QpackSectionTrackerLimits limits)
            : m_impl(std::make_unique<Impl>(limits)) { }

        Http3QpackSectionTracker::~Http3QpackSectionTracker() = default;

        Http3QpackSectionTracker::Http3QpackSectionTracker(
            Http3QpackSectionTracker&&) noexcept = default;

        Http3QpackSectionTracker& Http3QpackSectionTracker::operator=(
            Http3QpackSectionTracker&&) noexcept = default;

        Result<void> Http3QpackSectionTracker::SetKnownInsertCount(
            std::uint64_t insertCount) {
            if (!m_impl) return Status(StatusCode::FailedPrecondition,
                u"HTTP/3 QPACK section tracker is moved-from");
            if (insertCount > kMaxQuicStreamId) {
                return Status(StatusCode::OutOfRange,
                    u"HTTP/3 QPACK known insert count exceeds 62 bits");
            }
            if (insertCount < m_impl->knownInsertCount) {
                return Status::InvalidArgument(
                    u"HTTP/3 QPACK known insert count cannot decrease");
            }
            if (insertCount == m_impl->knownInsertCount) return {};

            std::unordered_set<std::uint64_t> nextBlocked;
            std::set<std::uint64_t> nextPending;
            std::vector<std::uint64_t> releasedStreams;
            try {
                nextBlocked = m_impl->blockedStreams;
                nextPending = m_impl->pendingUnblockedStreams;
                releasedStreams.reserve(m_impl->blockedStreams.size());
                for (const auto& entry : m_impl->sections) {
                    const bool remainsBlocked = std::any_of(
                        entry.second.begin(), entry.second.end(),
                        [insertCount](std::uint64_t required) {
                            return required > insertCount;
                        });
                    if (remainsBlocked || !m_impl->IsBlocked(entry.first)) continue;
                    nextBlocked.erase(entry.first);
                    nextPending.insert(entry.first);
                    releasedStreams.push_back(entry.first);
                }
            }
            catch (...) {
                return Status(StatusCode::ResourceExhausted,
                    u"HTTP/3 QPACK section transition allocation failed");
            }

            if (m_impl->resourceBudget != nullptr) {
                for (const auto streamId : releasedStreams) {
                    if (!m_impl->resourceBudget->IsBlockedStreamOpen(streamId)) {
                        return Status(StatusCode::FailedPrecondition,
                            u"HTTP/3 QPACK blocked-stream budget is inconsistent");
                    }
                }
                std::size_t closed = 0;
                for (; closed < releasedStreams.size(); ++closed) {
                    auto result = m_impl->resourceBudget->CloseBlockedStream(
                        releasedStreams[closed]);
                    if (result.IsOk()) continue;
                    bool restored = true;
                    while (closed != 0) {
                        --closed;
                        restored = m_impl->resourceBudget->OpenBlockedStream(
                            releasedStreams[closed]).IsOk() && restored;
                    }
                    if (!restored) {
                        return Status::Internal(
                            u"HTTP/3 QPACK blocked-stream budget rollback failed");
                    }
                    return result.GetStatus();
                }
            }

            m_impl->knownInsertCount = insertCount;
            m_impl->blockedStreams.swap(nextBlocked);
            m_impl->pendingUnblockedStreams.swap(nextPending);
            return {};
        }

        Result<void> Http3QpackSectionTracker::OpenSection(
            std::uint64_t streamId,
            std::uint64_t requiredInsertCount) {
            if (!m_impl) return Status(StatusCode::FailedPrecondition,
                u"HTTP/3 QPACK section tracker is moved-from");
            if (streamId > kMaxQuicStreamId
                || requiredInsertCount > kMaxQuicStreamId) {
                return Status(StatusCode::OutOfRange,
                    u"HTTP/3 QPACK section tracker value exceeds 62 bits");
            }
            if (m_impl->outstandingSections
                >= m_impl->limits.maxOutstandingSections) {
                return Status(StatusCode::ResourceExhausted,
                    u"HTTP/3 QPACK outstanding section limit exceeded");
            }
            const bool sectionIsBlocked =
                requiredInsertCount > m_impl->knownInsertCount;
            const bool opensBlockedStream =
                sectionIsBlocked && !m_impl->IsBlocked(streamId);
            if (opensBlockedStream
                && m_impl->blockedStreams.size()
                    >= m_impl->limits.maxBlockedStreams) {
                return Status(StatusCode::ResourceExhausted,
                    u"HTTP/3 QPACK blocked stream limit exceeded");
            }

            if (opensBlockedStream && m_impl->resourceBudget != nullptr) {
                auto reserved = m_impl->resourceBudget->OpenBlockedStream(streamId);
                if (!reserved.IsOk()) return reserved.GetStatus();
            }

            bool createdStream = false;
            bool pushedSection = false;
            bool insertedBlockedStream = false;
            decltype(m_impl->sections.begin()) found = m_impl->sections.end();
            try {
                auto inserted = m_impl->sections.try_emplace(streamId);
                found = inserted.first;
                createdStream = inserted.second;
                found->second.push_back(requiredInsertCount);
                pushedSection = true;
                if (opensBlockedStream) {
                    insertedBlockedStream =
                        m_impl->blockedStreams.insert(streamId).second;
                }
            }
            catch (...) {
                if (insertedBlockedStream) m_impl->blockedStreams.erase(streamId);
                if (pushedSection) found->second.pop_back();
                if (createdStream && found != m_impl->sections.end()
                    && found->second.empty()) {
                    m_impl->sections.erase(found);
                }
                if (opensBlockedStream && m_impl->resourceBudget != nullptr) {
                    (void)m_impl->resourceBudget->CloseBlockedStream(streamId);
                }
                return Status(StatusCode::ResourceExhausted,
                    u"HTTP/3 QPACK section reservation allocation failed");
            }
            if (opensBlockedStream && !insertedBlockedStream) {
                found->second.pop_back();
                if (createdStream && found->second.empty()) {
                    m_impl->sections.erase(found);
                }
                if (m_impl->resourceBudget != nullptr) {
                    (void)m_impl->resourceBudget->CloseBlockedStream(streamId);
                }
                return Status::Internal(
                    u"HTTP/3 QPACK blocked-stream ledger is inconsistent");
            }

            ++m_impl->outstandingSections;
            // A newly blocked section supersedes an unread runnable transition.
            if (sectionIsBlocked) {
                m_impl->pendingUnblockedStreams.erase(streamId);
            }
            return {};
        }

        Result<void> Http3QpackSectionTracker::AcknowledgeSection(
            std::uint64_t streamId) {
            if (!m_impl) return Status(StatusCode::FailedPrecondition,
                u"HTTP/3 QPACK section tracker is moved-from");
            if (streamId > kMaxQuicStreamId) {
                return Status(StatusCode::OutOfRange,
                    u"HTTP/3 QPACK stream id exceeds 62 bits");
            }
            const auto found = m_impl->sections.find(streamId);
            if (found == m_impl->sections.end() || found->second.empty()) {
                return Status(StatusCode::NotFound,
                    u"HTTP/3 QPACK section is not outstanding");
            }
            if (m_impl->acknowledgedSections ==
                (std::numeric_limits<std::size_t>::max)()) {
                return Status(StatusCode::OutOfRange,
                    u"HTTP/3 QPACK acknowledgment count overflow");
            }
            const bool wasBlocked = m_impl->IsBlocked(streamId);
            const bool hasRemainingSections = found->second.size() > 1;
            const bool remainsBlocked = hasRemainingSections
                && std::any_of(std::next(found->second.begin()), found->second.end(),
                    [&](std::uint64_t required) {
                        return required > m_impl->knownInsertCount;
                    });
            const bool releasesBlockedStream = wasBlocked && !remainsBlocked;

            std::set<std::uint64_t> nextPending;
            if (releasesBlockedStream) {
                try {
                    nextPending = m_impl->pendingUnblockedStreams;
                    if (hasRemainingSections) nextPending.insert(streamId);
                    else nextPending.erase(streamId);
                }
                catch (...) {
                    return Status(StatusCode::ResourceExhausted,
                        u"HTTP/3 QPACK wakeup allocation failed");
                }
                if (m_impl->resourceBudget != nullptr) {
                    if (!m_impl->resourceBudget->IsBlockedStreamOpen(streamId)) {
                        return Status(StatusCode::FailedPrecondition,
                            u"HTTP/3 QPACK blocked-stream budget is inconsistent");
                    }
                    auto released =
                        m_impl->resourceBudget->CloseBlockedStream(streamId);
                    if (!released.IsOk()) return released.GetStatus();
                }
            }

            found->second.pop_front();
            --m_impl->outstandingSections;
            ++m_impl->acknowledgedSections;
            if (found->second.empty()) {
                m_impl->sections.erase(found);
                m_impl->blockedStreams.erase(streamId);
                if (!releasesBlockedStream) {
                    m_impl->pendingUnblockedStreams.erase(streamId);
                }
            }
            else if (releasesBlockedStream) {
                m_impl->blockedStreams.erase(streamId);
            }
            if (releasesBlockedStream) {
                m_impl->pendingUnblockedStreams.swap(nextPending);
            }
            return {};
        }

        Result<void> Http3QpackSectionTracker::CancelStream(
            std::uint64_t streamId) {
            if (!m_impl) return Status(StatusCode::FailedPrecondition,
                u"HTTP/3 QPACK section tracker is moved-from");
            if (streamId > kMaxQuicStreamId) {
                return Status(StatusCode::OutOfRange,
                    u"HTTP/3 QPACK stream id exceeds 62 bits");
            }
            const auto found = m_impl->sections.find(streamId);
            if (found == m_impl->sections.end()) {
                return Status(StatusCode::NotFound,
                    u"HTTP/3 QPACK stream has no outstanding sections");
            }
            const std::size_t count = found->second.size();
            if (count > m_impl->outstandingSections) {
                return Status::Internal(
                    u"HTTP/3 QPACK section tracker count is inconsistent");
            }
            if (m_impl->IsBlocked(streamId)
                && m_impl->resourceBudget != nullptr) {
                if (!m_impl->resourceBudget->IsBlockedStreamOpen(streamId)) {
                    return Status(StatusCode::FailedPrecondition,
                        u"HTTP/3 QPACK blocked-stream budget is inconsistent");
                }
                auto released =
                    m_impl->resourceBudget->CloseBlockedStream(streamId);
                if (!released.IsOk()) return released.GetStatus();
            }
            m_impl->outstandingSections -= count;
            m_impl->sections.erase(found);
            m_impl->blockedStreams.erase(streamId);
            m_impl->pendingUnblockedStreams.erase(streamId);
            return {};
        }

        Result<void> Http3QpackSectionTracker::AttachResourceBudget(
            Http3QpackResourceBudget* budget) {
            if (!m_impl) return Status(StatusCode::FailedPrecondition,
                u"HTTP/3 QPACK section tracker is moved-from");
            if (budget == nullptr) {
                return Status::InvalidArgument(
                    u"HTTP/3 QPACK section tracker budget is null");
            }
            if (m_impl->resourceBudget != nullptr) {
                if (m_impl->resourceBudget == budget) return {};
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 QPACK section tracker already has a budget");
            }
            if (m_impl->outstandingSections != 0
                || m_impl->acknowledgedSections != 0
                || !m_impl->sections.empty()
                || !m_impl->blockedStreams.empty()
                || !m_impl->pendingUnblockedStreams.empty()) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 QPACK budget must attach before section tracking");
            }
            auto usable = budget->ValidateHeaderBlock(0);
            if (!usable.IsOk()) return usable.GetStatus();
            if (budget->Snapshot().blockedStreams != 0) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 QPACK budget already tracks blocked streams");
            }
            m_impl->resourceBudget = budget;
            return {};
        }

        bool Http3QpackSectionTracker::HasResourceBudget() const noexcept {
            return m_impl != nullptr && m_impl->resourceBudget != nullptr;
        }

        Result<std::vector<std::uint64_t>>
            Http3QpackSectionTracker::TakeUnblockedStreams() {
            if (!m_impl) return Status(StatusCode::FailedPrecondition,
                u"HTTP/3 QPACK section tracker is moved-from");

            // Copy the ordered set into a caller-owned one-shot scheduling batch.
            std::vector<std::uint64_t> streamIds;
            try {
                streamIds.assign(m_impl->pendingUnblockedStreams.begin(),
                    m_impl->pendingUnblockedStreams.end());
            }
            catch (...) {
                return Status(StatusCode::ResourceExhausted,
                    u"HTTP/3 QPACK wakeup batch allocation failed");
            }
            m_impl->pendingUnblockedStreams.clear();
            return streamIds;
        }

        Http3QpackSectionTrackerLimits Http3QpackSectionTracker::Limits()
            const noexcept {
            return m_impl ? m_impl->limits : Http3QpackSectionTrackerLimits{};
        }

        Http3QpackSectionTrackerSnapshot Http3QpackSectionTracker::Snapshot()
            const noexcept {
            if (!m_impl) return {};
            return { m_impl->knownInsertCount, m_impl->outstandingSections,
                m_impl->blockedStreams.size(), m_impl->acknowledgedSections,
                m_impl->pendingUnblockedStreams.size() };
        }

        void Http3QpackSectionTracker::Reset() noexcept {
            if (!m_impl) return;
            m_impl->ReleaseBudgetSlots();
            m_impl->knownInsertCount = 0;
            m_impl->outstandingSections = 0;
            m_impl->acknowledgedSections = 0;
            m_impl->sections.clear();
            m_impl->blockedStreams.clear();
            m_impl->pendingUnblockedStreams.clear();
        }

        Result<void> ApplyHttp3QpackDecoderInstruction(
            const Http3QpackDecoderInstruction& instruction,
            Http3QpackSectionTracker& sectionTracker) {
            switch (instruction.type) {
            case Http3QpackDecoderInstructionType::SectionAcknowledgment:
                return sectionTracker.AcknowledgeSection(instruction.value);
            case Http3QpackDecoderInstructionType::StreamCancellation:
                return sectionTracker.CancelStream(instruction.value);
            case Http3QpackDecoderInstructionType::InsertCountIncrement: {
                if (instruction.value == 0) {
                    return Status::InvalidArgument(
                        u"HTTP/3 QPACK insert count increment must be non-zero");
                }
                const auto snapshot = sectionTracker.Snapshot();
                if (instruction.value > kMaxQuicStreamId
                    || snapshot.knownInsertCount
                        > kMaxQuicStreamId - instruction.value) {
                    return Status(StatusCode::OutOfRange,
                        u"HTTP/3 QPACK insert count increment overflows 62 bits");
                }
                return sectionTracker.SetKnownInsertCount(
                    snapshot.knownInsertCount + instruction.value);
            }
            }
            return Status::InvalidArgument(
                u"HTTP/3 QPACK decoder instruction type is invalid");
        }

        Result<Http3QpackStreamQuicActions>
            MapHttp3QpackFieldSectionFailure(const Status& failure) {
            if (failure.IsOk()) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 QPACK field section has no failure");
            }
            return Http3QpackStreamQuicActions{
                static_cast<std::uint64_t>(
                    Http3QpackStreamErrorCode::DecompressionFailed), true };
        }

        struct Http3QpackDynamicTable::Impl {
            explicit Impl(std::size_t maximumCapacity)
                : maxCapacity(maximumCapacity) { }

            ~Impl() {
                ReleaseBudgetUsage();
            }

            std::size_t maxCapacity = 0;
            std::size_t capacity = 0;
            std::size_t bytes = 0;
            std::size_t droppedEntries = 0;
            std::uint64_t insertCount = 0;
            std::deque<Http3QpackDynamicEntry> entries;
            Http3QpackResourceBudget* resourceBudget = nullptr;

            bool BudgetMatches() const noexcept {
                if (resourceBudget == nullptr) return true;
                const auto snapshot = resourceBudget->Snapshot();
                return snapshot.dynamicTableCapacity == capacity
                    && snapshot.dynamicTableBytes == bytes;
            }

            void ReleaseBudgetUsage() noexcept {
                if (resourceBudget == nullptr) return;
                if (bytes != 0) {
                    (void)resourceBudget->ReleaseDynamicTable(bytes);
                }
                (void)resourceBudget->SetDynamicTableCapacity(0);
            }
        };

        namespace {
            Result<std::size_t> QpackDynamicEntrySize(
                const std::vector<std::uint8_t>& name,
                const std::vector<std::uint8_t>& value) {
                constexpr std::size_t overhead = 32;
                if (name.size() > std::numeric_limits<std::size_t>::max()
                        - value.size()
                    || name.size() + value.size()
                        > std::numeric_limits<std::size_t>::max() - overhead) {
                    return Status(StatusCode::OutOfRange,
                        u"HTTP/3 QPACK dynamic entry size overflows");
                }
                return name.size() + value.size() + overhead;
            }

        }

        Http3QpackDynamicTable::Http3QpackDynamicTable(std::size_t maxCapacity)
            : m_impl(std::make_unique<Impl>(maxCapacity)) { }

        Http3QpackDynamicTable::~Http3QpackDynamicTable() = default;

        Http3QpackDynamicTable::Http3QpackDynamicTable(
            Http3QpackDynamicTable&&) noexcept = default;

        Http3QpackDynamicTable& Http3QpackDynamicTable::operator=(
            Http3QpackDynamicTable&&) noexcept = default;

        Result<void> Http3QpackDynamicTable::SetCapacity(std::size_t capacity) {
            if (!m_impl) return Status::Internal(u"HTTP/3 QPACK table is moved-from");
            if (capacity > m_impl->maxCapacity) {
                return Status(StatusCode::ResourceExhausted,
                    u"HTTP/3 QPACK dynamic table capacity exceeds configured limit");
            }

            std::size_t nextBytes = m_impl->bytes;
            std::size_t evictionCount = 0;
            for (auto found = m_impl->entries.rbegin();
                nextBytes > capacity && found != m_impl->entries.rend();
                ++found) {
                if (found->size > nextBytes) {
                    return Status::Internal(
                        u"HTTP/3 QPACK dynamic table byte ledger is inconsistent");
                }
                nextBytes -= found->size;
                ++evictionCount;
            }
            if (nextBytes > capacity) {
                return Status::Internal(
                    u"HTTP/3 QPACK dynamic table eviction is inconsistent");
            }
            if (m_impl->droppedEntries
                > (std::numeric_limits<std::size_t>::max)() - evictionCount) {
                return Status(StatusCode::OutOfRange,
                    u"HTTP/3 QPACK dropped-entry count is exhausted");
            }

            const auto releasedBytes = m_impl->bytes - nextBytes;
            if (m_impl->resourceBudget != nullptr) {
                if (!m_impl->BudgetMatches()) {
                    return Status(StatusCode::FailedPrecondition,
                        u"HTTP/3 QPACK dynamic-table budget is inconsistent");
                }
                if (releasedBytes != 0) {
                    auto released = m_impl->resourceBudget->ReleaseDynamicTable(
                        releasedBytes);
                    if (!released.IsOk()) return released.GetStatus();
                }
                auto updated =
                    m_impl->resourceBudget->SetDynamicTableCapacity(capacity);
                if (!updated.IsOk()) {
                    if (releasedBytes != 0
                        && !m_impl->resourceBudget->ReserveDynamicTable(
                            releasedBytes).IsOk()) {
                        return Status::Internal(
                            u"HTTP/3 QPACK dynamic-table budget rollback failed");
                    }
                    return updated.GetStatus();
                }
            }

            const auto droppedCount = evictionCount;
            while (evictionCount != 0) {
                m_impl->entries.pop_back();
                --evictionCount;
            }
            m_impl->capacity = capacity;
            m_impl->bytes = nextBytes;
            m_impl->droppedEntries += droppedCount;
            return {};
        }

        Result<std::uint64_t> Http3QpackDynamicTable::Insert(
            const std::vector<std::uint8_t>& name,
            const std::vector<std::uint8_t>& value) {
            if (!m_impl) return Status::Internal(u"HTTP/3 QPACK table is moved-from");
            auto entrySize = QpackDynamicEntrySize(name, value);
            if (!entrySize.IsOk()) return entrySize.GetStatus();
            if (entrySize.Value() > m_impl->capacity) {
                return Status(StatusCode::ResourceExhausted,
                    u"HTTP/3 QPACK dynamic entry exceeds table capacity");
            }
            if (m_impl->insertCount == std::numeric_limits<std::uint64_t>::max()) {
                return Status(StatusCode::OutOfRange,
                    u"HTTP/3 QPACK insert count is exhausted");
            }

            Http3QpackDynamicEntry entry;
            try {
                entry.absoluteIndex = m_impl->insertCount;
                entry.size = entrySize.Value();
                entry.name = name;
                entry.value = value;
            }
            catch (...) {
                return Status(StatusCode::ResourceExhausted,
                    u"HTTP/3 QPACK dynamic entry allocation failed");
            }

            const auto byteLimit = m_impl->capacity - entrySize.Value();
            std::size_t nextBytes = m_impl->bytes;
            std::size_t evictionCount = 0;
            for (auto found = m_impl->entries.rbegin();
                nextBytes > byteLimit && found != m_impl->entries.rend();
                ++found) {
                if (found->size > nextBytes) {
                    return Status::Internal(
                        u"HTTP/3 QPACK dynamic table byte ledger is inconsistent");
                }
                nextBytes -= found->size;
                ++evictionCount;
            }
            if (nextBytes > byteLimit) {
                return Status::Internal(
                    u"HTTP/3 QPACK dynamic table eviction is inconsistent");
            }
            if (m_impl->droppedEntries
                > (std::numeric_limits<std::size_t>::max)() - evictionCount) {
                return Status(StatusCode::OutOfRange,
                    u"HTTP/3 QPACK dropped-entry count is exhausted");
            }

            const auto releasedBytes = m_impl->bytes - nextBytes;
            if (m_impl->resourceBudget != nullptr) {
                if (!m_impl->BudgetMatches()) {
                    return Status(StatusCode::FailedPrecondition,
                        u"HTTP/3 QPACK dynamic-table budget is inconsistent");
                }
                if (releasedBytes != 0) {
                    auto released = m_impl->resourceBudget->ReleaseDynamicTable(
                        releasedBytes);
                    if (!released.IsOk()) return released.GetStatus();
                }
                auto reserved = m_impl->resourceBudget->ReserveDynamicTable(
                    entrySize.Value());
                if (!reserved.IsOk()) {
                    if (releasedBytes != 0
                        && !m_impl->resourceBudget->ReserveDynamicTable(
                            releasedBytes).IsOk()) {
                        return Status::Internal(
                            u"HTTP/3 QPACK dynamic-table budget rollback failed");
                    }
                    return reserved.GetStatus();
                }
            }

            try {
                m_impl->entries.push_front(std::move(entry));
            }
            catch (...) {
                if (m_impl->resourceBudget != nullptr) {
                    bool restored = m_impl->resourceBudget->ReleaseDynamicTable(
                        entrySize.Value()).IsOk();
                    if (releasedBytes != 0) {
                        restored = m_impl->resourceBudget->ReserveDynamicTable(
                            releasedBytes).IsOk() && restored;
                    }
                    if (!restored) {
                        return Status::Internal(
                            u"HTTP/3 QPACK dynamic-table budget rollback failed");
                    }
                }
                return Status(StatusCode::ResourceExhausted,
                    u"HTTP/3 QPACK dynamic entry insertion failed");
            }

            const auto absoluteIndex = m_impl->insertCount;
            while (evictionCount != 0) {
                m_impl->entries.pop_back();
                --evictionCount;
                ++m_impl->droppedEntries;
            }
            ++m_impl->insertCount;
            m_impl->bytes = nextBytes + entrySize.Value();
            return absoluteIndex;
        }

        Result<std::uint64_t> Http3QpackDynamicTable::Duplicate(
            std::uint64_t relativeIndex) {
            auto entry = GetRelative(relativeIndex);
            if (!entry.IsOk()) return entry.GetStatus();
            return Insert(entry.Value().name, entry.Value().value);
        }

        Result<void> Http3QpackDynamicTable::AttachResourceBudget(
            Http3QpackResourceBudget* budget) {
            if (!m_impl) return Status::Internal(u"HTTP/3 QPACK table is moved-from");
            if (budget == nullptr) {
                return Status::InvalidArgument(
                    u"HTTP/3 QPACK dynamic table budget is null");
            }
            if (m_impl->resourceBudget != nullptr) {
                if (m_impl->resourceBudget == budget) return {};
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 QPACK dynamic table already has a budget");
            }
            if (m_impl->capacity != 0
                || m_impl->bytes != 0
                || m_impl->droppedEntries != 0
                || m_impl->insertCount != 0
                || !m_impl->entries.empty()) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 QPACK budget must attach before table mutation");
            }
            auto usable = budget->ValidateHeaderBlock(0);
            if (!usable.IsOk()) return usable.GetStatus();
            const auto snapshot = budget->Snapshot();
            if (snapshot.dynamicTableCapacity != 0
                || snapshot.dynamicTableBytes != 0) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 QPACK budget already tracks a dynamic table");
            }
            m_impl->resourceBudget = budget;
            return {};
        }

        bool Http3QpackDynamicTable::HasResourceBudget() const noexcept {
            return m_impl != nullptr && m_impl->resourceBudget != nullptr;
        }

        Result<Http3QpackDynamicEntry> Http3QpackDynamicTable::GetAbsolute(
            std::uint64_t absoluteIndex) const {
            if (!m_impl) return Status::Internal(u"HTTP/3 QPACK table is moved-from");
            for (const auto& entry : m_impl->entries) {
                if (entry.absoluteIndex != absoluteIndex) continue;
                try {
                    return entry;
                }
                catch (...) {
                    return Status(StatusCode::ResourceExhausted,
                        u"HTTP/3 QPACK dynamic entry copy failed");
                }
            }
            return Status::NotFound(u"HTTP/3 QPACK absolute table entry is unavailable");
        }

        Result<Http3QpackDynamicEntry> Http3QpackDynamicTable::GetRelative(
            std::uint64_t relativeIndex) const {
            if (!m_impl) return Status::Internal(u"HTTP/3 QPACK table is moved-from");
            if (relativeIndex >= m_impl->entries.size()) {
                return Status::NotFound(u"HTTP/3 QPACK relative table entry is unavailable");
            }
            try {
                return m_impl->entries[static_cast<std::size_t>(relativeIndex)];
            }
            catch (...) {
                return Status(StatusCode::ResourceExhausted,
                    u"HTTP/3 QPACK dynamic entry copy failed");
            }
        }

        std::size_t Http3QpackDynamicTable::MaxCapacity() const noexcept {
            return m_impl ? m_impl->maxCapacity : 0;
        }

        Http3QpackDynamicTableSnapshot Http3QpackDynamicTable::Snapshot() const noexcept {
            if (!m_impl) return {};
            return { m_impl->capacity, m_impl->bytes, m_impl->entries.size(),
                m_impl->droppedEntries, m_impl->insertCount };
        }

        void Http3QpackDynamicTable::Reset() noexcept {
            if (!m_impl) return;
            m_impl->ReleaseBudgetUsage();
            m_impl->capacity = 0;
            m_impl->bytes = 0;
            m_impl->droppedEntries = 0;
            m_impl->insertCount = 0;
            m_impl->entries.clear();
        }

        namespace {
            Result<std::vector<std::uint8_t>> DecodeQpackInstructionString(
                const Http3QpackStringLiteral& literal) {
                if (!literal.huffmanEncoded) return literal.bytes;
                return DecodeHttp3QpackHuffman(
                    literal.bytes.data(), literal.bytes.size());
            }

            std::vector<std::uint8_t> QpackStaticBytes(const std::string& value) {
                std::vector<std::uint8_t> bytes;
                bytes.reserve(value.size());
                for (const char character : value) {
                    bytes.push_back(static_cast<std::uint8_t>(character));
                }
                return bytes;
            }
        }

        Result<void> ApplyHttp3QpackEncoderInstruction(
            const Http3QpackEncoderInstruction& instruction,
            Http3QpackDynamicTable& dynamicTable) {
            switch (instruction.type) {
            case Http3QpackEncoderInstructionType::SetDynamicTableCapacity:
                if (instruction.value > std::numeric_limits<std::size_t>::max()) {
                    return Status(StatusCode::OutOfRange,
                        u"HTTP/3 QPACK capacity exceeds platform size");
                }
                return dynamicTable.SetCapacity(
                    static_cast<std::size_t>(instruction.value));
            case Http3QpackEncoderInstructionType::InsertWithLiteralName: {
                auto name = DecodeQpackInstructionString(instruction.name);
                if (!name.IsOk()) return name.GetStatus();
                auto value = DecodeQpackInstructionString(instruction.fieldValue);
                if (!value.IsOk()) return value.GetStatus();
                auto inserted = dynamicTable.Insert(name.Value(), value.Value());
                if (!inserted.IsOk()) return inserted.GetStatus();
                return {};
            }
            case Http3QpackEncoderInstructionType::InsertWithNameReference: {
                std::vector<std::uint8_t> name;
                if (instruction.staticTable) {
                    auto entry = GetHttp3QpackStaticEntry(instruction.value);
                    if (!entry.IsOk()) return entry.GetStatus();
                    name = QpackStaticBytes(entry.Value().name);
                } else {
                    auto entry = dynamicTable.GetRelative(instruction.value);
                    if (!entry.IsOk()) return entry.GetStatus();
                    name = entry.Value().name;
                }
                auto value = DecodeQpackInstructionString(instruction.fieldValue);
                if (!value.IsOk()) return value.GetStatus();
                auto inserted = dynamicTable.Insert(name, value.Value());
                if (!inserted.IsOk()) return inserted.GetStatus();
                return {};
            }
            case Http3QpackEncoderInstructionType::Duplicate:
                {
                    auto duplicated = dynamicTable.Duplicate(instruction.value);
                    if (!duplicated.IsOk()) return duplicated.GetStatus();
                }
                return {};
            }
            return Status::InvalidArgument(
                u"HTTP/3 QPACK encoder instruction type is invalid");
        }

        struct Http3QpackEncoderStream::Impl {
            Impl(Http3QpackDynamicTable& table,
                Http3QpackStreamLimits configuredLimits)
                : dynamicTable(&table), limits(configuredLimits) { }

            Result<void> Fail(Status status) {
                terminal = true;
                error = std::move(status);
                return error;
            }

            Http3QpackDynamicTable* dynamicTable;
            Http3QpackStreamLimits limits;
            std::vector<std::uint8_t> buffer;
            std::size_t appliedInstructions = 0;
            bool terminal = false;
            bool finished = false;
            Status error;
        };

        Http3QpackEncoderStream::Http3QpackEncoderStream(
            Http3QpackDynamicTable& dynamicTable,
            Http3QpackStreamLimits limits)
            : m_impl(std::make_unique<Impl>(dynamicTable, limits)) { }

        Http3QpackEncoderStream::~Http3QpackEncoderStream() = default;

        Http3QpackEncoderStream::Http3QpackEncoderStream(
            Http3QpackEncoderStream&&) noexcept = default;

        Http3QpackEncoderStream& Http3QpackEncoderStream::operator=(
            Http3QpackEncoderStream&&) noexcept = default;

        Result<void> Http3QpackEncoderStream::Feed(
            const std::uint8_t* data,
            std::size_t size) {
            if (!m_impl) return Status(StatusCode::FailedPrecondition);
            if (m_impl->terminal) return m_impl->error;
            if (m_impl->finished) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 QPACK encoder stream is finished");
            }
            if (data == nullptr && size != 0) {
                return m_impl->Fail(Status::InvalidArgument(
                    u"HTTP/3 QPACK encoder stream input is null"));
            }
            if (size > m_impl->limits.maxBufferedBytes
                || m_impl->buffer.size()
                    > m_impl->limits.maxBufferedBytes - size) {
                return m_impl->Fail(Status(StatusCode::ResourceExhausted,
                    u"HTTP/3 QPACK encoder stream buffer limit exceeded"));
            }
            if (size != 0) {
                m_impl->buffer.insert(m_impl->buffer.end(), data, data + size);
            }
            while (!m_impl->buffer.empty()) {
                if (m_impl->appliedInstructions
                    >= m_impl->limits.maxInstructions) {
                    return m_impl->Fail(Status(StatusCode::ResourceExhausted,
                        u"HTTP/3 QPACK encoder instruction limit exceeded"));
                }
                auto parsed = ParseHttp3QpackEncoderInstruction(
                    m_impl->buffer.data(), m_impl->buffer.size());
                if (!parsed.IsOk()) {
                    if (parsed.GetStatus().Code() == StatusCode::InvalidArgument) {
                        break;
                    }
                    return m_impl->Fail(parsed.GetStatus());
                }
                const std::size_t consumed = parsed.Value().second;
                if (consumed == 0 || consumed > m_impl->buffer.size()) {
                    return m_impl->Fail(Status::Internal(
                        u"HTTP/3 QPACK encoder stream parser made no progress"));
                }
                auto applied = ApplyHttp3QpackEncoderInstruction(
                    parsed.Value().first, *m_impl->dynamicTable);
                if (!applied.IsOk()) return m_impl->Fail(applied.GetStatus());
                m_impl->buffer.erase(m_impl->buffer.begin(),
                    m_impl->buffer.begin() + static_cast<std::ptrdiff_t>(consumed));
                ++m_impl->appliedInstructions;
            }
            return {};
        }

        Result<void> Http3QpackEncoderStream::Finish() {
            if (!m_impl) return Status(StatusCode::FailedPrecondition);
            if (m_impl->terminal) return m_impl->error;
            if (m_impl->finished) return {};
            if (!m_impl->buffer.empty()) {
                return m_impl->Fail(Status::InvalidArgument(
                    u"HTTP/3 QPACK encoder stream ends with an incomplete instruction"));
            }
            m_impl->finished = true;
            return {};
        }

        Status Http3QpackEncoderStream::LastError() const noexcept {
            return m_impl ? m_impl->error : Status(StatusCode::FailedPrecondition);
        }

        Result<Http3QpackStreamQuicActions>
            Http3QpackEncoderStream::FailureActions() const {
            if (!m_impl) return Status(StatusCode::FailedPrecondition,
                u"HTTP/3 QPACK encoder stream is moved-from");
            if (m_impl->error.IsOk()) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 QPACK encoder stream has no failure");
            }
            return Http3QpackStreamQuicActions{
                static_cast<std::uint64_t>(
                    Http3QpackStreamErrorCode::EncoderStreamError), true };
        }

        Http3QpackStreamLimits Http3QpackEncoderStream::Limits() const noexcept {
            return m_impl ? m_impl->limits : Http3QpackStreamLimits{};
        }

        Http3QpackStreamSnapshot Http3QpackEncoderStream::Snapshot() const noexcept {
            if (!m_impl) return {};
            return { m_impl->buffer.size(), m_impl->appliedInstructions,
                m_impl->terminal, m_impl->finished };
        }

        void Http3QpackEncoderStream::Reset() noexcept {
            if (!m_impl) return;
            m_impl->buffer.clear();
            m_impl->appliedInstructions = 0;
            m_impl->terminal = false;
            m_impl->finished = false;
            m_impl->error = Status();
        }

        struct Http3QpackDecoderStream::Impl {
            Impl(Http3QpackSectionTracker& tracker,
                Http3QpackStreamLimits configuredLimits)
                : sectionTracker(&tracker), limits(configuredLimits) { }

            Result<void> Fail(Status status) {
                terminal = true;
                error = std::move(status);
                return error;
            }

            Http3QpackSectionTracker* sectionTracker;
            Http3QpackStreamLimits limits;
            std::vector<std::uint8_t> buffer;
            std::size_t appliedInstructions = 0;
            bool terminal = false;
            bool finished = false;
            Status error;
        };

        Http3QpackDecoderStream::Http3QpackDecoderStream(
            Http3QpackSectionTracker& sectionTracker,
            Http3QpackStreamLimits limits)
            : m_impl(std::make_unique<Impl>(sectionTracker, limits)) { }

        Http3QpackDecoderStream::~Http3QpackDecoderStream() = default;

        Http3QpackDecoderStream::Http3QpackDecoderStream(
            Http3QpackDecoderStream&&) noexcept = default;

        Http3QpackDecoderStream& Http3QpackDecoderStream::operator=(
            Http3QpackDecoderStream&&) noexcept = default;

        Result<void> Http3QpackDecoderStream::Feed(
            const std::uint8_t* data,
            std::size_t size) {
            if (!m_impl) return Status(StatusCode::FailedPrecondition);
            if (m_impl->terminal) return m_impl->error;
            if (m_impl->finished) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 QPACK decoder stream is finished");
            }
            if (data == nullptr && size != 0) {
                return m_impl->Fail(Status::InvalidArgument(
                    u"HTTP/3 QPACK decoder stream input is null"));
            }
            if (size > m_impl->limits.maxBufferedBytes
                || m_impl->buffer.size()
                    > m_impl->limits.maxBufferedBytes - size) {
                return m_impl->Fail(Status(StatusCode::ResourceExhausted,
                    u"HTTP/3 QPACK decoder stream buffer limit exceeded"));
            }
            if (size != 0) {
                m_impl->buffer.insert(m_impl->buffer.end(), data, data + size);
            }
            while (!m_impl->buffer.empty()) {
                if (m_impl->appliedInstructions
                    >= m_impl->limits.maxInstructions) {
                    return m_impl->Fail(Status(StatusCode::ResourceExhausted,
                        u"HTTP/3 QPACK decoder instruction limit exceeded"));
                }
                auto parsed = ParseHttp3QpackDecoderInstruction(
                    m_impl->buffer.data(), m_impl->buffer.size());
                if (!parsed.IsOk()) {
                    if (parsed.GetStatus().Code() == StatusCode::InvalidArgument) {
                        break;
                    }
                    return m_impl->Fail(parsed.GetStatus());
                }
                const std::size_t consumed = parsed.Value().second;
                if (consumed == 0 || consumed > m_impl->buffer.size()) {
                    return m_impl->Fail(Status::Internal(
                        u"HTTP/3 QPACK decoder stream parser made no progress"));
                }
                auto applied = ApplyHttp3QpackDecoderInstruction(
                    parsed.Value().first, *m_impl->sectionTracker);
                if (!applied.IsOk()) return m_impl->Fail(applied.GetStatus());
                m_impl->buffer.erase(m_impl->buffer.begin(),
                    m_impl->buffer.begin() + static_cast<std::ptrdiff_t>(consumed));
                ++m_impl->appliedInstructions;
            }
            return {};
        }

        Result<void> Http3QpackDecoderStream::Finish() {
            if (!m_impl) return Status(StatusCode::FailedPrecondition);
            if (m_impl->terminal) return m_impl->error;
            if (m_impl->finished) return {};
            if (!m_impl->buffer.empty()) {
                return m_impl->Fail(Status::InvalidArgument(
                    u"HTTP/3 QPACK decoder stream ends with an incomplete instruction"));
            }
            m_impl->finished = true;
            return {};
        }

        Status Http3QpackDecoderStream::LastError() const noexcept {
            return m_impl ? m_impl->error : Status(StatusCode::FailedPrecondition);
        }

        Result<Http3QpackStreamQuicActions>
            Http3QpackDecoderStream::FailureActions() const {
            if (!m_impl) return Status(StatusCode::FailedPrecondition,
                u"HTTP/3 QPACK decoder stream is moved-from");
            if (m_impl->error.IsOk()) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 QPACK decoder stream has no failure");
            }
            return Http3QpackStreamQuicActions{
                static_cast<std::uint64_t>(
                    Http3QpackStreamErrorCode::DecoderStreamError), true };
        }

        Http3QpackStreamLimits Http3QpackDecoderStream::Limits() const noexcept {
            return m_impl ? m_impl->limits : Http3QpackStreamLimits{};
        }

        Http3QpackStreamSnapshot Http3QpackDecoderStream::Snapshot() const noexcept {
            if (!m_impl) return {};
            return { m_impl->buffer.size(), m_impl->appliedInstructions,
                m_impl->terminal, m_impl->finished };
        }

        void Http3QpackDecoderStream::Reset() noexcept {
            if (!m_impl) return;
            m_impl->buffer.clear();
            m_impl->appliedInstructions = 0;
            m_impl->terminal = false;
            m_impl->finished = false;
            m_impl->error = Status();
        }

        struct Http3QpackResourceBudget::Impl {
            explicit Impl(Http3QpackLimits configuredLimits)
                : limits(configuredLimits) { }

            Http3QpackLimits limits;
            std::size_t dynamicTableCapacity = 0;
            std::size_t dynamicTableBytes = 0;
            std::unordered_set<std::uint64_t> blockedStreams;
        };

        Http3QpackResourceBudget::Http3QpackResourceBudget(Http3QpackLimits limits)
            : m_impl(std::make_unique<Impl>(limits)) { }

        Http3QpackResourceBudget::~Http3QpackResourceBudget() = default;

        Http3QpackResourceBudget::Http3QpackResourceBudget(
            Http3QpackResourceBudget&&) noexcept = default;

        Http3QpackResourceBudget& Http3QpackResourceBudget::operator=(
            Http3QpackResourceBudget&&) noexcept = default;

        Result<void> Http3QpackResourceBudget::SetDynamicTableCapacity(
            std::size_t capacity) {
            if (!m_impl) return Status::Internal(u"HTTP/3 QPACK budget is moved-from");
            if (capacity > m_impl->limits.maxDynamicTableBytes) {
                return Status(StatusCode::ResourceExhausted,
                    u"HTTP/3 QPACK dynamic table capacity exceeds configured limit");
            }
            if (capacity < m_impl->dynamicTableBytes) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 QPACK dynamic table capacity is below current usage");
            }
            m_impl->dynamicTableCapacity = capacity;
            return {};
        }

        Result<void> Http3QpackResourceBudget::ReserveDynamicTable(std::size_t bytes) {
            if (!m_impl) return Status::Internal(u"HTTP/3 QPACK budget is moved-from");
            if (bytes > m_impl->dynamicTableCapacity
                || m_impl->dynamicTableBytes > m_impl->dynamicTableCapacity - bytes) {
                return Status(StatusCode::ResourceExhausted,
                    u"HTTP/3 QPACK dynamic table budget exhausted");
            }
            m_impl->dynamicTableBytes += bytes;
            return {};
        }

        Result<void> Http3QpackResourceBudget::ReleaseDynamicTable(std::size_t bytes) {
            if (!m_impl) return Status::Internal(u"HTTP/3 QPACK budget is moved-from");
            if (bytes > m_impl->dynamicTableBytes) {
                return Status::InvalidArgument(
                    u"HTTP/3 QPACK dynamic table release exceeds current usage");
            }
            m_impl->dynamicTableBytes -= bytes;
            return {};
        }

        Result<void> Http3QpackResourceBudget::ValidateHeaderBlock(
            std::size_t bytes) const {
            if (!m_impl) return Status::Internal(u"HTTP/3 QPACK budget is moved-from");
            if (bytes > m_impl->limits.maxHeaderBlockBytes) {
                return Status(StatusCode::ResourceExhausted,
                    u"HTTP/3 QPACK header block exceeds configured limit");
            }
            return {};
        }

        Result<void> Http3QpackResourceBudget::OpenBlockedStream(
            std::uint64_t streamId) {
            if (!m_impl) return Status::Internal(u"HTTP/3 QPACK budget is moved-from");
            if (streamId > kMaxQuicStreamId) {
                return Status::InvalidArgument(
                    u"HTTP/3 QPACK blocked stream id exceeds the 62-bit limit");
            }
            if (m_impl->blockedStreams.find(streamId) != m_impl->blockedStreams.end()) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 QPACK blocked stream is already open");
            }
            if (m_impl->blockedStreams.size() >= m_impl->limits.maxBlockedStreams) {
                return Status(StatusCode::ResourceExhausted,
                    u"HTTP/3 QPACK blocked stream limit exceeded");
            }
            try {
                m_impl->blockedStreams.insert(streamId);
            }
            catch (...) {
                return Status(StatusCode::ResourceExhausted,
                    u"HTTP/3 QPACK blocked-stream allocation failed");
            }
            return {};
        }

        Result<void> Http3QpackResourceBudget::CloseBlockedStream(
            std::uint64_t streamId) {
            if (!m_impl) return Status::Internal(u"HTTP/3 QPACK budget is moved-from");
            if (m_impl->blockedStreams.erase(streamId) == 0) {
                return Status::InvalidArgument(
                    u"HTTP/3 QPACK blocked stream is not open");
            }
            return {};
        }

        bool Http3QpackResourceBudget::IsBlockedStreamOpen(
            std::uint64_t streamId) const noexcept {
            return m_impl != nullptr
                && m_impl->blockedStreams.find(streamId)
                    != m_impl->blockedStreams.end();
        }

        Http3QpackLimits Http3QpackResourceBudget::Limits() const noexcept {
            return m_impl ? m_impl->limits : Http3QpackLimits{};
        }

        Http3QpackResourceSnapshot Http3QpackResourceBudget::Snapshot() const noexcept {
            if (!m_impl) return {};
            return { m_impl->dynamicTableCapacity, m_impl->dynamicTableBytes,
                m_impl->blockedStreams.size(), m_impl->limits.maxBlockedStreams };
        }

        void Http3QpackResourceBudget::Reset() noexcept {
            if (!m_impl) return;
            m_impl->dynamicTableCapacity = 0;
            m_impl->dynamicTableBytes = 0;
            m_impl->blockedStreams.clear();
        }
    }
}
