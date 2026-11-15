#include "stonekv/db.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <condition_variable>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <list>
#include <map>
#include <mutex>
#include <optional>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace stonekv {
namespace {
namespace fs = std::filesystem;
constexpr std::size_t kMaxKey = 4096;
constexpr std::size_t kMaxValue = 8U * 1024U * 1024U;
constexpr std::size_t kWalHead = 25; // magic4 seq8 kind1 keylen4 vallen4 crc4
constexpr std::size_t kBatchHead = 24; // magic4 seq8 count4 payloadlen4 crc4
constexpr std::size_t kWalV2Head = kWalHead + 4; // V1 fields + CRC32 of complete V1-sized header
constexpr std::size_t kBatchV2Head = kBatchHead + 4;
constexpr std::size_t kMaxBatchOperations = 4096;
constexpr std::size_t kMaxBatchBytes = 32U * 1024U * 1024U;
constexpr std::size_t kSstHead = 21; // seq8 kind1 keylen4 vallen4 crc4
constexpr const char* kSstMagic = "STONEKV1"; // 8 bytes
constexpr const char* kSstBlockMagic = "STONEKV2";
constexpr const char* kSstIndexedMagic = "STONEKV3";
constexpr const char* kSstFooterMagic = "SKV3TAIL";
constexpr std::size_t kSstBlockTarget = 64U * 1024U;
constexpr std::size_t kSstBlockMax = kMaxValue + kMaxKey + kSstHead;
constexpr std::size_t kSstMetadataMax = 256U * 1024U * 1024U;
constexpr const char* kManifestMagic = "STONEKV_MANIFEST_V1";
constexpr std::uint8_t kPut = 1;
constexpr std::uint8_t kDelete = 2;

[[noreturn]] void fail(const std::string& why) { throw std::runtime_error("StoneKV: " + why); }
std::string err(const std::string& where) { return where + ": " + std::strerror(errno); }

struct FD {
    int fd{-1};
    explicit FD(int n = -1) : fd(n) {}
    ~FD() { if (fd >= 0) ::close(fd); }
    FD(const FD&) = delete;
    FD& operator=(const FD&) = delete;
    FD(FD&& other) noexcept : fd(std::exchange(other.fd, -1)) {}
    FD& operator=(FD&& other) noexcept {
        if (this != &other) {
            if (fd >= 0) ::close(fd);
            fd = std::exchange(other.fd, -1);
        }
        return *this;
    }
};

void writeAll(int fd, const void* data, std::size_t size) {
    const auto* ptr = static_cast<const unsigned char*>(data);
    while (size != 0) {
        const ssize_t n = ::write(fd, ptr, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) fail(err("write"));
        ptr += n;
        size -= static_cast<std::size_t>(n);
    }
}

void preadAll(int fd, void* data, std::size_t size, std::uint64_t off) {
    auto* ptr = static_cast<unsigned char*>(data);
    while (size != 0) {
        const auto n = ::pread(fd, ptr, size, static_cast<off_t>(off));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) fail(n == 0 ? "unexpected EOF" : err("pread"));
        ptr += n;
        size -= static_cast<std::size_t>(n);
        off += static_cast<std::uint64_t>(n);
    }
}

void pwriteAll(int fd, const void* data, std::size_t size, std::uint64_t off) {
    const auto* ptr = static_cast<const unsigned char*>(data);
    while (size != 0) {
        const auto n = ::pwrite(fd, ptr, size, static_cast<off_t>(off));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) fail(err("pwrite"));
        ptr += n;
        size -= static_cast<std::size_t>(n);
        off += static_cast<std::uint64_t>(n);
    }
}
void syncFile(int fd) { if (::fsync(fd) < 0) fail(err("fsync")); }
void syncDir(const fs::path& path) {
    FD fd(::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (fd.fd < 0) fail(err("open directory"));
    syncFile(fd.fd);
}
// Persist each newly created directory entry before creating its descendants.
void createDirectoriesDurably(const fs::path& path) {
    fs::path current = path.root_path();
    for (const auto& component : path.relative_path()) {
        current /= component;
        std::error_code ec;
        const bool created = fs::create_directory(current, ec);
        if (ec) fail("create directory " + current.string() + ": " + ec.message());
        if (created) syncDir(current.parent_path());
        else if (!fs::is_directory(current)) fail("database path contains a non-directory");
    }
}
std::uint64_t fileSize(int fd) {
    struct stat st{};
    if (::fstat(fd, &st) != 0) fail(err("fstat"));
    if (st.st_size < 0) fail("negative file size");
    return static_cast<std::uint64_t>(st.st_size);
}
void truncateFile(int fd, std::uint64_t len) {
    if (::ftruncate(fd, static_cast<off_t>(len)) != 0) fail(err("ftruncate"));
    syncFile(fd);
}
void put32(std::string& s, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xffU));
}
void put64(std::string& s, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xffU));
}
std::uint32_t get32(const char* p) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(static_cast<unsigned char>(p[i])) << (8 * i);
    return v;
}
std::uint64_t get64(const char* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(static_cast<unsigned char>(p[i])) << (8 * i);
    return v;
}

const std::array<std::uint32_t, 256>& crcTable() {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t v = i;
            for (int j = 0; j < 8; ++j) v = (v >> 1U) ^ ((v & 1U) ? 0xedb88320U : 0U);
            t[i] = v;
        }
        return t;
    }();
    return table;
}
std::uint32_t crcUpdate(std::uint32_t crc, const void* data, std::size_t len) {
    const auto* p = static_cast<const unsigned char*>(data);
    const auto& t = crcTable();
    for (std::size_t i = 0; i < len; ++i) crc = t[(crc ^ p[i]) & 0xffU] ^ (crc >> 8U);
    return crc;
}
std::uint32_t crc32(const std::string& data) {
    return ~crcUpdate(0xffffffffU, data.data(), data.size());
}
std::uint32_t crcParts(const char* h, std::size_t hlen, const std::string& k, const std::string& v) {
    std::uint32_t c = crcUpdate(0xffffffffU, h, hlen);
    c = crcUpdate(c, k.data(), k.size());
    c = crcUpdate(c, v.data(), v.size());
    return ~c;
}

