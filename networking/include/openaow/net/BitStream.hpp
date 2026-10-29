#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace OpenArtOfWar::Net {

// Bits needed for values 0..maxValue
constexpr int bitsFor(std::uint32_t maxValue) {
    int bits = 0;
    while (maxValue) {
        ++bits;
        maxValue >>= 1;
    }
    return bits;
}

// Packs values bit by bit (least significant first) into a buffer the caller owns: no allocation per
// packet. Writing past the end sets overflowed() and drops the rest; the packet must then not be sent.
class BitWriter {
public:
    BitWriter(std::uint8_t* buffer, std::size_t capacity) : m_data(buffer), m_capacity(capacity) {}

    // Low `bits` (0..32) bits of `value`
    void writeBits(std::uint32_t value, int bits) {
        if (bits <= 0) return;
        if (bits < 32) value &= (1u << bits) - 1u;
        m_scratch |= static_cast<std::uint64_t>(value) << m_scratchBits;
        m_scratchBits += bits;
        while (m_scratchBits >= 8) {
            putByte(static_cast<std::uint8_t>(m_scratch));
            m_scratch >>= 8;
            m_scratchBits -= 8;
        }
    }
    void writeBool(bool v) { writeBits(v ? 1u : 0u, 1); }
    // `value` in [lo, hi] with just the bits the range needs
    void writeRanged(std::int32_t value, std::int32_t lo, std::int32_t hi) {
        if (value < lo) value = lo;
        if (value > hi) value = hi;
        writeBits(static_cast<std::uint32_t>(value - lo), bitsFor(static_cast<std::uint32_t>(hi - lo)));
    }
    // LEB128 in 8-bit groups: 1 byte below 128, 2 below 16384, ...
    void writeVarUint(std::uint64_t v) {
        while (v >= 0x80u) {
            writeBits(static_cast<std::uint32_t>(v & 0x7Fu) | 0x80u, 8);
            v >>= 7;
        }
        writeBits(static_cast<std::uint32_t>(v), 8);
    }
    // Zig-zag: small negative numbers stay short
    void writeVarInt(std::int64_t v) {
        writeVarUint((static_cast<std::uint64_t>(v) << 1) ^ static_cast<std::uint64_t>(v >> 63));
    }
    void writeU8(std::uint8_t v) { writeBits(v, 8); }
    void writeU16(std::uint16_t v) { writeBits(v, 16); }
    void writeU32(std::uint32_t v) { writeBits(v, 32); }
    void writeU64(std::uint64_t v) {
        writeBits(static_cast<std::uint32_t>(v), 32);
        writeBits(static_cast<std::uint32_t>(v >> 32), 32);
    }
    void writeFloat(float v) {   // exact bit pattern (simulation values must survive unchanged)
        std::uint32_t u;
        std::memcpy(&u, &v, sizeof u);
        writeU32(u);
    }
    void writeBytes(const void* data, std::size_t size) {
        alignToByte();
        const auto* p = static_cast<const std::uint8_t*>(data);
        if (m_overflow || size > m_capacity - m_bytes) {
            m_overflow = true;
            return;
        }
        std::memcpy(m_data + m_bytes, p, size);
        m_bytes += size;
    }
    // Length-prefixed; longer strings are cut to `maxLength` bytes
    void writeString(const std::string& s, std::size_t maxLength) {
        const std::size_t n = s.size() < maxLength ? s.size() : maxLength;
        writeVarUint(n);
        writeBytes(s.data(), n);
    }
    // Exp-Golomb (order 0): 0 -> 1 bit, 1..2 -> 3 bits, 3..6 -> 5 bits ...; for small, mostly tiny numbers
    void writeExpGolomb(std::uint32_t v) {
        const std::uint64_t x = static_cast<std::uint64_t>(v) + 1u;
        int bits = 0;
        while ((x >> bits) > 1u) ++bits;
        writeBits(0, bits);   // `bits` zeros, then x itself (its top bit is the 1 that ends the zeros)
        // x MSB-first, so the reader meets the terminating 1 right after the zeros
        for (int i = bits; i >= 0; --i) writeBits(static_cast<std::uint32_t>((x >> i) & 1u), 1);
    }
    // Ascending entity IDs as first value + gaps: a selection of 40 neighbouring units costs ~10 bytes
    // instead of 160. The list must be sorted and free of duplicates.
    void writeIdList(const std::vector<std::uint32_t>& sortedIds) {
        writeVarUint(sortedIds.size());
        std::uint32_t previous = 0;
        for (std::size_t i = 0; i < sortedIds.size(); ++i) {
            if (i == 0) writeVarUint(sortedIds[0]);
            else writeExpGolomb(sortedIds[i] - previous - 1);
            previous = sortedIds[i];
        }
    }
    void alignToByte() {
        if (m_scratchBits > 0) writeBits(0, 8 - m_scratchBits);
    }

    // Flushes the partial byte; the number of bytes the packet takes
    std::size_t finish() {
        alignToByte();
        return m_bytes;
    }
    std::size_t bitsWritten() const { return m_bytes * 8 + static_cast<std::size_t>(m_scratchBits); }
    bool overflowed() const { return m_overflow; }
    const std::uint8_t* data() const { return m_data; }

private:
    void putByte(std::uint8_t b) {
        if (m_bytes >= m_capacity) {
            m_overflow = true;
            return;
        }
        m_data[m_bytes++] = b;
    }

