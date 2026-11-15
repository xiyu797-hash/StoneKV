#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace stonekv {

// V1 is a Linux-only, single-process LSM-inspired KV store.
// Multiple threads may call DB concurrently; public state transitions are
// serialized, while the background worker builds an immutable SST outside the lock.
// The caller must keep DB alive until its threads finish.
struct Options {
    std::size_t memtable_max_bytes = 4U * 1024U * 1024U;
    std::size_t compaction_trigger = 4;
    bool sync_writes = true;  // default: fsync each WAL mutation before returning
    // Maximum cached block payload bytes; zero disables the block cache.
    std::size_t block_cache_capacity_bytes = 8U * 1024U * 1024U;
};

// A WriteBatch is atomically replayed from one WAL record.
// Batch ordering is preserved; the last operation on a repeated key wins.
struct BatchOperation {
    enum class Type { Put, Delete };
    Type type;
    std::string key;
    std::string value; // Must be empty for Delete.
};

struct Stats {
    std::uint64_t latest_sequence = 0;
    std::uint64_t durable_sequence = 0;
    std::size_t memtable_entries = 0;
    std::size_t memtable_bytes = 0;
    std::size_t sstable_count = 0;
    std::uint64_t wal_bytes = 0;
    // Per-process successful full compactions since this DB instance opened.
    std::uint64_t compaction_count = 0;
    std::uint64_t compaction_micros = 0;
    std::uint64_t compaction_output_bytes = 0;
    std::size_t background_tasks = 0;
    std::size_t block_cache_bytes = 0;
    std::uint64_t block_cache_hits = 0;
    std::uint64_t block_cache_misses = 0;
};

class DB final {
public:
    explicit DB(std::string directory, Options options = {});
    ~DB();
    DB(const DB&) = delete;
    DB& operator=(const DB&) = delete;
    DB(DB&&) = delete;
    DB& operator=(DB&&) = delete;

    void Put(const std::string& key, const std::string& value);
    void Delete(const std::string& key); // tombstones are durable, even if key does not exist
    // One durable WAL record for the entire batch; empty batch is a no-op.
    // Max 4096 operations and 32 MiB encoded payload per batch.
    void WriteBatch(const std::vector<BatchOperation>& operations);
    std::optional<std::string> Get(const std::string& key) const;
    // Half-open [begin, end). If end=="", scan to the final key.
    // Empty begin scans from the first key. Results are ascending by byte order.
    std::vector<std::pair<std::string, std::string>> Scan(
        const std::string& begin = "", const std::string& end = "",
        std::size_t limit = 100) const;

    // Flush waits for all preceding writes to reach a committed SST and is a
    // durability boundary for sync_writes=false mode. Background I/O failures
    // poison the instance; close and reopen after investigating the error.
    void Flush();
    // Wait for a full compaction submitted to the background worker. Other
    // clients may continue reads and writes while the merged SST is built.
    void Compact();
    Stats GetStats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace stonekv