std::uint64_t hash64(const std::string& s) {
    std::uint64_t v = 14695981039346656037ULL;
    for (char ch : s) { v ^= static_cast<unsigned char>(ch); v *= 1099511628211ULL; }
    return v;
}
struct Bloom {
    std::vector<std::uint8_t> bits;
    std::size_t total_bits{0};
    void Initialize(std::size_t keys) {
        if (keys > (std::numeric_limits<std::size_t>::max() - 7) / 10) fail("Bloom size overflow");
        total_bits = std::max<std::size_t>(64, keys * 10);
        bits.assign((total_bits + 7) / 8, 0);
    }
    void Add(const std::string& key) {
        AddHash(hash64(key));
    }
    void AddHash(std::uint64_t h1) {
        const auto h2 = (h1 >> 17U) | 1ULL;
        for (std::uint64_t i = 0; i < 7; ++i) {
            const auto pos = static_cast<std::size_t>((h1 + i * h2) % total_bits);
            bits[pos >> 3U] = static_cast<std::uint8_t>(bits[pos >> 3U] | (1U << (pos & 7U)));
        }
    }
    bool MayContain(const std::string& key) const {
        if (total_bits == 0) return false;
        const auto h1 = hash64(key);
        const auto h2 = (h1 >> 17U) | 1ULL;
        for (std::uint64_t i = 0; i < 7; ++i) {
            const auto pos = static_cast<std::size_t>((h1 + i * h2) % total_bits);
            if ((bits[pos >> 3U] & (1U << (pos & 7U))) == 0) return false;
        }
        return true;
    }
};

struct Record { std::uint64_t seq; bool deleted; std::string value; };
struct IndexEntry {
    std::string key; std::uint64_t offset; std::uint64_t seq; bool deleted;
    std::uint64_t block_offset{0}; std::uint32_t block_bytes{0}; std::uint32_t block_crc{0};
};
struct SparseBlock { std::uint64_t offset; std::uint32_t bytes; std::string first_key; };
struct SSTable {
    std::uint64_t id{0};
    fs::path path;
    std::vector<IndexEntry> index;
    Bloom bloom;
    std::vector<SparseBlock> sparse_blocks;
};
std::string sstFilename(std::uint64_t id) {
    std::ostringstream s;
    s << "SST-" << std::setfill('0') << std::setw(12) << id << ".sst";
    return s.str();
}
void checkBounds(std::size_t k, std::size_t v) {
    if (k == 0 || k > kMaxKey) fail("key must have 1..4096 bytes");
    if (v > kMaxValue) fail("value exceeds 8 MiB limit");
}
void maybeCrash(const char* point) {
    const char* configured = std::getenv("STONEKV_FAILPOINT");
    if (configured != nullptr && std::strcmp(configured, point) == 0) ::_exit(86);
}

// Writer keeps memory bounded in SST compaction; the index is rebuilt by validation.
class SSTWriter {
public:
    explicit SSTWriter(const fs::path& final_path)
        : final_(final_path), temp_(final_path.string() + ".tmp"),
          out_(::open(temp_.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644)) {
        if (out_.fd < 0) fail(err("create SST"));
        std::string head(kSstIndexedMagic, 8);
        put64(head, 0);
        writeAll(out_.fd, head.data(), head.size());
    }
    void Append(const std::string& key, const Record& r) {
        checkBounds(key.size(), r.value.size());
        if (!prev_.empty() && !(prev_ < key)) fail("SST writer keys not ordered");
        std::string head;
        put64(head, r.seq);
        head.push_back(static_cast<char>(r.deleted ? kDelete : kPut));
        put32(head, static_cast<std::uint32_t>(key.size()));
        put32(head, static_cast<std::uint32_t>(r.value.size()));
        const std::uint32_t crc = crcParts(head.data(), head.size(), key, r.value);
        put32(head, crc);
        const auto record_size = head.size() + key.size() + r.value.size();
        if (!block_.empty() && block_.size() + record_size > kSstBlockTarget) flushBlock();
        if (block_.empty()) block_first_ = key;
        block_ += head;
        block_ += key;
        block_ += r.value;
        hashes_.push_back(hash64(key));
        ++count_;
        prev_ = key;
    }
    void Finish() {
        flushBlock();
        Bloom bloom; bloom.Initialize(hashes_.size());
        for (auto h : hashes_) bloom.AddHash(h);
        if (blocks_.size() > std::numeric_limits<std::uint32_t>::max() ||
            bloom.bits.size() > std::numeric_limits<std::uint32_t>::max()) fail("SST metadata too large");
        std::string meta("IDX3", 4);
        put32(meta, static_cast<std::uint32_t>(blocks_.size()));
        put64(meta, bloom.total_bits);
        put32(meta, static_cast<std::uint32_t>(bloom.bits.size()));
        for (const auto& b : blocks_) {
            put64(meta, b.offset); put32(meta, b.bytes);
            put32(meta, static_cast<std::uint32_t>(b.first_key.size())); meta += b.first_key;
            if (meta.size() > kSstMetadataMax) fail("SST metadata too large");
        }
        if (bloom.bits.size() > kSstMetadataMax - meta.size()) fail("SST metadata too large");
        meta.append(reinterpret_cast<const char*>(bloom.bits.data()), bloom.bits.size());
        std::string footer(kSstFooterMagic, 8);
        put64(footer, file_offset_); put64(footer, meta.size()); put32(footer, crc32(meta));
        writeAll(out_.fd, meta.data(), meta.size()); writeAll(out_.fd, footer.data(), footer.size());
        std::string count;
        put64(count, count_);
        pwriteAll(out_.fd, count.data(), count.size(), 8);
        syncFile(out_.fd);
        // Close before rename: fsync has already completed.
        out_ = FD{};
        if (::rename(temp_.c_str(), final_.c_str()) != 0) fail(err("rename SST"));
        syncDir(final_.parent_path());
        finished_ = true;
    }
    ~SSTWriter() { if (!finished_) { std::error_code ec; fs::remove(temp_, ec); } }
private:
    void flushBlock() {
        if (block_.empty()) return;
        std::string header;
        put32(header, static_cast<std::uint32_t>(block_.size()));
        put32(header, crc32(block_));
        writeAll(out_.fd, header.data(), header.size());
        writeAll(out_.fd, block_.data(), block_.size());
        blocks_.push_back({file_offset_, static_cast<std::uint32_t>(block_.size()), std::move(block_first_)});
        file_offset_ += header.size() + block_.size();
        block_.clear();
    }
    fs::path final_, temp_;
    FD out_;
    std::string prev_;
    std::string block_;
    std::string block_first_;
    std::vector<std::uint64_t> hashes_;
    std::vector<SparseBlock> blocks_;
    std::uint64_t file_offset_{16};
    std::uint64_t count_{0};
    bool finished_{false};
};

