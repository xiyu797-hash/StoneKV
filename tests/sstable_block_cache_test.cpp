#include "stonekv/db.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>
static std::atomic<unsigned> reads{0};
extern "C" ssize_t __real_pread(int,void*,size_t,off_t);
extern "C" ssize_t __wrap_pread(int fd,void* data,size_t n,off_t off){char link[64],name[4096];std::snprintf(link,sizeof(link),"/proc/self/fd/%d",fd);auto len=::readlink(link,name,sizeof(name)-1);if(len>0){name[len]='\0';std::string p(name);if(p.size()>=4 && p.substr(p.size()-4)==".sst")++reads;}return __real_pread(fd,data,n,off);}
int main(){try{
 std::string temp="/tmp/stonekv-block-cache-XXXXXX";std::vector<char> p(temp.begin(),temp.end());p.push_back('\0');auto dir=::mkdtemp(p.data());if(!dir)throw std::runtime_error("mkdtemp");
 {stonekv::Options o;o.compaction_trigger=100;stonekv::DB db(dir,o);db.Put("alpha","one");db.Put("beta","two");db.Flush();reads=0;
  if(db.Get("alpha")!="one")throw std::runtime_error("first key mismatch");auto first=reads.load();
  if(db.Get("beta")!="two")throw std::runtime_error("second key mismatch");auto second=reads.load();
  if(first==0 || second!=first)throw std::runtime_error("same-block second Get performed SST pread");
  auto rows=db.Scan();if(rows.size()!=2 || reads.load()!=second)throw std::runtime_error("scan did not reuse cached block");
  auto stats=db.GetStats();if(stats.block_cache_hits<2 || stats.block_cache_misses!=1 || stats.block_cache_bytes==0 || stats.block_cache_bytes>o.block_cache_capacity_bytes)throw std::runtime_error("cache accounting mismatch");
  db.Put("alpha","new");db.Flush();db.Compact();if(db.Get("alpha")!="new")throw std::runtime_error("compaction cache invalidation mismatch");
 }
 {stonekv::DB db(dir);if(db.Get("alpha")!="new"||db.Get("beta")!="two")throw std::runtime_error("reopen mismatch");}
 {stonekv::Options o;o.block_cache_capacity_bytes=0;stonekv::DB db(dir,o);reads=0;if(db.Get("alpha")!="new"||db.Get("alpha")!="new"||reads.load()<2||db.GetStats().block_cache_bytes!=0)throw std::runtime_error("disabled cache mismatch");}
 {stonekv::Options o;o.block_cache_capacity_bytes=1;stonekv::DB db(dir,o);if(db.Get("alpha")!="new"||db.GetStats().block_cache_bytes!=0)throw std::runtime_error("oversized block exceeded capacity");}
 {stonekv::Options o;o.block_cache_capacity_bytes=257U*1024U*1024U;bool rejected=false;try{stonekv::DB db(dir,o);}catch(const std::exception&){rejected=true;}if(!rejected)throw std::runtime_error("invalid cache capacity accepted");}
 std::string t2="/tmp/stonekv-block-evict-XXXXXX";std::vector<char> p2(t2.begin(),t2.end());p2.push_back('\0');auto dir2=::mkdtemp(p2.data());if(!dir2)throw std::runtime_error("mkdtemp eviction");
 {stonekv::Options o;o.sync_writes=false;o.memtable_max_bytes=1024*1024;o.block_cache_capacity_bytes=64*1024;stonekv::DB db(dir2,o);
  for(int i=0;i<700;++i)db.Put("key"+std::to_string(i),std::string(128,'x'));db.Flush();
  if(!db.Get("key0") || !db.Get("key699"))throw std::runtime_error("multi-block read mismatch");auto before=db.GetStats();
  if(!db.Get("key0"))throw std::runtime_error("evicted block read mismatch");auto after=db.GetStats();
  if(after.block_cache_bytes>o.block_cache_capacity_bytes || after.block_cache_misses!=before.block_cache_misses+1)throw std::runtime_error("block LRU eviction mismatch");
 }
 std::string t3="/tmp/stonekv-sparse-scan-XXXXXX";std::vector<char> p3(t3.begin(),t3.end());p3.push_back('\0');auto dir3=::mkdtemp(p3.data());if(!dir3)throw std::runtime_error("mkdtemp scan");
 {stonekv::Options o;o.sync_writes=false;o.memtable_max_bytes=8*1024*1024;o.block_cache_capacity_bytes=0;stonekv::DB db(dir3,o);
  for(int i=0;i<4000;++i){char key[32];std::snprintf(key,sizeof(key),"k%05d",i);db.Put(key,std::string(128,'x'));}db.Flush();reads=0;
  const auto rows=db.Scan("k02000","",1);const auto scanned=reads.load();
  if(rows.size()!=1 || rows[0].first!="k02000")throw std::runtime_error("bounded sparse scan mismatch");
  if(scanned>4)throw std::runtime_error("bounded scan read unrelated blocks: "+std::to_string(scanned));
 }
 std::string t4="/tmp/stonekv-scan-range-prune-XXXXXX";std::vector<char> p4(t4.begin(),t4.end());p4.push_back('\0');auto dir4=::mkdtemp(p4.data());if(!dir4)throw std::runtime_error("mkdtemp range prune");
 {stonekv::Options o;o.sync_writes=false;o.compaction_trigger=100;o.block_cache_capacity_bytes=0;stonekv::DB db(dir4,o);
  for(int i=0;i<10;++i){db.Put("a"+std::to_string(i),"v");db.Flush();}db.Put("z0","ok");db.Flush();reads=0;
  auto rows=db.Scan("z","",1);auto calls=reads.load();
  if(rows.size()!=1||rows[0].first!="z0"||rows[0].second!="ok")throw std::runtime_error("range prune wrong result");
  if(calls>4)throw std::runtime_error("scan read SSTs outside key range: "+std::to_string(calls));
 }
 std::string t5="/tmp/stonekv-get-range-prune-XXXXXX";std::vector<char> p5(t5.begin(),t5.end());p5.push_back('\0');auto dir5=::mkdtemp(p5.data());if(!dir5)throw std::runtime_error("mkdtemp get prune");
 {stonekv::Options o;o.sync_writes=false;o.compaction_trigger=100;o.block_cache_capacity_bytes=0;stonekv::DB db(dir5,o);
  for(int i=0;i<10;++i){for(int j=0;j<100;++j)db.Put("a"+std::to_string(i)+"-"+std::to_string(j),"v");db.Flush();}reads=0;
  for(int i=0;i<10000;++i)if(db.Get("z"+std::to_string(i)))throw std::runtime_error("out of range get found key");
  if(reads.load()!=0)throw std::runtime_error("out of range get read SST blocks: "+std::to_string(reads.load()));
 }
 std::cout<<"PASS block cache read reuse and invalidation fixture="<<dir<<'\n';return 0;
}catch(const std::exception&e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
