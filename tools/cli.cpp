#include "stonekv/db.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void usage() {
    std::cerr << "StoneKV CLI (Linux)\n"
              << "Usage:\n"
              << "  stonekv_cli <database_dir> put <key> <value>\n"
              << "  stonekv_cli <database_dir> get <key>\n"
              << "  stonekv_cli <database_dir> del <key>\n"
              << "  stonekv_cli <database_dir> scan [begin] [end] [limit]\n"
              << "  stonekv_cli <database_dir> stats | stats-json | flush | compact\n"
              << "Note: CLI displays raw values as text; C++ API supports arbitrary bytes.\n";
}
}
int main(int argc, char** argv) {
    try {
        if (argc < 3) { usage(); return 2; }
        stonekv::DB db(argv[1]);
        const std::string cmd(argv[2]);
        if (cmd == "put" && argc == 5) {
            db.Put(argv[3], argv[4]); std::cout << "OK\n";
        } else if (cmd == "get" && argc == 4) {
            auto value = db.Get(argv[3]);
            if (!value) { std::cout << "NOT_FOUND\n"; return 4; }
            std::cout << *value << '\n';
        } else if (cmd == "del" && argc == 4) {
            db.Delete(argv[3]); std::cout << "OK\n";
        } else if (cmd == "scan" && argc <= 6) {
            const auto begin = argc > 3 ? argv[3] : "";
            const auto end = argc > 4 ? argv[4] : "";
            const auto limit = argc > 5 ? std::stoull(argv[5]) : 100ULL;
            for (const auto& item : db.Scan(begin, end, static_cast<std::size_t>(limit)))
                std::cout << item.first << '\t' << item.second << '\n';
        } else if (cmd == "stats" && argc == 3) {
            const auto s = db.GetStats();
            std::cout << "latest_seq=" << s.latest_sequence << '\n'
                      << "durable_seq=" << s.durable_sequence << '\n'
                      << "memtable_entries=" << s.memtable_entries << '\n'
                      << "memtable_bytes=" << s.memtable_bytes << '\n'
                      << "sstable_count=" << s.sstable_count << '\n'
                      << "wal_bytes=" << s.wal_bytes << '\n';
        } else if (cmd == "stats-json" && argc == 3) {
            const auto s = db.GetStats();
            // Stable numeric JSON fields, without keys, values or database path.
            std::cout << "{\"event\":\"stats\",\"latest_sequence\":" << s.latest_sequence
                      << ",\"durable_sequence\":" << s.durable_sequence
                      << ",\"memtable_entries\":" << s.memtable_entries
                      << ",\"memtable_bytes\":" << s.memtable_bytes
                      << ",\"sstable_count\":" << s.sstable_count
                      << ",\"wal_bytes\":" << s.wal_bytes
                      << ",\"compaction_count\":" << s.compaction_count
                      << ",\"compaction_micros\":" << s.compaction_micros
                      << ",\"compaction_output_bytes\":" << s.compaction_output_bytes
                      << ",\"background_tasks\":" << s.background_tasks
                      << ",\"background_tasks_peak\":" << s.background_tasks_peak
                      << ",\"block_cache_bytes\":" << s.block_cache_bytes
                      << ",\"block_cache_hits\":" << s.block_cache_hits
                      << ",\"block_cache_misses\":" << s.block_cache_misses
                      << ",\"sst_full_index_entries\":" << s.sst_full_index_entries
                      << ",\"sst_sparse_blocks\":" << s.sst_sparse_blocks << "}\n";
        } else if (cmd == "flush" && argc == 3) {
            db.Flush(); std::cout << "OK\n";
        } else if (cmd == "compact" && argc == 3) {
            db.Compact(); std::cout << "OK\n";
        } else { usage(); return 2; }
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