SSTable loadSST(const fs::path& path, std::uint64_t id) {
    FD fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
    if (fd.fd < 0) fail(err("open SST " + path.string()));
    const auto size = fileSize(fd.fd);
    if (size < 16) fail("short SST: " + path.string());
    char head[16];
    preadAll(fd.fd, head, sizeof(head), 0);
    const bool indexed_format = std::memcmp(head, kSstIndexedMagic, 8) == 0;
    const bool block_format = indexed_format || std::memcmp(head, kSstBlockMagic, 8) == 0;
    if (!block_format && std::memcmp(head, kSstMagic, 8) != 0) fail("invalid SST magic or unsupported version");
    const auto count = get64(head + 8);
    if (count > (size - 16) / (kSstHead + 1)) fail("invalid SST count");
    SSTable table;
    table.id = id;
    table.path = path;
    table.index.reserve(static_cast<std::size_t>(count));
    std::uint64_t data_end = size;
    if (indexed_format) {
        if (size < 16 + 28) fail("short indexed SST footer");
        char footer[28]; preadAll(fd.fd, footer, sizeof(footer), size - sizeof(footer));
        if (std::memcmp(footer, kSstFooterMagic, 8) != 0) fail("invalid SST footer magic");
        const auto meta_offset = get64(footer + 8), meta_length = get64(footer + 16);
        if (meta_offset < 16 || meta_offset > size - sizeof(footer) ||
            meta_length != size - sizeof(footer) - meta_offset || meta_length < 20 || meta_length > kSstMetadataMax)
            fail("invalid SST metadata bounds");
        std::string meta(static_cast<std::size_t>(meta_length), '\0');
        preadAll(fd.fd, meta.data(), meta.size(), meta_offset);
        if (crc32(meta) != get32(footer + 24) || std::memcmp(meta.data(), "IDX3", 4) != 0)
            fail("SST metadata CRC or magic mismatch");
        const auto blocks = get32(meta.data() + 4);
        const auto bloom_bits = get64(meta.data() + 8);
        const auto bloom_bytes = get32(meta.data() + 16);
        if (bloom_bits < 64 || bloom_bits > kSstMetadataMax * 8ULL ||
            bloom_bytes != (bloom_bits + 7) / 8 || blocks > (meta.size() - 20) / 17)
            fail("invalid SST sparse index or Bloom size");
        std::size_t pos = 20;
        std::string prior_key;
        for (std::uint32_t i = 0; i < blocks; ++i) {
            if (meta.size() - pos < 16) fail("truncated SST sparse index");
            const auto offset = get64(meta.data() + pos);
            const auto bytes = get32(meta.data() + pos + 8), keylen = get32(meta.data() + pos + 12);
            pos += 16;
            if (keylen == 0 || keylen > kMaxKey || keylen > meta.size() - pos || bytes == 0 || bytes > kSstBlockMax)
                fail("invalid SST sparse index entry");
            std::string first(meta.data() + pos, keylen); pos += keylen;
            if (!prior_key.empty() && !(prior_key < first)) fail("unsorted SST sparse index");
            prior_key = first; table.sparse_blocks.push_back({offset, bytes, std::move(first)});
        }
        if (bloom_bytes != meta.size() - pos) fail("invalid SST Bloom payload size");
        table.bloom.total_bits = static_cast<std::size_t>(bloom_bits);
        table.bloom.bits.assign(meta.begin() + static_cast<std::ptrdiff_t>(pos), meta.end());
        data_end = meta_offset;
    } else table.bloom.Initialize(static_cast<std::size_t>(count));
    std::uint64_t off = 16;
    std::uint64_t block_end = off;
    std::uint64_t block_start = 0;
    std::uint32_t block_bytes = 0, block_checksum = 0;
    std::size_t block_number = 0;
    bool first_in_block = false;
    std::string prev;
    for (std::uint64_t i = 0; i < count; ++i) {
        if (block_format && off == block_end) {
            if (off > data_end || data_end - off < 8) fail("truncated SST block header");
            char bh[8]; preadAll(fd.fd, bh, sizeof(bh), off);
            const auto bytes = get32(bh);
            if (bytes == 0 || bytes > kSstBlockMax || data_end - off - 8 < bytes) fail("invalid SST block length");
            if (indexed_format && (block_number >= table.sparse_blocks.size() ||
                table.sparse_blocks[block_number].offset != off || table.sparse_blocks[block_number].bytes != bytes))
                fail("SST sparse index/block mismatch");
            std::string block(bytes, '\0');
            preadAll(fd.fd, block.data(), block.size(), off + 8);
            if (crc32(block) != get32(bh + 4)) fail("SST block CRC mismatch");
            off += 8;
            block_start = off; block_bytes = bytes; block_checksum = get32(bh + 4);
            block_end = off + bytes;
            first_in_block = true;
        }
        const auto limit = block_format ? block_end : size;
        if (off > limit || limit - off < kSstHead) fail("truncated SST record header");
        char rh[kSstHead];
        preadAll(fd.fd, rh, sizeof(rh), off);
        const auto seq = get64(rh);
        const auto kind = static_cast<std::uint8_t>(rh[8]);
        const auto klen = get32(rh + 9), vlen = get32(rh + 13);
        if (kind != kPut && kind != kDelete) fail("invalid SST operation");
        checkBounds(klen, vlen);
        if (limit - off - kSstHead < static_cast<std::uint64_t>(klen) + vlen)
            fail("truncated SST record payload");
        std::string key(klen, '\0'), value(vlen, '\0');
        preadAll(fd.fd, key.data(), key.size(), off + kSstHead);
        preadAll(fd.fd, value.data(), value.size(), off + kSstHead + klen);
        if (crcParts(rh, 17, key, value) != get32(rh + 17)) fail("SST CRC mismatch");
        if (!prev.empty() && !(prev < key)) fail("SST keys unsorted/duplicated");
        if (indexed_format && first_in_block && table.sparse_blocks[block_number].first_key != key)
            fail("SST sparse index first key mismatch");
        if (first_in_block) { ++block_number; first_in_block = false; }
        table.index.push_back({key, off, seq, kind == kDelete, block_start, block_bytes, block_checksum});
        if (indexed_format) { if (!table.bloom.MayContain(key)) fail("SST Bloom false negative"); }
        else table.bloom.Add(key);
        prev = std::move(key);
        off += kSstHead + static_cast<std::uint64_t>(klen) + vlen;
    }
    if (off != data_end || (block_format && off != block_end) ||
        (indexed_format && block_number != table.sparse_blocks.size())) fail("unexpected trailing bytes in SST");
    return table;
}
Record readAt(const SSTable& table, const IndexEntry& item, int reader_fd = -1) {
    FD opened(reader_fd < 0 ? ::open(table.path.c_str(), O_RDONLY | O_CLOEXEC) : -1);
    if (reader_fd < 0 && opened.fd < 0) fail(err("read SST"));
    const int fd = reader_fd >= 0 ? reader_fd : opened.fd;
    char h[kSstHead];
    preadAll(fd, h, sizeof(h), item.offset);
    const auto seq = get64(h);
    const auto kind = static_cast<std::uint8_t>(h[8]);
    const auto klen = get32(h + 9), vlen = get32(h + 13);
    if (klen != item.key.size() || seq != item.seq || (kind == kDelete) != item.deleted ||
        (kind != kPut && kind != kDelete) || vlen > kMaxValue) fail("SST index/data mismatch");
    std::string key(klen, '\0'), value(vlen, '\0');
    preadAll(fd, key.data(), key.size(), item.offset + kSstHead);
    preadAll(fd, value.data(), value.size(), item.offset + kSstHead + klen);
    if (key != item.key || get32(h + 17) != crcParts(h, 17, key, value)) fail("SST read CRC mismatch");
    return {seq, kind == kDelete, std::move(value)};
}

