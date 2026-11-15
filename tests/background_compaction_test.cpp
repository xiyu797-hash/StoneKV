// A blocked compaction SST write must not stop ordinary reads and writes.
#include "stonekv/db.hpp"
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <cstdio>
#include <unistd.h>
#include <sys/wait.h>
#include <cstdlib>
using namespace std::chrono_literals;
static std::atomic<bool> armed{false}, entered{false}, released{false};
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd) {
 char link[64],name[4096];std::snprintf(link,sizeof(link),"/proc/self/fd/%d",fd);auto n=::readlink(link,name,sizeof(name)-1);
 if(n>0){name[n]='\0';if(armed.load() && std::string(name).find(".sst.tmp")!=std::string::npos){entered=true;while(!released.load())std::this_thread::sleep_for(1ms);}}
 return __real_fsync(fd);
}
int main(){try{
 std::string t="/tmp/stonekv-compact-test-XXXXXX";std::vector<char> p(t.begin(),t.end());p.push_back('\0');auto dir=::mkdtemp(p.data());if(!dir)throw std::runtime_error("mkdtemp");
 {stonekv::Options o;o.memtable_max_bytes=1024;o.compaction_trigger=100;stonekv::DB db(dir,o);
  db.Put("old","v1");db.Flush();db.Put("old","v2");db.Put("other","x");db.Flush();
  armed=true; auto compact=std::async(std::launch::async,[&]{db.Compact();});
  auto deadline=std::chrono::steady_clock::now()+5s;while(!entered && std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(1ms);
  if(!entered){released=true;compact.wait();throw std::runtime_error("compaction did not reach SST fsync");}
  auto access=std::async(std::launch::async,[&]{db.Put("new",std::string(1500,'z'));return db.Get("old")=="v2" && db.Get("other")=="x" && db.Get("new")==std::string(1500,'z') && db.Scan().size()==3 && db.GetStats().background_tasks>=1;});
  bool progress=access.wait_for(500ms)==std::future_status::ready;bool correct=false;
  if(progress)correct=access.get();armed=false;released=true;if(!progress)access.wait();compact.get();
  if(!progress || !correct)throw std::runtime_error("reads or writes blocked during compaction");
  const auto stats=db.GetStats();if(stats.compaction_count!=1 || stats.compaction_output_bytes==0 || stats.compaction_micros==0)throw std::runtime_error("compaction statistics missing");
 }
 {stonekv::DB db(dir);if(db.Get("old")!="v2" || db.Get("new")!=std::string(1500,'z'))throw std::runtime_error("recovery mismatch");}
 // Kill after the compaction MANIFEST commit while a newer, acknowledged
 // write remains in the WAL. Reopen must retain both SST and WAL versions.
 std::string tc="/tmp/stonekv-compact-crash-XXXXXX";std::vector<char> pc(tc.begin(),tc.end());pc.push_back('\0');auto crash_dir=::mkdtemp(pc.data());if(!crash_dir)throw std::runtime_error("mkdtemp crash");
 auto pid=::fork();if(pid<0)throw std::runtime_error("fork");
 if(pid==0){try{stonekv::Options o;o.memtable_max_bytes=1024*1024;o.compaction_trigger=100;stonekv::DB db(crash_dir,o);db.Put("old","v1");db.Flush();db.Put("old","v2");db.Flush();entered=false;released=false;armed=true;auto compact=std::async(std::launch::async,[&]{db.Compact();});auto deadline=std::chrono::steady_clock::now()+5s;while(!entered && std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(1ms);if(!entered)::_exit(21);db.Put("new","ack");::setenv("STONEKV_FAILPOINT","after_compaction_manifest",1);armed=false;released=true;std::this_thread::sleep_for(5s);::_exit(22);}catch(...){::_exit(23);}}
 int status=0;if(::waitpid(pid,&status,0)!=pid || !WIFEXITED(status) || WEXITSTATUS(status)!=86)throw std::runtime_error("compaction crash fixture failed");
 {stonekv::DB db(crash_dir);if(db.Get("old")!="v2" || db.Get("new")!="ack")throw std::runtime_error("compaction crash lost acknowledged write");}
 std::cout<<"PASS compaction concurrent read/write and recovery; fixture="<<dir<<'\n';return 0;
}catch(const std::exception& e){released=true;std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
