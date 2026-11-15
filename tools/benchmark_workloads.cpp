#include "stonekv/db.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <numeric>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using Ns = std::chrono::nanoseconds;
struct Sample { std::string phase, op; std::size_t index, thread; std::int64_t latency_ns; };
static std::string key(char prefix, std::size_t i) {
    std::ostringstream out; out << prefix << std::setw(12) << std::setfill('0') << i; return out.str();
}
static std::int64_t ns(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration_cast<Ns>(b-a).count();
}
int main(int argc, char** argv) {
 try {
    if (argc != 6) throw std::invalid_argument("usage: benchmark_workloads <new_run_dir> <count> <sync:0|1> <seed> <threads>");
    fs::path root(argv[1]); const auto count=std::stoull(argv[2]); const std::string syncarg(argv[3]);
    const auto seed=std::stoull(argv[4]); const auto threads=std::stoull(argv[5]);
    if(count<4 || count>1000000 || (syncarg!="0" && syncarg!="1") || threads<1 || threads>64)
        throw std::invalid_argument("count=4..1000000, sync=0|1, threads=1..64");
    if (!fs::create_directory(root)) throw std::runtime_error("run directory must not already exist");
    std::ofstream samples(root/"latencies.csv"), phases(root/"phases.csv");
    if(!samples || !phases) throw std::runtime_error("cannot create CSVs");
    samples<<"phase,operation,index,thread,latency_ns\n"; phases<<"phase,operations,wall_ns\n";
    stonekv::Options opt; opt.sync_writes=syncarg=="1"; opt.memtable_max_bytes=128*1024; opt.compaction_trigger=1000;
    std::vector<Sample> data; data.reserve(static_cast<std::size_t>(count)*5);
    auto db=std::make_unique<stonekv::DB>((root/"db").string(),opt);
    auto phase=[&](const std::string& name,auto fn) { auto before=data.size(); auto a=Clock::now(); fn(); auto b=Clock::now(); phases<<name<<','<<(data.size()-before)<<','<<ns(a,b)<<'\n'; };
    auto measure=[&](const char* phase_name,const char* op,std::size_t i,auto fn) { auto a=Clock::now(); fn(); auto b=Clock::now(); data.push_back({phase_name,op,i,0,ns(a,b)}); };
    const std::string val(128,'x'); std::vector<std::size_t> order(count);std::iota(order.begin(),order.end(),0);std::mt19937_64 gen(seed);std::shuffle(order.begin(),order.end(),gen);
    phase("sequential_put",[&]{for(std::size_t i=0;i<count;++i)measure("sequential_put","put",i,[&]{db->Put(key('s',i),val);});});
    phase("flush_initial",[&]{measure("flush_initial","flush",0,[&]{db->Flush();});});
    phase("random_put",[&]{for(auto i:order)measure("random_put","put",i,[&]{db->Put(key('r',i),val);});});
    phase("random_get",[&]{for(auto i:order)measure("random_get","get",i,[&]{if(db->Get(key('s',i))!=val)throw std::runtime_error("random_get mismatch");});});
    const auto scans=std::min<std::size_t>(count,100);
    phase("range_scan",[&]{for(std::size_t j=0;j<scans;++j){const auto i=order[j];measure("range_scan","scan",j,[&]{auto rows=db->Scan(key('s',i),"",16);if(rows.empty() || rows.front().first!=key('s',i))throw std::runtime_error("scan mismatch");});}});
    const std::string newer(128,'y');
    phase("overwrite",[&]{for(std::size_t i=0;i<count;++i)measure("overwrite","put",i,[&]{db->Put(key('s',i),newer);});});
    phase("delete",[&]{for(std::size_t i=0;i<count/4;++i)measure("delete","delete",i,[&]{db->Delete(key('s',i));});});
    phase("flush_final",[&]{measure("flush_final","flush",0,[&]{db->Flush();});});
    phase("compaction",[&]{measure("compaction","compact",0,[&]{db->Compact();});});
    phase("concurrent_mixed",[&]{
      std::vector<std::vector<Sample>> per(threads);std::vector<std::thread> ts;std::exception_ptr failure;std::mutex fail_mu;
      try {
        for(std::size_t t=0;t<threads;++t)ts.emplace_back([&,t]{try{for(std::size_t i=t;i<count;i+=threads){auto a=Clock::now();if(i%2==0)db->Put(key('c',i),val);else if(db->Get(key('s',count-1))!=newer)throw std::runtime_error("concurrent get mismatch");auto b=Clock::now();per[t].push_back({"concurrent_mixed",i%2==0?"put":"get",i,t,ns(a,b)});}}catch(...){std::lock_guard<std::mutex> l(fail_mu);if(!failure)failure=std::current_exception();}});
      } catch (...) { for(auto& t:ts) t.join(); throw; }
      for(auto& t:ts) t.join();
      if(failure) std::rethrow_exception(failure);
      for(auto& v:per) data.insert(data.end(),v.begin(),v.end());
    });
    phase("close",[&]{measure("close","close",0,[&]{db.reset();});});
    phase("reopen",[&]{measure("reopen","open",0,[&]{db=std::make_unique<stonekv::DB>((root/"db").string(),opt);});});
    if(db->Get(key('s',0)) || db->Get(key('s',count-1))!=newer || db->Get(key('r',0))!=val)throw std::runtime_error("reopen verification mismatch");
    db.reset();
    for(const auto& s:data)samples<<s.phase<<','<<s.op<<','<<s.index<<','<<s.thread<<','<<s.latency_ns<<'\n';
    samples.flush();phases.flush();if(!samples || !phases)throw std::runtime_error("CSV write failed");
    std::cout<<"PASS count="<<count<<" sync="<<syncarg<<" seed="<<seed<<" threads="<<threads<<" samples="<<data.size()<<'\n';return 0;
 }catch(const std::exception& e){std::cerr<<"ERROR: "<<e.what()<<'\n';return 1;}
}
