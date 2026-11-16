// Count allocations while scanning an SST with and without unrelated active
// keys. The result range is identical; unrelated keys should not be copied.
#include "stonekv/db.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>

static std::atomic<bool> counting{false};
static std::atomic<std::size_t> allocations{0};
void* operator new(std::size_t n) {
    if(counting.load(std::memory_order_relaxed)) allocations.fetch_add(1,std::memory_order_relaxed);
    if(void* p=std::malloc(n?n:1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p,std::size_t) noexcept { std::free(p); }

static std::size_t run(bool extra) {
    std::string temp="/tmp/stonekv-scan-bounds-XXXXXX";std::vector<char> buffer(temp.begin(),temp.end());buffer.push_back('\0');
    const auto dir=::mkdtemp(buffer.data());if(!dir)throw std::runtime_error("mkdtemp");
    stonekv::Options options;options.sync_writes=false;options.memtable_max_bytes=1024*1024;options.compaction_trigger=100;
    stonekv::DB db(dir,options);char key[32];
    for(int i=0;i<200;++i){std::snprintf(key,sizeof(key),"s%012d",i);db.Put(key,std::string(128,'x'));}
    db.Flush();
    if(extra)for(int i=0;i<300;++i){std::snprintf(key,sizeof(key),"r%012d",i);db.Put(key,std::string(128,'y'));}
    const std::string begin="s000000000100",end="s000000000116";
    if(db.Scan(begin,end,16).size()!=16)throw std::runtime_error("scan warmup mismatch");
    allocations=0;counting=true;const auto rows=db.Scan(begin,end,16);counting=false;
    if(rows.size()!=16 || rows.front().first!=begin || rows.back().first!="s000000000115")throw std::runtime_error("scan result mismatch");
    return allocations.load();
}
int main(){try{
    const auto clean=run(false), unrelated=run(true);
    std::cout<<"allocations without unrelated keys="<<clean<<" with unrelated keys="<<unrelated<<'\n';
    if(unrelated>clean+50)throw std::runtime_error("out-of-range active keys increase scan allocations");
    return 0;
}catch(const std::exception& e){counting=false;std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
