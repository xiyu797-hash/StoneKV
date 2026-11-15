#include "stonekv/db.hpp"
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
static std::atomic<unsigned> sst_opens{0};
static std::size_t openSsts() {
 std::size_t count=0;
 for(const auto& e:std::filesystem::directory_iterator("/proc/self/fd")) {
  char path[4096];auto n=::readlink(e.path().c_str(),path,sizeof(path)-1);
  if(n>0){path[n]='\0';std::string s(path);if(s.size()>=4 && s.substr(s.size()-4)==".sst")++count;}
 }
 return count;
}
extern "C" int __real_open(const char*,int,...);
extern "C" int __wrap_open(const char* path,int flags,...) {
 mode_t mode=0;if(flags&O_CREAT){va_list a;va_start(a,flags);mode=static_cast<mode_t>(va_arg(a,int));va_end(a);}
 const auto fd=(flags&O_CREAT)?__real_open(path,flags,mode):__real_open(path,flags);
 if(fd>=0 && std::string(path).find(".sst")!=std::string::npos && std::string(path).find(".tmp")==std::string::npos)++sst_opens;
 return fd;
}
int main(){try{
 std::string temp="/tmp/stonekv-fd-cache-XXXXXX";std::vector<char> p(temp.begin(),temp.end());p.push_back('\0');auto dir=::mkdtemp(p.data());if(!dir)throw std::runtime_error("mkdtemp");
 {stonekv::Options o;o.compaction_trigger=100;stonekv::DB db(dir,o);db.Put("key","value");db.Flush();auto before=sst_opens.load();
  for(int i=0;i<20;++i)if(db.Get("key")!="value")throw std::runtime_error("get mismatch");
  auto opened=sst_opens.load()-before;if(opened>1)throw std::runtime_error("repeated Get reopens SST "+std::to_string(opened)+" times");
  db.Put("key","new");db.Flush();db.Compact();if(db.Get("key")!="new")throw std::runtime_error("compaction mismatch");}
 {stonekv::Options o;o.compaction_trigger=100;stonekv::DB db(dir,o);if(db.Get("key")!="new")throw std::runtime_error("reopen mismatch");
  for(int i=0;i<40;++i){db.Put("k"+std::to_string(i),"v");db.Flush();}
  for(int i=0;i<40;++i)if(db.Get("k"+std::to_string(i))!="v")throw std::runtime_error("eviction read mismatch");
  if(openSsts()>32)throw std::runtime_error("SST descriptor cache exceeds bound");
  if(db.Get("k0")!="v")throw std::runtime_error("evicted descriptor could not reopen");
 }
 if(openSsts()!=0)throw std::runtime_error("SST descriptor leaked after DB close");
 std::cout<<"PASS SST handles reused and compacted data survives reopen fixture="<<dir<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
