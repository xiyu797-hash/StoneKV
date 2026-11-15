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
#include <iomanip>
#include <limits>
#include <map>
#include <random>
#include <sstream>
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
    const char x = 'x'; require(::pwrite(fd, &x, 1, 29) == 1, "inject payload corruption");
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
void blockSstFormatAndLegacyCompatibility() {
    auto u32=[](std::string& s,std::uint32_t x){for(int i=0;i<4;++i)s.push_back(static_cast<char>((x>>(8*i))&255U));};
    auto u64=[](std::string& s,std::uint64_t x){for(int i=0;i<8;++i)s.push_back(static_cast<char>((x>>(8*i))&255U));};
    auto crc=[](const std::string& s){std::uint32_t x=0xffffffffU;for(unsigned char c:s){x^=c;for(int i=0;i<8;++i)x=(x>>1U)^((x&1U)?0xedb88320U:0U);}return ~x;};
    Tmp t; {stonekv::DB db(t.path.string());}
    std::string entry;u64(entry,1);entry.push_back(1);u32(entry,3);u32(entry,3);const auto checksum=crc(entry+"oldone");u32(entry,checksum);entry+="oldone";
    std::string old("STONEKV1",8);u64(old,1);old+=entry;
    {std::ofstream out(t.path/"SST-000000000001.sst",std::ios::binary);out.write(old.data(),static_cast<std::streamsize>(old.size()));require(bool(out),"write independent V1 SST");}
    const std::string payload="STONEKV_MANIFEST_V1\n1 2 1\n1\n";
    {std::ofstream out(t.path/"MANIFEST",std::ios::binary|std::ios::trunc);out<<payload<<"crc32 "<<std::hex<<std::setw(8)<<std::setfill('0')<<crc(payload)<<'\n';require(bool(out),"write V1 fixture manifest");}
    {stonekv::DB db(t.path.string(),opt(1024*1024,100));require(db.Get("old")=="one","V1 table read");db.Put("new","two");db.Flush();}
    {std::ifstream in(t.path/"SST-000000000002.sst",std::ios::binary);char magic[8]{};in.read(magic,8);require(in.gcount()==8 && std::string(magic,8)=="STONEKV2","new tables must use block V2 format");}
    {stonekv::DB db(t.path.string(),opt(1024*1024,100));require(db.Get("old")=="one" && db.Get("new")=="two","mixed V1/V2 reopen");db.Compact();}
    {stonekv::DB db(t.path.string());require(db.Get("old")=="one" && db.Get("new")=="two","mixed version compaction");}
}
void blockSstCorruptionAndBoundaries() {
    for (int mode=0; mode<5; ++mode) {
        Tmp t;
        { stonekv::DB db(t.path.string(),opt(1024*1024,100));
          for(int i=0;i<700;++i)db.Put("key-"+std::to_string(i),std::string(128,static_cast<char>('a'+i%26)));
          db.Flush(); }
        fs::path file;
        for(const auto& e:fs::directory_iterator(t.path))if(e.path().extension()==".sst")file=e.path();
        require(!file.empty(),"find V2 block SST");
        int fd=::open(file.c_str(),O_RDWR);require(fd>=0,"open V2 SST");
        unsigned char h[4]{};require(::pread(fd,h,4,16)==4,"read first block length");
        const std::uint32_t len=std::uint32_t(h[0])|(std::uint32_t(h[1])<<8U)|(std::uint32_t(h[2])<<16U)|(std::uint32_t(h[3])<<24U);
        require(len>0 && fs::file_size(file)>16+8+len,"fixture has multiple blocks");
        if(mode==1){const char zero[4]={};require(::pwrite(fd,zero,4,16)==4,"zero block length");}
        if(mode==2){char byte=0;require(::pread(fd,&byte,1,20)==1,"read block CRC");byte^=1;require(::pwrite(fd,&byte,1,20)==1,"corrupt block CRC");}
        if(mode==3){require(::ftruncate(fd,static_cast<off_t>(fs::file_size(file)-1))==0,"truncate final block");}
        if(mode==4){const char bad='9';require(::pwrite(fd,&bad,1,7)==1,"unsupported format version");}
        require(::close(fd)==0,"close V2 fixture");
        if(mode==0){stonekv::DB db(t.path.string());for(int i=0;i<700;++i)require(db.Get("key-"+std::to_string(i))==std::string(128,static_cast<char>('a'+i%26)),"multi-block read");}
        else expectThrow([&]{stonekv::DB db(t.path.string());},"corrupt or unknown V2 SST rejected");
    }
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
void missingWalFailsClosed() {
    Tmp t;
    { stonekv::DB db(t.path.string()); db.Put("acknowledged", "value"); }
    require(fs::remove(t.path / "active.wal"), "remove existing WAL");
    expectThrow([&] { stonekv::DB db(t.path.string()); }, "missing WAL must not silently lose acknowledged data");
    require(!fs::exists(t.path / "active.wal"), "missing WAL must not be recreated");
}
void manifestTrailingGarbageFailsClosed() {
    Tmp t;
    { stonekv::DB db(t.path.string()); db.Put("key", "value"); db.Flush(); }
    { std::ofstream out(t.path / "MANIFEST", std::ios::app | std::ios::binary); out << "garbage\n"; }
    expectThrow([&] { stonekv::DB db(t.path.string()); }, "MANIFEST trailing garbage");
}
void exhaustedSstIdFailsClosed() {
    // Construct a checksummed MANIFEST fixture, so errors cannot be attributed
    // to the parser rejecting an intentionally damaged checksum.
    auto rewrite = [](const fs::path& dir, std::uint64_t durable, const std::vector<std::uint64_t>& ids) {
        std::ostringstream body;
        body << "STONEKV_MANIFEST_V1\n" << durable << ' ' << std::numeric_limits<std::uint64_t>::max() << ' ' << ids.size() << '\n';
        for (auto id : ids) body << id << '\n';
        const std::string content = body.str();
        std::uint32_t crc = 0xffffffffU;
        for (unsigned char c : content) { crc ^= c; for (int i = 0; i != 8; ++i) crc = (crc >> 1U) ^ ((crc & 1U) ? 0xedb88320U : 0U); }
        std::ofstream out(dir / "MANIFEST", std::ios::binary | std::ios::trunc);
        out << content << "crc32 " << std::hex << std::setw(8) << std::setfill('0') << ~crc << '\n';
        require(bool(out), "write valid exhausted-id MANIFEST fixture");
    };
    for (bool compact : {false, true}) {
        Tmp t;
        {
            stonekv::DB db(t.path.string(), opt(1024 * 1024, 100));
            db.Put("ack", "value");
            if (compact) { db.Flush(); db.Put("second", "value2"); db.Flush(); }
        }
        rewrite(t.path, compact ? 2 : 0, compact ? std::vector<std::uint64_t>{1, 2} : std::vector<std::uint64_t>{});
        const auto manifest_before = fs::file_size(t.path / "MANIFEST");
        const auto wal_before = fs::file_size(t.path / "active.wal");
        {
            stonekv::DB db(t.path.string(), opt(1024 * 1024, 100));
            require(db.Get("ack") == "value", "fixture opens and ACK is readable");
            if (compact) expectThrow([&] { db.Compact(); }, "compaction must reject exhausted SST id");
            else expectThrow([&] { db.Flush(); }, "flush must reject exhausted SST id");
        }
        require(fs::file_size(t.path / "MANIFEST") == manifest_before && fs::file_size(t.path / "active.wal") == wal_before, "exhaustion must not publish or truncate");
        require(!fs::exists(t.path / "SST-18446744073709551615.sst") && !fs::exists(t.path / "SST-18446744073709551615.sst.tmp"), "exhaustion must fail before creating SST");
        { stonekv::DB db(t.path.string()); require(db.Get("ack") == "value", "acknowledged value survives exhaustion"); if (compact) require(db.Get("second") == "value2", "second SST survives exhaustion"); }
    }
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
void batchAtomic() {
    using Op = stonekv::BatchOperation;
    Tmp t;
    {
        stonekv::DB db(t.path.string(), opt(1024, 100));
        db.Put("keep", "old");
        db.Put("erase", "value");
        db.Flush();
        const auto before = db.GetStats().latest_sequence;
        db.WriteBatch({{Op::Type::Put, "keep", "new"},
                       {Op::Type::Put, "repeated", "first"},
                       {Op::Type::Delete, "erase", ""},
                       {Op::Type::Put, "repeated", "last"},
                       {Op::Type::Put, "binary", std::string("x\0y", 3)}});
        require(db.GetStats().latest_sequence == before + 1, "batch allocates one sequence");
        require(db.Get("keep") == "new", "batch overwrite");
        require(!db.Get("erase"), "batch delete");
        require(db.Get("repeated") == "last", "repeated batch key last wins");
        require(db.Get("binary") == std::string("x\0y", 3), "batch binary value");
        db.Flush(); db.Compact();
    }
    {
        stonekv::DB db(t.path.string());
        require(db.Get("keep") == "new" && !db.Get("erase"), "batch survives compaction reopen");
        require(db.Get("repeated") == "last", "batch order survives reopen");
    }
}
void batchValidation() {
    using Op = stonekv::BatchOperation;
    Tmp t;
    stonekv::DB db(t.path.string());
    db.WriteBatch({});
    require(db.GetStats().latest_sequence == 0, "empty batch no-op");
    expectThrow([&] { db.WriteBatch({{Op::Type::Delete, "key", "invalid"}}); }, "delete value forbidden");
    expectThrow([&] { db.WriteBatch({{Op::Type::Put, "", "bad"}}); }, "empty batch key");
    std::vector<Op> too_many(4097, {Op::Type::Put, "key", "value"});
    expectThrow([&] { db.WriteBatch(too_many); }, "max batch op count");
    require(db.GetStats().latest_sequence == 0, "validation failures do not poison db");
    db.WriteBatch({{Op::Type::Put, "valid", "ok"}});
    require(db.Get("valid") == "ok", "batch after invalid input");
}
void batchTornTail() {
    using Op = stonekv::BatchOperation;
    Tmp t;
    { stonekv::DB db(t.path.string()); db.Put("safe", "yes"); }
    const auto wal = t.path / "active.wal";
    const auto before = fs::file_size(wal);
    { std::ofstream out(wal, std::ios::binary | std::ios::app); out.write("SKB1\x01\x02", 6); }
    stonekv::DB db(t.path.string());
    require(db.Get("safe") == "yes", "torn batch preserves previous writes");
    require(db.GetStats().wal_bytes == before, "torn batch truncated as one record");
    db.WriteBatch({{Op::Type::Put, "after", "ok"}});
    require(db.Get("after") == "ok", "writes after torn batch recover");
}
void batchCorruption() {
    using Op = stonekv::BatchOperation;
    Tmp t;
    { stonekv::DB db(t.path.string()); db.WriteBatch({{Op::Type::Put, "key", "value"}}); }
    const auto wal = t.path / "active.wal";
    const int fd = ::open(wal.c_str(), O_WRONLY);
    require(fd >= 0, "open batch WAL for corruption");
    const char x = 'z';
    require(::pwrite(fd, &x, 1, 28 + 9 + 3) == 1, "flip batch payload byte");
    ::close(fd);
    expectThrow([&] { stonekv::DB db(t.path.string()); }, "WAL batch checksum must fail closed");
}
void batchCrashReplay() {
    using Op = stonekv::BatchOperation;
    Tmp t;
    const auto pid = ::fork();
    require(pid >= 0, "fork batch crash");
    if (pid == 0) {
        ::setenv("STONEKV_FAILPOINT", "after_batch_wal_sync", 1);
        stonekv::DB db(t.path.string());
        db.WriteBatch({{Op::Type::Put, "first", "one"},
                       {Op::Type::Put, "second", "two"},
                       {Op::Type::Delete, "third", ""}});
        ::_exit(0);
    }
    int status = 0;
    require(::waitpid(pid, &status, 0) == pid, "wait batch crash");
    require(WIFEXITED(status) && WEXITSTATUS(status) == 86, "expected batch WAL sync crash");
    stonekv::DB db(t.path.string());
    require(db.Get("first") == "one" && db.Get("second") == "two", "whole batch recovered");
    require(!db.Get("third"), "batch tombstone recovered");
    require(db.GetStats().latest_sequence == 1, "batch sequence after crash");
}
void walLengthHeaderCorruptionFailsClosed() {
    using Op = stonekv::BatchOperation;
    for (bool batch : {false, true}) {
        Tmp t;
        {
            stonekv::DB db(t.path.string());
            if (batch) db.WriteBatch({{Op::Type::Put, "key", "value"}});
            else db.Put("key", "value");
        }
        const auto wal = t.path / "active.wal";
        const auto size = fs::file_size(wal);
        int fd = ::open(wal.c_str(), O_RDWR);
        require(fd >= 0, "open WAL to corrupt length");
        const off_t field = batch ? 16 : 17;
        unsigned char byte = 0;
        require(::pread(fd, &byte, 1, field) == 1, "read length byte");
        ++byte; // still within legal range but larger than the actual payload
        require(::pwrite(fd, &byte, 1, field) == 1, "corrupt length byte");
        require(::close(fd) == 0, "close WAL");
        expectThrow([&] { stonekv::DB db(t.path.string()); }, "corrupt WAL length must not be mistaken for torn tail");
        require(fs::file_size(wal) == size, "corrupt WAL must remain intact for diagnosis");
    }
}
void v2MagicCorruptionFailsClosed() {
    using Op = stonekv::BatchOperation;
    for (bool batch : {false, true}) for (char changed : {'X', '1'}) {
        Tmp t;
        { stonekv::DB db(t.path.string());
          if (batch) db.WriteBatch({{Op::Type::Put, "key", "value"}}); else db.Put("key", "value"); }
        const auto wal = t.path / "active.wal";
        const auto size = fs::file_size(wal);
        int fd = ::open(wal.c_str(), O_RDWR);
        require(fd >= 0, "open WAL magic");
        require(::pwrite(fd, &changed, 1, 3) == 1, "change V2 magic, including downgrade to V1");
        require(::close(fd) == 0, "close WAL magic");
        expectThrow([&] { stonekv::DB db(t.path.string()); }, "corrupted V2 magic must fail closed");
        require(fs::file_size(wal) == size, "magic-corrupt WAL left intact");
    }
}
void legacyWalMixedVersionReplay() {
    // Construct V1 records independently so compatibility does not depend on the V2 writer.
    auto u32 = [](std::string& s, std::uint32_t x) { for (int i = 0; i != 4; ++i) s.push_back(static_cast<char>((x >> (8 * i)) & 255U)); };
    auto u64 = [](std::string& s, std::uint64_t x) { for (int i = 0; i != 8; ++i) s.push_back(static_cast<char>((x >> (8 * i)) & 255U)); };
    auto crc = [](const std::string& s) {
        std::uint32_t x = 0xffffffffU;
        for (unsigned char c : s) {
            x ^= c;
            for (int i = 0; i != 8; ++i) x = (x >> 1U) ^ ((x & 1U) ? 0xedb88320U : 0U);
        }
        return ~x;
    };
    Tmp t;
    { stonekv::DB db(t.path.string()); }
    std::string legacy("SKW1", 4), h;
    u64(h, 1); h.push_back(1); u32(h, 3); u32(h, 3);
    legacy += h; u32(legacy, crc(h + "oldone")); legacy += "oldone";
    std::string payload; payload.push_back(1); u32(payload, 3); u32(payload, 3); payload += "newtwo";
    std::string bh; u64(bh, 2); u32(bh, 1); u32(bh, static_cast<std::uint32_t>(payload.size()));
    legacy += "SKB1"; legacy += bh; u32(legacy, crc(bh + payload)); legacy += payload;
    { std::ofstream f(t.path / "active.wal", std::ios::binary | std::ios::trunc); f.write(legacy.data(), static_cast<std::streamsize>(legacy.size())); require(bool(f), "write V1 fixture"); }
    { stonekv::DB db(t.path.string()); require(db.Get("old") == "one" && db.Get("new") == "two", "V1 replay"); db.Put("v2", "three"); }
    { stonekv::DB db(t.path.string()); require(db.Get("old") == "one" && db.Get("new") == "two" && db.Get("v2") == "three", "mixed V1/V2 replay"); }
}
void v2IncompleteTailPreservesPriorRecord() {
    using Op = stonekv::BatchOperation;
    for (bool batch : {false, true}) {
        Tmp source, target;
        std::uint64_t offset = 0;
        {
            stonekv::DB db(source.path.string()); db.Put("safe", "yes"); offset = db.GetStats().wal_bytes;
            if (batch) db.WriteBatch({{Op::Type::Put, "pending", "value"}});
            else db.Put("pending", "value");
        }
        { stonekv::DB db(target.path.string()); db.Put("safe", "yes"); }
        const auto wal = target.path / "active.wal";
        require(fs::file_size(wal) == offset, "matching record prefix");
        std::ifstream f(source.path / "active.wal", std::ios::binary);
        f.seekg(static_cast<std::streamoff>(offset));
        std::string record((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        require(record.size() > 30, "complete V2 source record");
        record.pop_back();
        { std::ofstream out(wal, std::ios::binary | std::ios::app); out.write(record.data(), static_cast<std::streamsize>(record.size())); require(bool(out), "append incomplete record"); }
        { stonekv::DB db(target.path.string()); require(db.Get("safe") == "yes" && !db.Get("pending"), "incomplete tail replay"); require(db.GetStats().wal_bytes == offset, "incomplete tail truncated"); }
    }
}
} // namespace

int main() {
    using Test = std::pair<const char*, std::function<void()>>;
    const std::vector<Test> tests{
        {"wal_length_header_corruption_fails_closed", walLengthHeaderCorruptionFailsClosed},
        {"v2_magic_corruption_fails_closed", v2MagicCorruptionFailsClosed},
        {"legacy_wal_mixed_version_replay", legacyWalMixedVersionReplay},
        {"v2_incomplete_tail_preserves_prior_record", v2IncompleteTailPreservesPriorRecord},
        {"exhausted_sst_id_fails_closed", exhaustedSstIdFailsClosed},
        {"basic_and_reopen", basic}, {"binary_values", binaryValues}, {"ordered_scan", scan},
        {"flush_and_reopen", flushReopen}, {"compaction_and_tombstones", compaction},
        {"auto_compaction", automaticCompaction}, {"input_and_exclusive_lock", validationAndLock},
        {"torn_wal_tail", tornWalTail}, {"corrupt_wal_fails_closed", corruptWal},
        {"block_sst_and_legacy_compatibility", blockSstFormatAndLegacyCompatibility},
        {"block_sst_corruption_and_boundaries", blockSstCorruptionAndBoundaries},
        {"corrupt_sst_fails_closed", corruptSst}, {"missing_manifest_fails_closed", missingManifest},
        {"corrupt_manifest_fails_closed", corruptManifest},
        {"manifest_trailing_garbage_fails_closed", manifestTrailingGarbageFailsClosed},
        {"missing_wal_fails_closed", missingWalFailsClosed},
        {"max_key_edge", maxKeyAndLimit},
        {"reopen_sequence_overwrite", reopenedSequenceAndOverwrite},
        {"cross_process_exclusive_lock", crossProcessLock},
        {"multi_thread", concurrency}, {"randomized_reference_model", modelCheck},
        {"crash_after_sst_rename", [] { crashPoint("after_sst_rename"); }},
        {"crash_after_manifest_rename", [] { crashPoint("after_manifest_rename"); }},
        {"crash_after_wal_truncate", [] { crashPoint("after_wal_truncate"); }},
        {"crash_during_compaction_before_commit", [] { crashPoint("after_compaction_sst_rename", true); }},
        {"crash_during_compaction_after_commit", [] { crashPoint("after_compaction_manifest", true); }},
        {"large_binary_value", largeValue},
        {"batch_atomic_and_reopen", batchAtomic},
        {"batch_validation", batchValidation},
        {"batch_torn_tail", batchTornTail},
        {"batch_crc_fails_closed", batchCorruption},
        {"batch_crash_replay", batchCrashReplay}
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
