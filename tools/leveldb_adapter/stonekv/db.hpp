#pragma once
// Benchmark-only compatibility adapter. LevelDB is linked separately and is
// not a StoneKV runtime dependency. TEST_CompactMemTable is an internal API.
#include <leveldb/cache.h>
#include <leveldb/db.h>
#include <leveldb/filter_policy.h>
#include <leveldb/options.h>
#include <leveldb/iterator.h>
#include "db/db_impl.h"
#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace stonekv {
struct Options {
    std::size_t memtable_max_bytes=4U*1024U*1024U;
    std::size_t compaction_trigger=4; // No direct LevelDB equivalent.
    bool sync_writes=true;
    std::size_t block_cache_capacity_bytes=8U*1024U*1024U;
};
class DB {
    std::unique_ptr<leveldb::Cache> cache_;
    std::unique_ptr<const leveldb::FilterPolicy> bloom_;
    std::unique_ptr<leveldb::DB> db_;
    leveldb::WriteOptions write_;
    static void check(const leveldb::Status& s) { if(!s.ok())throw std::runtime_error(s.ToString()); }
public:
    explicit DB(const std::string& path,Options settings={}) {
        leveldb::Options o;
        o.create_if_missing=true;
        o.write_buffer_size=settings.memtable_max_bytes;
        o.compression=leveldb::kNoCompression;
        cache_.reset(leveldb::NewLRUCache(settings.block_cache_capacity_bytes));
        bloom_.reset(leveldb::NewBloomFilterPolicy(10));
        o.block_cache=cache_.get();o.filter_policy=bloom_.get();
        leveldb::DB* raw=nullptr;check(leveldb::DB::Open(o,path,&raw));db_.reset(raw);
        write_.sync=settings.sync_writes;
    }
    void Put(const std::string& k,const std::string& v) { check(db_->Put(write_,k,v)); }
    void Delete(const std::string& k) { check(db_->Delete(write_,k)); }
    std::optional<std::string> Get(const std::string& k) const {
        std::string v;const auto s=db_->Get(leveldb::ReadOptions{},k,&v);
        if(s.IsNotFound())return std::nullopt;check(s);return v;
    }
    std::vector<std::pair<std::string,std::string>> Scan(const std::string& begin,const std::string& end,std::size_t limit) const {
        std::vector<std::pair<std::string,std::string>> out;
        std::unique_ptr<leveldb::Iterator> it(db_->NewIterator(leveldb::ReadOptions{}));
        for(it->Seek(begin);it->Valid()&&out.size()<limit;it->Next()) {
            const auto k=it->key().ToString();if(!end.empty() && k>=end)break;
            out.emplace_back(k,it->value().ToString());
        }
        check(it->status());return out;
    }
    void Flush() { check(static_cast<leveldb::DBImpl*>(db_.get())->TEST_CompactMemTable()); }
    void Compact() { db_->CompactRange(nullptr,nullptr); }
};
}
