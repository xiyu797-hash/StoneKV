#include "stonekv/db.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;
namespace {
struct Tmp {
    fs::path path;
    Tmp() {
        std::string pattern = (fs::temp_directory_path() / "stonekv-test-XXXXXX").string();
        std::vector<char> chars(pattern.begin(), pattern.end()); chars.push_back('\0');
        char* v = ::mkdtemp(chars.data());
        if (!v) throw std::runtime_error("mkdtemp failed");
        path = v;
    }
    ~Tmp() { std::error_code ignored; fs::remove_all(path, ignored); }
};
void require(bool result, const std::string& what) {
    if (!result) throw std::runtime_error("assertion: " + what);
}
template <class F> void expectThrow(F func, const std::string& why) {
    try { func(); } catch (const std::exception&) { return; }
    throw std::runtime_error("expected exception: " + why);
}
stonekv::Options opt(std::size_t threshold = 1024, std::size_t compaction = 4, bool sync = true) {
    stonekv::Options o; o.memtable_max_bytes = threshold; o.compaction_trigger = compaction; o.sync_writes = sync;
    return o;
}
void basic() {
    Tmp t;
    {
        stonekv::DB db(t.path.string());
        require(!db.Get("a"), "missing key");
        db.Put("a", "hello");
        require(db.Get("a") == "hello", "get");
        db.Put("a", "world");
        require(db.Get("a") == "world", "overwrite");
        db.Put("empty", "");
        require(db.Get("empty") == "", "empty value");
        db.Delete("a");
        require(!db.Get("a"), "delete");
        require(db.GetStats().latest_sequence == 4, "sequence");
    }
    {
        stonekv::DB db(t.path.string());
        require(!db.Get("a"), "delete after reopen");
        require(db.Get("empty") == "", "empty after reopen");
    }
}
void binaryValues() {
    Tmp t;
    std::string key("\x00\x01\xff", 3), value("z\x00\xff\n", 4);
    stonekv::DB db(t.path.string());
    db.Put(key, value);
    require(db.Get(key) == value, "binary roundtrip");
    db.Flush();
    require(db.Get(key) == value, "binary SST");
}
void scan() {
    Tmp t;
    stonekv::DB db(t.path.string(), opt());
    db.Put("d", "4"); db.Put("c", "3"); db.Put("b", "2"); db.Put("a", "1");
    db.Flush();
    db.Put("b", "22"); db.Delete("c"); db.Put("e", "5");
    auto all = db.Scan();
    require(all == (std::vector<std::pair<std::string,std::string>>{{"a","1"},{"b","22"},{"d","4"},{"e","5"}}), "merged scan");
    auto sliced = db.Scan("b", "e", 2);
    require(sliced.size() == 2 && sliced[0].first == "b" && sliced[1].first == "d", "scan bounds limit");
    require(db.Scan("z", "a").empty(), "inverted scan bounds");
    require(db.Scan("", "", 0).empty(), "zero limit");
}
void flushReopen() {
    Tmp t;
    {
        stonekv::DB db(t.path.string(), opt());
        for (int i = 0; i < 100; ++i) db.Put("k" + std::to_string(i), std::string(100, 'a'));
        db.Flush();
        require(db.GetStats().wal_bytes == 0, "WAL truncated");
        require(db.GetStats().sstable_count >= 1, "SST created");
    }
    stonekv::DB db(t.path.string(), opt());
    for (int i = 0; i < 100; ++i)
        require(db.Get("k" + std::to_string(i)) == std::string(100, 'a'), "flush recovery");
}
void compaction() {
    Tmp t;
    {
        stonekv::DB db(t.path.string(), opt(1024, 100));
        for (int i = 0; i < 6; ++i) { db.Put("a", std::to_string(i)); db.Flush(); }
        db.Delete("a"); db.Flush();
        db.Put("b", "present"); db.Flush();
        require(db.GetStats().sstable_count >= 8, "multiple tables");
        db.Compact();
        require(db.GetStats().sstable_count == 1, "full compaction");
        require(!db.Get("a") && db.Get("b") == "present", "tombstone compaction");
    }
    stonekv::DB db(t.path.string());
    require(!db.Get("a") && db.Get("b") == "present", "compaction reopen");
}
void automaticCompaction() {
    Tmp t;
    stonekv::DB db(t.path.string(), opt(1024, 3));
    for (int i = 0; i < 100; ++i) db.Put("k" + std::to_string(i), std::string(100, 'x'));
    require(db.GetStats().sstable_count <= 2, "automatic compaction threshold");
}
void validationAndLock() {
    Tmp t;
    stonekv::DB db(t.path.string());
    expectThrow([&]{ db.Put("", "x"); }, "empty key");
    expectThrow([&]{ db.Put("k", std::string(8 * 1024 * 1024 + 1, 'x')); }, "large value");
    expectThrow([&]{ (void)db.Scan("", "", 100001); }, "large scan");
    expectThrow([&]{ stonekv::DB second(t.path.string()); }, "exclusive process lock");
}
void tornWalTail() {
    Tmp t;
    {
        stonekv::DB db(t.path.string()); db.Put("safe", "yes");
    }
    const auto wal = t.path / "active.wal";
    const auto before = fs::file_size(wal);
    { std::ofstream f(wal, std::ios::app | std::ios::binary); f.write("SKW1x", 5); }
    stonekv::DB db(t.path.string());
    require(db.Get("safe") == "yes", "recover torn tail");
    require(db.GetStats().wal_bytes == before, "truncate torn tail");
}
void corruptWal() {
    Tmp t;
    { stonekv::DB db(t.path.string()); db.Put("a", "b"); }
    const auto path = t.path / "active.wal";
    int fd = ::open(path.c_str(), O_WRONLY);
    require(fd >= 0, "open WAL for corruption");
    const char x = 'x'; require(::pwrite(fd, &x, 1, 25) == 1, "inject payload corruption");
    ::close(fd);
    expectThrow([&]{ stonekv::DB bad(t.path.string()); }, "CRC-corrupt WAL must not be silently recovered");
}
void corruptSst() {
    Tmp t;
    {
        stonekv::DB db(t.path.string()); db.Put("a", "b"); db.Flush();
    }
    for (auto& entry : fs::directory_iterator(t.path)) {
        if (entry.path().extension() != ".sst") continue;
        int fd = ::open(entry.path().c_str(), O_WRONLY);
        require(fd >= 0, "open SST");
        const char x = 'x'; require(::pwrite(fd, &x, 1, 16 + 21 + 1) == 1, "inject SST corruption");
        ::close(fd);
    }
    expectThrow([&]{ stonekv::DB bad(t.path.string()); }, "CRC-corrupt SST refused");
}
void corruptManifest() {
    Tmp t;
    { stonekv::DB db(t.path.string()); db.Put("key", "value"); db.Flush(); }
    const auto path = t.path / "MANIFEST";
    int fd = ::open(path.c_str(), O_RDWR);
    require(fd >= 0, "open MANIFEST");
    const char damaged = 'x';
    require(::pwrite(fd, &damaged, 1, 0) == 1, "inject MANIFEST corruption");
    ::close(fd);
    expectThrow([&] { stonekv::DB db(t.path.string()); }, "invalid MANIFEST CRC");
}
void maxKeyAndLimit() {
    Tmp t;
    stonekv::DB db(t.path.string());
    const std::string k(4096, 'x');
    db.Put(k, "edge");
    require(db.Get(k) == "edge", "maximum key accepted");
    expectThrow([&] { db.Put(std::string(4097, 'x'), "v"); }, "oversized key rejected");
    db.Flush();
    require(db.Get(k) == "edge", "maximum key in SST");
}
void reopenedSequenceAndOverwrite() {
    Tmp t;
    { stonekv::DB db(t.path.string()); db.Put("a", "v1"); db.Flush(); }
    { stonekv::DB db(t.path.string()); db.Put("a", "v2"); db.Put("b", "v3"); }
    { stonekv::DB db(t.path.string());
      require(db.Get("a") == "v2" && db.Get("b") == "v3", "sequence survives multiple reopens");
      db.Compact();
      require(db.Get("a") == "v2", "overwrite survives compaction");
    }
}
void crossProcessLock() {
    Tmp t;
    stonekv::DB db(t.path.string());
    const auto pid = ::fork();
    require(pid >= 0, "fork lock tester");
    if (pid == 0) {
        try { stonekv::DB other(t.path.string()); ::_exit(7); }
        catch (...) { ::_exit(0); }
    }
    int status = 0;
    require(::waitpid(pid, &status, 0) == pid, "wait lock child");
    require(WIFEXITED(status) && WEXITSTATUS(status) == 0, "other process is denied lock");
}
void missingManifest() {
    Tmp t;
    { stonekv::DB db(t.path.string()); db.Put("k", "v"); db.Flush(); }
    fs::remove(t.path / "MANIFEST");
    expectThrow([&]{ stonekv::DB bad(t.path.string()); }, "do not initialize over existing SST/WAL");
}
void concurrency() {
    Tmp t;
    stonekv::DB db(t.path.string(), opt(8192, 4, false));
    constexpr int threads = 4, each = 150;
    std::vector<std::thread> pool;
    for (int id = 0; id < threads; ++id) {
        pool.emplace_back([&, id] {
            for (int i = 0; i < each; ++i) {
                const auto key = std::to_string(id) + "-" + std::to_string(i);
                db.Put(key, "v" + key);
                require(db.Get(key) == "v" + key, "thread get");
            }
        });
    }
    for (auto& th : pool) th.join();
    db.Flush();
    for (int id = 0; id < threads; ++id)
        for (int i = 0; i < each; ++i) {
            const auto key = std::to_string(id) + "-" + std::to_string(i);
            require(db.Get(key) == "v" + key, "thread persisted");
        }
}
void modelCheck() {
    Tmp t;
    std::map<std::string, std::string> reference;
    std::mt19937 rng(20261115);
    {
        stonekv::DB db(t.path.string(), opt(2048, 4, false));
        for (int i = 0; i < 1200; ++i) {
            const auto key = "key-" + std::to_string(rng() % 120);
            const auto action = rng() % 5;
            if (action == 0) { db.Delete(key); reference.erase(key); }
            else { auto value = "val-" + std::to_string(rng()); db.Put(key, value); reference[key] = value; }
            if (i % 47 == 0) db.Flush();
            if (i % 101 == 0) {
                require(db.Scan("", "", 1000) == (std::vector<std::pair<std::string, std::string>>(reference.begin(), reference.end())), "randomized scan model");
            }
        }
        db.Compact();
    }
    stonekv::DB db(t.path.string());
    require(db.Scan("", "", 1000) == (std::vector<std::pair<std::string, std::string>>(reference.begin(), reference.end())), "randomized model after reopen");
}
void crashPoint(const std::string& point, bool write_compress = false) {
    Tmp t;
    {
        stonekv::DB db(t.path.string(), opt(1024, 100));
        db.Put("existing", "ok"); db.Flush();
        if (write_compress) {
            db.Put("t", "old"); db.Flush();
            db.Put("t", "new"); db.Flush();
        }
    }
    const auto pid = ::fork();
    if (pid < 0) throw std::runtime_error("fork failed");
    if (pid == 0) {
        ::setenv("STONEKV_FAILPOINT", point.c_str(), 1);
        stonekv::DB db(t.path.string(), opt(1024, 100));
        if (write_compress) db.Compact();
        else { db.Put("pending", "committed-to-WAL"); db.Flush(); }
        ::_exit(0);
    }
    int status = 0;
    require(::waitpid(pid, &status, 0) == pid, "waitpid");
    require(WIFEXITED(status) && WEXITSTATUS(status) == 86, "expected injected crash");
    stonekv::DB db(t.path.string(), opt(1024, 100));
    require(db.Get("existing") == "ok", "existing key survives crash");
    if (write_compress) require(db.Get("t") == "new", "compaction crash preserves newest");
    else require(db.Get("pending") == "committed-to-WAL", "crash point recovers WAL ack");
}
void largeValue() {
    Tmp t;
    std::string v(400000, '\x17'); v[0] = '\0'; v.back() = '\xff';
    { stonekv::DB db(t.path.string(), opt(1024 * 1024, 3)); db.Put("big", v); db.Flush(); }
    { stonekv::DB db(t.path.string(), opt(1024 * 1024, 3)); require(db.Get("big") == v, "large value survives SST"); }
}
} // namespace