struct Manifest {
    std::uint64_t durable_seq{0};
    std::uint64_t next_id{1};
    std::vector<std::uint64_t> files;
};
void saveManifest(const fs::path& dir, const Manifest& m) {
    std::ostringstream ss;
    ss << kManifestMagic << '\n' << m.durable_seq << ' ' << m.next_id << ' ' << m.files.size() << '\n';
    for (auto id : m.files) ss << id << '\n';
    std::string content = ss.str();
    std::ostringstream checksum;
    checksum << "crc32 " << std::hex << std::setw(8) << std::setfill('0') << crc32(content) << '\n';
    content += checksum.str();
    const fs::path tmp = dir / "MANIFEST.tmp";
    FD fd(::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644));
    if (fd.fd < 0) fail(err("create MANIFEST"));
    writeAll(fd.fd, content.data(), content.size());
    syncFile(fd.fd);
    fd = FD{};
    if (::rename(tmp.c_str(), (dir / "MANIFEST").c_str()) != 0) fail(err("rename MANIFEST"));
    syncDir(dir);
}
Manifest readManifest(const fs::path& dir) {
    std::ifstream in(dir / "MANIFEST", std::ios::binary);
    if (!in) fail("cannot read MANIFEST");
    const std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (data.size() > 1024 * 1024) fail("MANIFEST too large");
    const auto pos = data.rfind("crc32 ");
    if (pos == std::string::npos || (pos != 0 && data[pos - 1] != '\n')) fail("MANIFEST missing checksum");
    const std::string payload = data.substr(0, pos);
    std::istringstream check(data.substr(pos + 6));
    std::uint32_t expected = 0;
    if (!(check >> std::hex >> expected) || expected != crc32(payload)) fail("MANIFEST checksum mismatch");
    check >> std::ws;
    if (!check.eof()) fail("MANIFEST has trailing data after checksum");
    std::istringstream ss(payload);
    std::string magic;
    Manifest m;
    std::size_t count = 0;
    if (!(ss >> magic) || magic != kManifestMagic || !(ss >> m.durable_seq >> m.next_id >> count) ||
        count > 100000 || m.next_id == 0) fail("bad MANIFEST header");
    for (std::size_t i = 0; i < count; ++i) {
        std::uint64_t id = 0;
        if (!(ss >> id) || id == 0 || id >= m.next_id) fail("invalid MANIFEST SST id");
        m.files.push_back(id);
    }
    std::string unexpected;
    if (ss >> unexpected) fail("MANIFEST has trailing fields");
    std::unordered_set<std::uint64_t> unique(m.files.begin(), m.files.end());
    if (unique.size() != m.files.size()) fail("duplicate SST in MANIFEST");
    return m;
}

struct SourceEntry {
    std::string key;
    std::uint64_t seq;
    bool deleted;
    std::size_t source; // 0: memtable; >=1: table index+1
    std::size_t index;
};
struct KeyGreater {
    bool operator()(const SourceEntry& a, const SourceEntry& b) const { return a.key > b.key; }
};
} // namespace

struct DB::Impl {
    fs::path dir;
    Options opt;
    FD lock_fd;
    FD wal_fd;
    mutable std::mutex mutex;
    mutable std::condition_variable cv;
    Manifest manifest;
    std::uint64_t latest_seq{0};
    bool poisoned{false}; // 出现 I/O 提交异常后拒绝继续操作，避免重复序号或状态分叉
    std::map<std::string, Record> mem;
    std::size_t mem_bytes{0};
    std::unique_ptr<std::map<std::string, Record>> immutable;
    std::uint64_t immutable_seq{0};
    std::size_t immutable_bytes{0};
    bool barrier{false};
    bool shutdown{false};
    bool compaction_pending{false};
    std::uint64_t compaction_count{0};
    std::uint64_t compaction_micros{0};
    std::uint64_t compaction_output_bytes{0};
    std::string background_error;
    std::thread flush_worker;
    std::vector<SSTable> tables;
    // A bounded LRU of descriptors used only under mutex; compaction has its
    // own short-lived readers. Keeping 32 avoids an unbounded descriptor cost.
    mutable std::list<std::pair<std::uint64_t, FD>> read_handles;
    struct CachedBlock { std::uint64_t table_id; std::uint64_t offset; std::string data; };
    mutable std::list<CachedBlock> block_cache;
    mutable std::size_t block_cache_bytes{0};
    mutable std::uint64_t block_cache_hits{0}, block_cache_misses{0};

    explicit Impl(const std::string& directory, Options options) : dir(fs::absolute(directory).lexically_normal()), opt(options) {
        if (options.memtable_max_bytes < 1024 || options.compaction_trigger < 2 ||
            options.block_cache_capacity_bytes > 256U * 1024U * 1024U)
            fail("invalid memtable, compaction, or block cache options");
        createDirectoriesDurably(dir);
        lock_fd = FD(::open((dir / "LOCK").c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0644));
        if (lock_fd.fd < 0) fail(err("open LOCK"));
        if (::flock(lock_fd.fd, LOCK_EX | LOCK_NB) != 0) fail("database already open by another process");
        if (fs::exists(dir / "MANIFEST")) {
            manifest = readManifest(dir);
            // An existing database must retain its WAL name even when empty.
            // Recreating a missing WAL would silently discard acknowledged writes.
            wal_fd = FD(::open((dir / "active.wal").c_str(), O_RDWR | O_CLOEXEC));
            if (wal_fd.fd < 0) fail(err("open existing WAL"));
        } else {
            // Do not silently overwrite an existing store if MANIFEST disappeared.
            bool foreign_data = false;
            for (const auto& entry : fs::directory_iterator(dir)) {
                if (entry.path().filename() != "LOCK") { foreign_data = true; break; }
            }
            if (foreign_data) fail("MANIFEST missing from non-empty directory; refusing new DB");
            // Publish and persist the WAL directory entry before publishing the
            // MANIFEST, so a successfully initialized store always has a WAL.
            wal_fd = FD(::open((dir / "active.wal").c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644));
            if (wal_fd.fd < 0) fail(err("create WAL"));
            syncFile(wal_fd.fd);
            syncDir(dir);
            saveManifest(dir, manifest);
        }
        latest_seq = manifest.durable_seq;
        for (auto id : manifest.files) tables.push_back(loadSST(dir / sstFilename(id), id));
        replayWal();
        // Under the process lock, unreferenced SST and interrupted tmp writes are safe to discard.
        std::unordered_set<std::string> retained;
        for (auto id : manifest.files) retained.insert(sstFilename(id));
        for (const auto& entry : fs::directory_iterator(dir)) {
            const auto name = entry.path().filename().string();
            if (name == "MANIFEST.tmp" ||
                (name.rfind("SST-", 0) == 0 &&
                 (entry.path().extension() == ".tmp" ||
                  (entry.path().extension() == ".sst" && retained.count(name) == 0)))) {
                std::error_code ec;
                fs::remove(entry.path(), ec);
                if (ec) fail("cannot clean orphan file " + name + ": " + ec.message());
            }
        }
        flush_worker = std::thread([this] { backgroundLoop(); });
    }

