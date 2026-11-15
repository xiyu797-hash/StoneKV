#include "stonekv/db.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
double elapsedMs(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}
void printMetric(const std::string& op, std::uint64_t count, double ms) {
    std::cout << op << ',' << count << ',' << std::fixed << std::setprecision(3) << ms << ',';
    if (op == "put" || op == "random_get")
        std::cout << std::setprecision(2) << count * 1000.0 / std::max(0.001, ms);
    else
        std::cout << "NA"; // flush/compaction are maintenance operations, not throughput
    std::cout << '\n';
}
}
int main(int argc, char** argv) {
    try {
        if (argc < 3 || argc > 6) {
            std::cerr << "Usage: stonekv_bench <empty_database_dir> <count> [value_bytes=128] [sync_writes=0|1] [seed=42]\n"
                      << "Writes are serialized; sync_writes=0 is NOT a durable-ack benchmark.\n";
            return 2;
        }
        const std::string path = argv[1];
        const auto n = std::stoull(argv[2]);
        const auto value_size = argc > 3 ? std::stoull(argv[3]) : 128ULL;
        const bool sync = argc > 4 ? std::stoi(argv[4]) != 0 : false;
        const auto seed = argc > 5 ? static_cast<unsigned>(std::stoul(argv[5])) : 42U;
        if (n < 1 || n > 2000000 || value_size > 8192) throw std::runtime_error("count 1..2M, value_bytes 0..8192");
        if (std::filesystem::exists(path) && !std::filesystem::is_empty(path))
            throw std::runtime_error("benchmark requires an empty directory; never deletes existing data");
        stonekv::Options opt;
        opt.sync_writes = sync;
        opt.memtable_max_bytes = 128 * 1024; // Force several immutable SSTs before measured compaction
        opt.compaction_trigger = 1000; // Avoid automatic compaction during write phase
        stonekv::DB db(path, opt);
        std::mt19937_64 rng(seed);
        std::string value(static_cast<std::size_t>(value_size), 'x');
        std::vector<std::string> keys;
        keys.reserve(static_cast<std::size_t>(n));
        for (std::uint64_t i = 0; i < n; ++i) keys.push_back("key-" + std::to_string(i));
        std::cout << "# StoneKV v1 Linux benchmark; release build recommended\n"
                  << "# count=" << n << " value_bytes=" << value_size
                  << " sync_writes=" << (sync ? 1 : 0) << " seed=" << seed << '\n'
                  << "operation,count,elapsed_ms,ops_per_sec\n";
        auto start = Clock::now();
        for (const auto& key : keys) db.Put(key, value);
        auto end = Clock::now();
        printMetric("put", n, elapsedMs(start, end));
        start = Clock::now(); db.Flush(); end = Clock::now();
        printMetric("flush", 1, elapsedMs(start, end));
        std::shuffle(keys.begin(), keys.end(), rng);
        start = Clock::now();
        std::uint64_t hits = 0;
        for (const auto& key : keys) if (db.Get(key)) ++hits;
        end = Clock::now();
        if (hits != n) throw std::runtime_error("readback validation failed");
        printMetric("random_get", n, elapsedMs(start, end));
        if (db.GetStats().sstable_count >= 2) {
            start = Clock::now(); db.Compact(); end = Clock::now();
            printMetric("full_compact", 1, elapsedMs(start, end));
        } else {
            std::cout << "# full_compact skipped (fewer than 2 SSTables)\n";
        }
        const auto s = db.GetStats();
        std::cout << "# sstables=" << s.sstable_count << " memtable_entries=" << s.memtable_entries << '\n';
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
