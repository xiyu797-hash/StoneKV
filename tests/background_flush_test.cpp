// Verify acknowledged writes and reads remain available while an SST fsync stalls.
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
static std::atomic<bool> block_sst{false}, entered{false}, release_sst{false};
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd){
 char link[64],name[4096];std::snprintf(link,sizeof(link),"/proc/self/fd/%d",fd);auto n=::readlink(link,name,sizeof(name)-1);
 if(n>0){name[n]='\0';if(block_sst.load() && std::string(name).find(".sst.tmp")!=std::string::npos){entered.store(true);while(!release_sst.load())std::this_thread::sleep_for(1ms);}}
 return __real_fsync(fd);
}
int main(){
 try{
  std::string tmp="/tmp/stonekv-bg-test-XXXXXX";std::vector<char> p(tmp.begin(),tmp.end());p.push_back('\0');char* dir=::mkdtemp(p.data());if(!dir)throw std::runtime_error("mkdtemp");
  { stonekv::Options o;o.memtable_max_bytes=1024;o.compaction_trigger=100;stonekv::DB db(dir,o);
    db.Put("shared","old");
    block_sst=true;
    auto put=std::async(std::launch::async,[&]{db.Put("first",std::string(1500,'x'));});
    auto deadline=std::chrono::steady_clock::now()+5s;while(!entered.load() && std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(1ms);
    bool saw=entered.load(); bool completed=put.wait_for(500ms)==std::future_status::ready;
    bool accessible=false;
    if(completed){put.get();auto read=std::async(std::launch::async,[&]{db.WriteBatch({{stonekv::BatchOperation::Type::Put,"second","ok"},{stonekv::BatchOperation::Type::Put,"shared","new"},{stonekv::BatchOperation::Type::Delete,"gone",""}});const auto range=db.Scan("shared","t",10);return db.Get("first")==std::string(1500,'x') && db.Get("second")=="ok" && db.Scan().size()==3 && range.size()==1 && range[0].first=="shared" && range[0].second=="new";});
      if(read.wait_for(500ms)==std::future_status::ready)accessible=read.get(); else {release_sst=true;read.wait();}
    }
    release_sst=true; if(!completed)put.wait(); block_sst=false;
    if(!saw || !completed || !accessible)throw std::runtime_error("writes/reads blocked on SST fsync");
    db.Flush();
  }
  {stonekv::DB db(dir);if(db.Get("first")!=std::string(1500,'x') || db.Get("second")!="ok" || db.Get("shared")!="new")throw std::runtime_error("reopen mismatch");}
  // When both immutable and active are full, the next writer must wait until
  // the worker makes room; its WAL record must not be published early.
  std::string temp2="/tmp/stonekv-bg-pressure-XXXXXX"; std::vector<char> p2(temp2.begin(),temp2.end());p2.push_back('\0');char* dir2=::mkdtemp(p2.data());if(!dir2)throw std::runtime_error("mkdtemp pressure");
  { stonekv::Options o;o.memtable_max_bytes=1024;o.compaction_trigger=100;stonekv::DB db(dir2,o);
    entered=false;release_sst=false;block_sst=true;
    db.Put("frozen",std::string(1500,'a'));
    auto deadline=std::chrono::steady_clock::now()+5s;while(!entered && std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(1ms);
    if(!entered){release_sst=true;throw std::runtime_error("worker did not enter fsync");}
    const auto outstanding=db.GetStats();
    if(outstanding.background_tasks!=1 || outstanding.background_tasks_peak!=1){release_sst=true;throw std::runtime_error("background task statistics mismatch");}
    db.Put("active",std::string(1500,'b'));
    auto third=std::async(std::launch::async,[&]{db.Put("after","ok");});
    const bool waited=third.wait_for(200ms)==std::future_status::timeout;
    block_sst=false;release_sst=true;third.get();db.Flush();
    if(db.GetStats().background_tasks_peak<1)throw std::runtime_error("background task peak was lost");
    if(!waited || db.Get("frozen")!=std::string(1500,'a') || db.Get("active")!=std::string(1500,'b') || db.Get("after")!="ok")throw std::runtime_error("backpressure failed");
  }
  // Crash after later WAL ACK while an older immutable table has not been
  // published; recovery must replay both records rather than lose the newer one.
  std::string temp3="/tmp/stonekv-bg-crash-XXXXXX"; std::vector<char> p3(temp3.begin(),temp3.end());p3.push_back('\0');char* dir3=::mkdtemp(p3.data());if(!dir3)throw std::runtime_error("mkdtemp crash");
  auto child=::fork();if(child<0)throw std::runtime_error("fork");
  if(child==0){try{stonekv::Options o;o.memtable_max_bytes=1024;o.compaction_trigger=100;stonekv::DB db(dir3,o);entered=false;release_sst=false;block_sst=true;db.Put("frozen",std::string(1500,'a'));auto deadline=std::chrono::steady_clock::now()+5s;while(!entered && std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(1ms);if(!entered)::_exit(11);db.Put("later","durable");::_exit(0);}catch(...){::_exit(12);}}
  int status=0;if(::waitpid(child,&status,0)!=child || !WIFEXITED(status) || WEXITSTATUS(status)!=0)throw std::runtime_error("crash fixture failed");
  {stonekv::DB db(dir3);if(db.Get("frozen")!=std::string(1500,'a') || db.Get("later")!="durable")throw std::runtime_error("crash lost acknowledged writes");}
  // Crash after publishing the older immutable's manifest while the WAL still
  // contains a newer acknowledged active write. Replay must keep that write.
  std::string temp4="/tmp/stonekv-bg-manifest-XXXXXX"; std::vector<char> p4(temp4.begin(),temp4.end());p4.push_back('\0');char* dir4=::mkdtemp(p4.data());if(!dir4)throw std::runtime_error("mkdtemp manifest crash");
  child=::fork();if(child<0)throw std::runtime_error("fork manifest crash");
  if(child==0){try{stonekv::Options o;o.memtable_max_bytes=1024;o.compaction_trigger=100;stonekv::DB db(dir4,o);entered=false;release_sst=false;block_sst=true;db.Put("frozen",std::string(1500,'a'));auto deadline=std::chrono::steady_clock::now()+5s;while(!entered && std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(1ms);if(!entered)::_exit(21);db.Put("later","durable");::setenv("STONEKV_FAILPOINT","after_manifest_rename",1);block_sst=false;release_sst=true;std::this_thread::sleep_for(5s);::_exit(22);}catch(...){::_exit(23);}}
  if(::waitpid(child,&status,0)!=child || !WIFEXITED(status) || WEXITSTATUS(status)!=86)throw std::runtime_error("manifest crash fixture failed");
  {stonekv::DB db(dir4);if(db.Get("frozen")!=std::string(1500,'a') || db.Get("later")!="durable")throw std::runtime_error("manifest crash lost acknowledged writes");}
  std::cout<<"PASS background flush progress and reopen; fixture="<<dir<<'\n';return 0;
 }catch(const std::exception& e){release_sst=true;std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