    ~Impl() {
        { std::lock_guard<std::mutex> lock(mutex); shutdown = true; }
        cv.notify_all();
        if (flush_worker.joinable()) flush_worker.join();
    }

    void freezeLocked() {
        if (mem.empty() || immutable) return;
        immutable = std::make_unique<std::map<std::string, Record>>();
        immutable->swap(mem);
        immutable_seq = latest_seq;
        immutable_bytes = mem_bytes;
        mem_bytes = 0;
        cv.notify_all();
    }

    void waitForWrite(std::unique_lock<std::mutex>& lock) {
        cv.wait(lock, [this] { return poisoned || (!barrier && !(immutable && mem_bytes >= opt.memtable_max_bytes)); });
        if (poisoned) fail("database has an uncertain I/O commit; close and reopen it: " + background_error);
    }

    void backgroundLoop() noexcept {
        std::unique_lock<std::mutex> lock(mutex);
        for (;;) {
            cv.wait(lock, [this] { return shutdown || poisoned || bool(immutable) || compaction_pending; });
            if (poisoned || (!immutable && !compaction_pending && shutdown)) return;
            if (compaction_pending) {
                try {
                    compactWorker(lock);
                    compaction_pending = false;
                    cv.notify_all();
                } catch (const std::exception& e) {
                    if (!lock.owns_lock()) lock.lock();
                    poisoned = true; background_error = e.what(); cv.notify_all(); return;
                } catch (...) {
                    if (!lock.owns_lock()) lock.lock();
                    poisoned = true; background_error = "unknown background compaction failure"; cv.notify_all(); return;
                }
                continue;
            }
            const auto id = manifest.next_id;
            const auto seq = immutable_seq;
            try {
                if (id == std::numeric_limits<std::uint64_t>::max()) fail("SST file id exhausted");
                const auto* snapshot = immutable.get();
                lock.unlock();
                SSTWriter writer(dir / sstFilename(id));
                for (const auto& kv : *snapshot) writer.Append(kv.first, kv.second);
                writer.Finish();
                maybeCrash("after_sst_rename");
                SSTable table = loadSST(dir / sstFilename(id), id);
                lock.lock();
                if (poisoned) { cv.notify_all(); return; }
                Manifest updated = manifest;
                updated.files.push_back(id);
                updated.next_id = id + 1;
                updated.durable_seq = seq;
                saveManifest(dir, updated);
                maybeCrash("after_manifest_rename");
                tables.push_back(std::move(table));
                manifest = std::move(updated);
                immutable.reset();
                immutable_bytes = 0;
                // Never truncate WAL while it may contain newer active writes.
                if (mem.empty()) {
                    truncateFile(wal_fd.fd, 0);
                    if (::lseek(wal_fd.fd, 0, SEEK_END) < 0) fail(err("reset WAL position"));
                    maybeCrash("after_wal_truncate");
                }
                if (tables.size() >= opt.compaction_trigger) compaction_pending = true;
                if (!barrier && !shutdown && mem_bytes >= opt.memtable_max_bytes) freezeLocked();
                cv.notify_all();
            } catch (const std::exception& e) {
                if (!lock.owns_lock()) lock.lock();
                poisoned = true; background_error = e.what(); cv.notify_all(); return;
            } catch (...) {
                if (!lock.owns_lock()) lock.lock();
                poisoned = true; background_error = "unknown background flush failure"; cv.notify_all(); return;
            }
        }
    }