int main() {
    using Test = std::pair<const char*, std::function<void()>>;
    const std::vector<Test> tests{
        {"basic_and_reopen", basic}, {"binary_values", binaryValues}, {"ordered_scan", scan},
        {"flush_and_reopen", flushReopen}, {"compaction_and_tombstones", compaction},
        {"auto_compaction", automaticCompaction}, {"input_and_exclusive_lock", validationAndLock},
        {"torn_wal_tail", tornWalTail}, {"corrupt_wal_fails_closed", corruptWal},
        {"corrupt_sst_fails_closed", corruptSst}, {"missing_manifest_fails_closed", missingManifest},
        {"corrupt_manifest_fails_closed", corruptManifest},
        {"max_key_edge", maxKeyAndLimit},
        {"reopen_sequence_overwrite", reopenedSequenceAndOverwrite},
        {"cross_process_exclusive_lock", crossProcessLock},
        {"multi_thread", concurrency}, {"randomized_reference_model", modelCheck},
        {"crash_after_sst_rename", [] { crashPoint("after_sst_rename"); }},
        {"crash_after_manifest_rename", [] { crashPoint("after_manifest_rename"); }},
        {"crash_after_wal_truncate", [] { crashPoint("after_wal_truncate"); }},
        {"crash_during_compaction_before_commit", [] { crashPoint("after_compaction_sst_rename", true); }},
        {"crash_during_compaction_after_commit", [] { crashPoint("after_compaction_manifest", true); }},
        {"large_binary_value", largeValue}
    };
    for (const auto& test : tests) {
        try { test.second(); std::cout << "[PASS] " << test.first << '\n'; }
        catch (const std::exception& e) {
            std::cerr << "[FAIL] " << test.first << " : " << e.what() << '\n';
            return 1;
        }
    }
    std::cout << "ALL PASSED: " << tests.size() << '/' << tests.size() << '\n';
    return 0;
}