    std::uint8_t* m_data;
    std::size_t m_capacity;
    std::size_t m_bytes{0};
    std::uint64_t m_scratch{0};
    int m_scratchBits{0};
    bool m_overflow{false};
};

// Reads what BitWriter wrote, straight from the received buffer (zero-copy). Every read is checked:
// reading past the end, an over-long varint or a too large count sets failed() and returns zeros, so
// a decoder can read the whole message and look at failed() once at the end.
class BitReader {
public:
    BitReader(const std::uint8_t* data, std::size_t size) : m_data(data), m_size(size) {}

    std::uint32_t readBits(int bits) {
        if (bits <= 0) return 0;
        while (m_scratchBits < bits) {
            if (m_pos >= m_size) {
                m_failed = true;
                m_scratchBits = bits;   // stop reading; the zeros below are returned from here on
                m_scratch = 0;
                break;
            }
            m_scratch |= static_cast<std::uint64_t>(m_data[m_pos++]) << m_scratchBits;
            m_scratchBits += 8;
        }
        const std::uint32_t v = bits < 32 ? static_cast<std::uint32_t>(m_scratch & ((1ull << bits) - 1u))
                                          : static_cast<std::uint32_t>(m_scratch);
        m_scratch >>= bits;
        m_scratchBits -= bits;
        return m_failed ? 0u : v;
    }
    bool readBool() { return readBits(1) != 0; }
    std::int32_t readRanged(std::int32_t lo, std::int32_t hi) {
        const std::uint32_t raw = readBits(bitsFor(static_cast<std::uint32_t>(hi - lo)));
        const std::int64_t v = static_cast<std::int64_t>(lo) + raw;
        if (v > hi) {
            m_failed = true;
            return lo;
        }
        return static_cast<std::int32_t>(v);
    }
    std::uint64_t readVarUint() {
        std::uint64_t v = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            const std::uint32_t b = readBits(8);
            v |= static_cast<std::uint64_t>(b & 0x7Fu) << shift;
            if (!(b & 0x80u)) return m_failed ? 0 : v;
        }
        m_failed = true;   // more than 10 groups: not a varint
        return 0;
    }
    std::int64_t readVarInt() {
        const std::uint64_t u = readVarUint();
        return static_cast<std::int64_t>(u >> 1) ^ -static_cast<std::int64_t>(u & 1u);
    }
    std::uint8_t readU8() { return static_cast<std::uint8_t>(readBits(8)); }
    std::uint16_t readU16() { return static_cast<std::uint16_t>(readBits(16)); }
    std::uint32_t readU32() { return readBits(32); }
    std::uint64_t readU64() {
        const std::uint64_t lo = readBits(32);
        return lo | static_cast<std::uint64_t>(readBits(32)) << 32;
    }
    float readFloat() {
        const std::uint32_t u = readU32();
        float v;
        std::memcpy(&v, &u, sizeof v);
        return v;
    }
    // A view into the buffer (no copy); nullptr when it is not all there
    const std::uint8_t* readBytes(std::size_t size) {
        alignToByte();
        if (m_failed || size > m_size - m_pos) {
            m_failed = true;
            return nullptr;
        }
        const std::uint8_t* p = m_data + m_pos;
        m_pos += size;
        return p;
    }
    std::string readString(std::size_t maxLength) {
        const std::uint64_t n = readVarUint();
        if (n > maxLength) {
            m_failed = true;
            return {};
        }
        const std::uint8_t* p = readBytes(static_cast<std::size_t>(n));
        return p ? std::string(reinterpret_cast<const char*>(p), static_cast<std::size_t>(n)) : std::string();
    }
    std::uint32_t readExpGolomb() {
        int zeros = 0;
        while (!m_failed && readBits(1) == 0) {
            if (++zeros > 32) {
                m_failed = true;
                return 0;
            }
        }
        std::uint64_t x = 1;
        for (int i = 0; i < zeros; ++i) x = (x << 1) | readBits(1);
        if (m_failed || x - 1u > 0xFFFFFFFFull) {
            m_failed = true;
            return 0;
        }
        return static_cast<std::uint32_t>(x - 1u);
    }
    // `maxCount` caps what a hostile packet can make us allocate
    bool readIdList(std::vector<std::uint32_t>& out, std::size_t maxCount) {
        out.clear();
        const std::uint64_t n = readVarUint();
        if (n > maxCount || n > remainingBytes() * 8 + 8) {   // every ID takes at least one bit
            m_failed = true;
            return false;
        }
        out.reserve(static_cast<std::size_t>(n));
        std::uint64_t previous = 0;
        for (std::uint64_t i = 0; i < n && !m_failed; ++i) {
            const std::uint64_t id = i == 0 ? readVarUint() : previous + readExpGolomb() + 1;
            if (id > 0xFFFFFFFFull) {
                m_failed = true;
                break;
            }
            out.push_back(static_cast<std::uint32_t>(id));
            previous = id;
        }
        if (m_failed) out.clear();
        return !m_failed;
    }
    void alignToByte() {
        m_scratch = 0;
        m_scratchBits = 0;
    }

    bool failed() const { return m_failed; }
    void fail() { m_failed = true; }
    std::size_t remainingBytes() const { return m_size - m_pos; }
    // Everything read and nothing but padding left over
    bool atEnd() const { return m_pos == m_size && (m_scratch == 0); }

private:
    const std::uint8_t* m_data;
    std::size_t m_size;
    std::size_t m_pos{0};
    std::uint64_t m_scratch{0};
    int m_scratchBits{0};
    bool m_failed{false};
};

} // namespace OpenArtOfWar::Net