    void replayWal() {
        const auto size = fileSize(wal_fd.fd);
        std::uint64_t off = 0, previous = 0;
        while (off < size) {
            if (size - off < 4) break; // incomplete record magic at the tail
            char magic[4];
            preadAll(wal_fd.fd, magic, sizeof(magic), off);
            const bool batch_v2 = std::memcmp(magic, "SKB2", 4) == 0;
            if (batch_v2 || std::memcmp(magic, "SKB1", 4) == 0) {
                const auto header_size = batch_v2 ? kBatchV2Head : kBatchHead;
                if (size - off < header_size) break;
                char bh[kBatchV2Head];
                preadAll(wal_fd.fd, bh, header_size, off);
                if (batch_v2 && crc32(std::string(bh, kBatchHead)) != get32(bh + kBatchHead))
                    fail("WAL batch header CRC mismatch");
                const auto seq = get64(bh + 4);
                const auto count = get32(bh + 12);
                const auto payload_len = get32(bh + 16);
                if (seq == 0 || seq <= previous) fail("batch WAL sequence out of order");
                if (count == 0 || count > kMaxBatchOperations || payload_len > kMaxBatchBytes ||
                    static_cast<std::uint64_t>(count) * 9 > payload_len)
                    fail("invalid WAL batch header");
                if (size - off - header_size < payload_len) break; // torn batch payload
                std::string payload(payload_len, '\0');
                preadAll(wal_fd.fd, payload.data(), payload.size(), off + header_size);
                std::uint32_t crc = crcUpdate(0xffffffffU, bh + 4, 16);
                crc = ~crcUpdate(crc, payload.data(), payload.size());
                if (crc != get32(bh + 20)) fail("WAL batch CRC mismatch");
                std::vector<std::pair<std::string, Record>> parsed;
                parsed.reserve(count);
                std::size_t pos = 0;
                for (std::uint32_t i = 0; i < count; ++i) {
                    if (payload.size() - pos < 9) fail("WAL batch record truncated");
                    const auto kind = static_cast<std::uint8_t>(payload[pos]);
                    const auto kl = get32(payload.data() + pos + 1);
                    const auto vl = get32(payload.data() + pos + 5);
                    pos += 9;
                    if (kind != kPut && kind != kDelete) fail("invalid WAL batch kind");
                    checkBounds(kl, vl);
                    if (kind == kDelete && vl != 0) fail("invalid WAL batch tombstone");
                    if (static_cast<std::uint64_t>(kl) + vl > payload.size() - pos)
                        fail("WAL batch record length mismatch");
                    std::string key(payload.data() + pos, kl);
                    pos += kl;
                    std::string val(payload.data() + pos, vl);
                    pos += vl;
                    parsed.emplace_back(std::move(key), Record{seq, kind == kDelete, std::move(val)});
                }
                if (pos != payload.size()) fail("WAL batch trailing bytes");
                previous = seq;
                if (seq > manifest.durable_seq) {
                    for (auto& item : parsed) setMem(item.first, std::move(item.second));
                }
                latest_seq = std::max(latest_seq, seq);
                off += header_size + payload_len;
                continue;
            }
            const bool wal_v2 = std::memcmp(magic, "SKW2", 4) == 0;
            if (!wal_v2 && std::memcmp(magic, "SKW1", 4) != 0)
                fail("WAL invalid record magic at offset " + std::to_string(off));
            const auto header_size = wal_v2 ? kWalV2Head : kWalHead;
            if (size - off < header_size) break; // interrupted record header
            char h[kWalV2Head];
            preadAll(wal_fd.fd, h, header_size, off);
            if (wal_v2 && crc32(std::string(h, kWalHead)) != get32(h + kWalHead))
                fail("WAL header CRC mismatch at offset " + std::to_string(off));
            const auto seq = get64(h + 4);
            const auto kind = static_cast<std::uint8_t>(h[12]);
            const auto kl = get32(h + 13), vl = get32(h + 17);
            if (kind != kPut && kind != kDelete) fail("WAL invalid kind at offset " + std::to_string(off));
            checkBounds(kl, vl);
            if (seq == 0 || seq <= previous) fail("WAL sequence out of order");
            if (size - off - header_size < static_cast<std::uint64_t>(kl) + vl) break; // interrupted payload
            std::string key(kl, '\0'), val(vl, '\0');
            preadAll(wal_fd.fd, key.data(), key.size(), off + header_size);
            preadAll(wal_fd.fd, val.data(), val.size(), off + header_size + kl);
            if (crcParts(h + 4, 17, key, val) != get32(h + 21))
                fail("WAL CRC mismatch at offset " + std::to_string(off));
            previous = seq;
            if (seq > manifest.durable_seq) setMem(key, {seq, kind == kDelete, std::move(val)});
            latest_seq = std::max(latest_seq, seq);
            off += header_size + static_cast<std::uint64_t>(kl) + vl;
        }
        if (off != size) truncateFile(wal_fd.fd, off); // only incomplete trailing record truncated
        if (::lseek(wal_fd.fd, 0, SEEK_END) < 0) fail(err("lseek WAL"));
    }
    void setMem(const std::string& key, Record record) {
        const auto it = mem.find(key);
        if (it != mem.end()) mem_bytes -= key.size() + it->second.value.size() + 32;
        mem_bytes += key.size() + record.value.size() + 32;
        mem[key] = std::move(record);
    }
    void appendWal(const std::string& key, const std::string& val, std::uint8_t kind, std::uint64_t seq) {
        std::string h("SKW2", 4);
        put64(h, seq);
        h.push_back(static_cast<char>(kind));
        put32(h, static_cast<std::uint32_t>(key.size()));
        put32(h, static_cast<std::uint32_t>(val.size()));
        put32(h, crcParts(h.data() + 4, 17, key, val));
        put32(h, crc32(h));
        // A single writer is protected by mutex; position is EOF after replay/truncate.
        writeAll(wal_fd.fd, h.data(), h.size());
        writeAll(wal_fd.fd, key.data(), key.size());
        writeAll(wal_fd.fd, val.data(), val.size());
        if (opt.sync_writes) syncFile(wal_fd.fd);
    }
    void mutate(const std::string& key, const std::string& val, std::uint8_t kind) {
        checkBounds(key.size(), val.size());
        std::unique_lock<std::mutex> guard(mutex);
        waitForWrite(guard);
        if (latest_seq == std::numeric_limits<std::uint64_t>::max()) fail("sequence overflow");
        const auto seq = latest_seq + 1;
        try {
            // 先将写入持久化到 WAL；默认 fsync 成功后才更新内存状态。
            appendWal(key, val, kind, seq);
            latest_seq = seq;
            setMem(key, {seq, kind == kDelete, val});
            if (mem_bytes >= opt.memtable_max_bytes) freezeLocked();
        } catch (...) {
            // I/O 失败可能让磁盘落入“写了部分字节”的不确定状态，不可再沿用此实例。
            poisoned = true; cv.notify_all();
            throw;
        }
    }
    void writeBatch(const std::vector<BatchOperation>& operations) {
        if (operations.empty()) return;
        if (operations.size() > kMaxBatchOperations) fail("batch exceeds 4096 operations");
        std::string payload;
        for (const auto& op : operations) {
            const bool is_put = op.type == BatchOperation::Type::Put;
            if (!is_put && op.type != BatchOperation::Type::Delete) fail("unknown batch operation");
            checkBounds(op.key.size(), op.value.size());
            if (!is_put && !op.value.empty()) fail("batch Delete value must be empty");
            const auto addition = 9U + op.key.size() + op.value.size();
            if (addition > kMaxBatchBytes - payload.size()) fail("batch exceeds 32 MiB");
            payload.push_back(static_cast<char>(is_put ? kPut : kDelete));
            put32(payload, static_cast<std::uint32_t>(op.key.size()));
            put32(payload, static_cast<std::uint32_t>(op.value.size()));
            payload += op.key;
            payload += op.value;
        }
        std::unique_lock<std::mutex> guard(mutex);
        waitForWrite(guard);
        if (latest_seq == std::numeric_limits<std::uint64_t>::max()) fail("sequence overflow");
        const auto seq = latest_seq + 1;
        std::string header("SKB2", 4);
        put64(header, seq);
        put32(header, static_cast<std::uint32_t>(operations.size()));
        put32(header, static_cast<std::uint32_t>(payload.size()));
        std::uint32_t crc = crcUpdate(0xffffffffU, header.data() + 4, 16);
        crc = ~crcUpdate(crc, payload.data(), payload.size());
        put32(header, crc);
        put32(header, crc32(header));
        try {
            writeAll(wal_fd.fd, header.data(), header.size());
            writeAll(wal_fd.fd, payload.data(), payload.size());
            if (opt.sync_writes) syncFile(wal_fd.fd);
            maybeCrash("after_batch_wal_sync");
            latest_seq = seq;
            for (const auto& op : operations) {
                setMem(op.key, {seq, op.type == BatchOperation::Type::Delete, op.value});
            }
            if (mem_bytes >= opt.memtable_max_bytes) freezeLocked();
        } catch (...) {
            poisoned = true; cv.notify_all();
            throw;
        }
    }
    std::optional<std::string> getLocked(const std::string& key) const {
        if (poisoned) fail("database has an uncertain I/O commit; close and reopen it");
        if (const auto it = mem.find(key); it != mem.end()) {
            if (it->second.deleted) return std::nullopt;
            return it->second.value;
        }
        if (immutable) {
            if (const auto it = immutable->find(key); it != immutable->end()) {
                if (it->second.deleted) return std::nullopt;
                return it->second.value;
            }
        }
        // 新的 SST 文件优先；同一键的较早版本被新值或墓碑覆盖。
        for (auto it = tables.rbegin(); it != tables.rend(); ++it) {
            if (!it->bloom.MayContain(key)) continue;
            const auto& idx = it->index;
            auto pos = std::lower_bound(idx.begin(), idx.end(), key,
                [](const IndexEntry& e, const std::string& k) { return e.key < k; });
            if (pos == idx.end() || pos->key != key) continue;
            if (pos->deleted) return std::nullopt;
            return cachedRead(*it, *pos).value;
        }
        return std::nullopt;
    }
    int cachedReader(const SSTable& table) const {
        for (auto it = read_handles.begin(); it != read_handles.end(); ++it) {
            if (it->first == table.id) {
                read_handles.splice(read_handles.begin(), read_handles, it);
                return read_handles.front().second.fd;
            }
        }
        FD fd(::open(table.path.c_str(), O_RDONLY | O_CLOEXEC));
        if (fd.fd < 0) fail(err("open cached SST"));
        read_handles.emplace_front(table.id, std::move(fd));
        if (read_handles.size() > 32) read_handles.pop_back();
        return read_handles.front().second.fd;
    }
    Record cachedRead(const SSTable& table, const IndexEntry& item) const {
        if (item.block_bytes == 0 || opt.block_cache_capacity_bytes == 0)
            return readAt(table, item, cachedReader(table));
        auto found = block_cache.end();
        for (auto it = block_cache.begin(); it != block_cache.end(); ++it) {
            if (it->table_id == table.id && it->offset == item.block_offset) { found = it; break; }
        }
        std::string uncached;
        const std::string* data = nullptr;
        if (found != block_cache.end()) {
            ++block_cache_hits;
            block_cache.splice(block_cache.begin(), block_cache, found);
            data = &block_cache.front().data;
        } else {
            ++block_cache_misses;
            uncached.resize(item.block_bytes);
            preadAll(cachedReader(table), uncached.data(), uncached.size(), item.block_offset);
            if (crc32(uncached) != item.block_crc) fail("SST cached block CRC mismatch");
            if (uncached.size() <= opt.block_cache_capacity_bytes) {
                while (!block_cache.empty() && block_cache_bytes > opt.block_cache_capacity_bytes - uncached.size()) {
                    block_cache_bytes -= block_cache.back().data.size(); block_cache.pop_back();
                }
                block_cache.emplace_front(CachedBlock{table.id, item.block_offset, std::move(uncached)});
                block_cache_bytes += block_cache.front().data.size();
                data = &block_cache.front().data;
            } else data = &uncached;
        }
        if (item.offset < item.block_offset) fail("invalid SST block record offset");
        const auto rel = item.offset - item.block_offset;
        if (rel > data->size() || data->size() - rel < kSstHead) fail("truncated cached SST header");
        const char* h = data->data() + static_cast<std::size_t>(rel);
        const auto seq = get64(h); const auto kind = static_cast<std::uint8_t>(h[8]);
        const auto kl = get32(h + 9), vl = get32(h + 13);
        if (kl != item.key.size() || seq != item.seq || (kind == kDelete) != item.deleted ||
            (kind != kPut && kind != kDelete) || vl > kMaxValue ||
            static_cast<std::uint64_t>(kl) + vl > data->size() - rel - kSstHead)
            fail("cached SST index/data mismatch");
        std::string key(h + kSstHead, kl), value(h + kSstHead + kl, vl);
        if (key != item.key || get32(h + 17) != crcParts(h, 17, key, value)) fail("cached SST record CRC mismatch");
        return {seq, kind == kDelete, std::move(value)};
    }
    // Only the worker changes tables and manifest. While it builds the merged
    // SST without the mutex, readers can safely use the unchanged old tables.
    void compactWorker(std::unique_lock<std::mutex>& lock) {
        if (tables.size() < 2) return;
        const auto id = manifest.next_id;
        if (id == std::numeric_limits<std::uint64_t>::max()) fail("SST file id exhausted");
        const auto started = std::chrono::steady_clock::now();
        lock.unlock();
        SSTWriter writer(dir / sstFilename(id));
        // Reuse one read descriptor per source SST during this compaction.
        // Opening a file for every key creates O(records) extra open/close syscalls.
        std::vector<FD> readers;
        readers.reserve(tables.size());
        for (const auto& table : tables) {
            FD fd(::open(table.path.c_str(), O_RDONLY | O_CLOEXEC));
            if (fd.fd < 0) fail(err("open compaction source SST"));
            readers.push_back(std::move(fd));
        }
        std::priority_queue<SourceEntry, std::vector<SourceEntry>, KeyGreater> pq;
        auto push = [&](std::size_t src, std::size_t pos) {
            const auto& table = tables[src];
            if (pos < table.index.size()) {
                const auto& item = table.index[pos];
                pq.push({item.key, item.seq, item.deleted, src + 1, pos});
            }
        };
        for (std::size_t i = 0; i < tables.size(); ++i) push(i, 0);
        while (!pq.empty()) {
            const auto key = pq.top().key;
            SourceEntry best = pq.top();
            while (!pq.empty() && pq.top().key == key) {
                const auto curr = pq.top(); pq.pop();
                if (curr.seq > best.seq) best = curr;
                push(curr.source - 1, curr.index + 1);
            }
            // Since all SSTables are merged, no older disk version can be exposed.
            if (!best.deleted) {
                const auto& tab = tables[best.source - 1];
                writer.Append(key, readAt(tab, tab.index[best.index], readers[best.source - 1].fd));
            }
        }
        writer.Finish();
        maybeCrash("after_compaction_sst_rename");
        SSTable compacted = loadSST(dir / sstFilename(id), id);
        std::error_code size_error;
        const auto output_bytes = fs::file_size(dir / sstFilename(id), size_error);
        if (size_error) fail("cannot stat compacted SST: " + size_error.message());
        lock.lock();
        if (poisoned) return;
        Manifest updated = manifest;
        updated.files = {id};
        updated.next_id = id + 1;
        // durable_seq unchanged: all WAL-covered versions are already in SSTables.
        // 新 SST + MANIFEST 成功后才允许删除旧 SST；保留崩溃恢复能力。
        saveManifest(dir, updated);
        maybeCrash("after_compaction_manifest");
        const auto old = manifest.files;
        manifest = std::move(updated);
        tables.clear();
        tables.push_back(std::move(compacted));
        read_handles.remove_if([&](const auto& handle) {
            return std::find(old.begin(), old.end(), handle.first) != old.end();
        });
        for (auto it = block_cache.begin(); it != block_cache.end();) {
            if (std::find(old.begin(), old.end(), it->table_id) != old.end()) {
                block_cache_bytes -= it->data.size(); it = block_cache.erase(it);
            } else ++it;
        }
        for (auto old_id : old) {
            std::error_code ec;
            fs::remove(dir / sstFilename(old_id), ec);
            if (ec) fail("failed deleting obsolete SST: " + ec.message());
        }
        syncDir(dir);
        ++compaction_count;
        compaction_output_bytes += output_bytes;
        compaction_micros += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started).count());
    }
    void flush() {
        std::unique_lock<std::mutex> lock(mutex);
        flushBarrier(lock, false);
    }
    void compact() {
        std::unique_lock<std::mutex> lock(mutex);
        flushBarrier(lock, true);
    }
    void flushBarrier(std::unique_lock<std::mutex>& lock, bool compact_after) {
        cv.wait(lock, [this] { return poisoned || !barrier; });
        if (poisoned) fail("database has an uncertain I/O commit; close and reopen it: " + background_error);
        barrier = true;
        try {
            cv.wait(lock, [this] { return poisoned || !immutable; });
            if (poisoned) fail("background flush failed: " + background_error);
            freezeLocked();
            cv.wait(lock, [this] { return poisoned || !immutable; });
            if (poisoned) fail("background flush failed: " + background_error);
            if (compact_after) {
                compaction_pending = true;
                barrier = false; cv.notify_all();
                cv.wait(lock, [this] { return poisoned || !compaction_pending; });
                if (poisoned) fail("background compaction failed: " + background_error);
            } else { barrier = false; cv.notify_all(); }
        } catch (...) { poisoned = true; barrier = false; cv.notify_all(); throw; }
    }
    Stats stats() const {
        std::unique_lock<std::mutex> lock(mutex);
        // Preserve the API's historical threshold observation: when an
        // automatic full compaction is due, report after it has settled.
        cv.wait(lock, [this] { return poisoned || !compaction_pending || tables.size() < opt.compaction_trigger; });
        return {latest_seq, manifest.durable_seq, mem.size() + (immutable ? immutable->size() : 0),
                mem_bytes + immutable_bytes, tables.size(), fileSize(wal_fd.fd), compaction_count,
                compaction_micros, compaction_output_bytes, static_cast<std::size_t>(bool(immutable)) + static_cast<std::size_t>(compaction_pending),
                block_cache_bytes, block_cache_hits, block_cache_misses};
    }
    std::vector<std::pair<std::string, std::string>> scan(
        const std::string& begin, const std::string& end, std::size_t limit) const {
        if (limit > 100000) fail("scan limit maximum is 100000");
        if (limit == 0 || (!end.empty() && begin >= end)) return {};
        std::lock_guard<std::mutex> lock(mutex);
        if (poisoned) fail("database has an uncertain I/O commit; close and reopen it");
        std::vector<std::pair<std::string, Record>> mem_records;
        std::map<std::string, Record> merged;
        if (immutable) merged = *immutable;
        for (const auto& kv : mem) merged.insert_or_assign(kv.first, kv.second);
        for (auto it = merged.lower_bound(begin); it != merged.end(); ++it) {
            if (!end.empty() && it->first >= end) break;
            mem_records.push_back(*it);
        }
        std::priority_queue<SourceEntry, std::vector<SourceEntry>, KeyGreater> pq;
        auto push = [&](std::size_t src, std::size_t pos) {
            if (src == 0) {
                if (pos >= mem_records.size()) return;
                const auto& e = mem_records[pos];
                pq.push({e.first, e.second.seq, e.second.deleted, src, pos});
            } else {
                const auto& idx = tables[src - 1].index;
                if (pos >= idx.size()) return;
                const auto& item = idx[pos];
                if (!end.empty() && item.key >= end) return;
                pq.push({item.key, item.seq, item.deleted, src, pos});
            }
        };
        push(0, 0);
        for (std::size_t src = 1; src <= tables.size(); ++src) {
            const auto& idx = tables[src - 1].index;
            const auto it = std::lower_bound(idx.begin(), idx.end(), begin,
                [](const IndexEntry& e, const std::string& k) { return e.key < k; });
            push(src, static_cast<std::size_t>(it - idx.begin()));
        }
        std::vector<std::pair<std::string, std::string>> result;
        while (!pq.empty() && result.size() < limit) {
            const auto key = pq.top().key;
            SourceEntry best = pq.top();
            while (!pq.empty() && pq.top().key == key) {
                auto curr = pq.top(); pq.pop();
                if (curr.seq > best.seq) best = curr;
                push(curr.source, curr.index + 1);
            }
            if (!best.deleted) {
                if (best.source == 0) result.emplace_back(key, mem_records[best.index].second.value);
                else {
                    const auto& table = tables[best.source - 1];
                    result.emplace_back(key, cachedRead(table, table.index[best.index]).value);
                }
            }
        }
        return result;
    }
};

DB::DB(std::string directory, Options options) : p_(std::make_unique<Impl>(directory, options)) {}
DB::~DB() = default;
void DB::Put(const std::string& key, const std::string& value) { p_->mutate(key, value, kPut); }
void DB::Delete(const std::string& key) { p_->mutate(key, "", kDelete); }
void DB::WriteBatch(const std::vector<BatchOperation>& operations) { p_->writeBatch(operations); }
std::optional<std::string> DB::Get(const std::string& key) const {
    checkBounds(key.size(), 0);
    std::lock_guard<std::mutex> lock(p_->mutex);
    return p_->getLocked(key);
}
std::vector<std::pair<std::string, std::string>> DB::Scan(
    const std::string& begin, const std::string& end, std::size_t limit) const {
    return p_->scan(begin, end, limit);
}
void DB::Flush() { p_->flush(); }
void DB::Compact() { p_->compact(); }
Stats DB::GetStats() const { return p_->stats(); }
} // namespace stonekv
