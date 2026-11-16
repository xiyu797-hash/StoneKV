#include "stonekv/db.hpp"
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
using Clock=std::chrono::steady_clock;
static std::string key(std::size_t i){std::ostringstream s;s<<'k'<<std::setw(12)<<std::setfill('0')<<i<<std::string(48,'x');return s.str();}
int main(int argc,char**argv){try{
 if(argc!=6)throw std::invalid_argument("usage: sst_index_probe create|read db_dir key_count reads seed");
 const std::string mode=argv[1],path=argv[2];const auto count=std::stoull(argv[3]),reads=std::stoull(argv[4]),seed=std::stoull(argv[5]);
 if(count<1||count>1000000||reads>1000000)throw std::invalid_argument("invalid count/reads");
 stonekv::Options options;options.memtable_max_bytes=64U*1024U*1024U;options.compaction_trigger=100;options.sync_writes=false;
 if(mode=="create"){
  if(std::filesystem::exists(path))throw std::runtime_error("database path must not exist");stonekv::DB db(path,options);for(std::uint64_t i=0;i<count;++i)db.Put(key(i),std::string(256,'v'));db.Flush();std::cout<<"created="<<count<<"\n";return 0;
 }
 if(mode!="read")throw std::invalid_argument("invalid mode");
 auto start=Clock::now();stonekv::DB db(path,options);auto opened=Clock::now();
 std::cout<<"open_us="<<std::chrono::duration_cast<std::chrono::microseconds>(opened-start).count()<<"\n";
#ifdef STONEKV_HAS_INDEX_STATS
 auto stats=db.GetStats();std::cout<<"sst_full_index_entries="<<stats.sst_full_index_entries<<"\nsst_sparse_blocks="<<stats.sst_sparse_blocks<<"\n";
#endif
 std::mt19937_64 rng(seed);std::cout<<"index,latency_ns\n";for(std::uint64_t i=0;i<reads;++i){const auto k=key(static_cast<std::size_t>(rng()%count));auto a=Clock::now();auto v=db.Get(k);auto b=Clock::now();if(!v||v->size()!=256)throw std::runtime_error("read mismatch");std::cout<<i<<','<<std::chrono::duration_cast<std::chrono::nanoseconds>(b-a).count()<<'\n';}if(!std::cout)throw std::runtime_error("output failure");return 0;
}catch(const std::exception&e){std::cerr<<"ERROR: "<<e.what()<<'\n';return 1;}}
