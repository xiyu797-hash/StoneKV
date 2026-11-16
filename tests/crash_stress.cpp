// Bounded randomized process-kill test. Each acknowledged mutation has a unique
// key; unacknowledged writes may be present after recovery. Never removes data.
#include "stonekv/db.hpp"
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
namespace fs=std::filesystem;
static void check(bool ok,const char* msg){if(!ok)throw std::runtime_error(msg);}
static std::string key(int round,int i){return "round-"+std::to_string(round)+"-key-"+std::to_string(i);}
int main(int argc,char**argv){try{
 int cycles=argc>1?std::stoi(argv[1]):4; if(argc>3||cycles<1||cycles>10000)throw std::invalid_argument("usage: crash_stress [cycles 1..10000] [new output dir]");
 fs::path root;
 if(argc==3){root=fs::absolute(argv[2]);check(fs::create_directory(root),"output directory exists or cannot be created");}
 else{std::string s="/tmp/stonekv-crash-stress-XXXXXX";std::vector<char> b(s.begin(),s.end());b.push_back('\0');auto p=::mkdtemp(b.data());check(p!=nullptr,"mkdtemp failed");root=p;}
 std::ofstream log(root/"cycles.csv");check(bool(log),"cannot create log");log<<"round,seed,acks,sigkill,recovery_us,verified_keys\n";
 const auto dbpath=(root/"db").string();const std::string value(512,'v');std::vector<std::string> confirmed;std::mt19937 rng(20261115);
 for(int round=0;round<cycles;++round){
  int pipes[2];check(::pipe(pipes)==0,"pipe failed");pid_t pid=::fork();check(pid>=0,"fork failed");
  if(pid==0){::close(pipes[0]);try{stonekv::Options o;o.memtable_max_bytes=4096;o.compaction_trigger=4;o.sync_writes=true;stonekv::DB db(dbpath,o);for(int i=0;i<10000;++i){db.Put(key(round,i),value);std::uint32_t ack=static_cast<std::uint32_t>(i);const char* ptr=reinterpret_cast<const char*>(&ack);std::size_t off=0;while(off<sizeof(ack)){auto n=::write(pipes[1],ptr+off,sizeof(ack)-off);if(n<=0)::_exit(42);off+=static_cast<std::size_t>(n);}}::_exit(43);}catch(...){::_exit(44);}}
  ::close(pipes[1]);const int target=5+static_cast<int>(rng()%26);bool ok=true;int nacks=0;
  for(int i=0;i<target;++i){pollfd pfd{pipes[0],POLLIN,0};if(::poll(&pfd,1,15000)<=0){ok=false;break;}std::uint32_t ack=0;std::size_t off=0;while(off<sizeof(ack)){auto n=::read(pipes[0],reinterpret_cast<char*>(&ack)+off,sizeof(ack)-off);if(n<0&&errno==EINTR)continue;if(n<=0){ok=false;break;}off+=static_cast<std::size_t>(n);}if(!ok||ack!=static_cast<std::uint32_t>(i)){ok=false;break;}confirmed.push_back(key(round,i));++nacks;}
  int killed=::kill(pid,SIGKILL);int status=0;auto waited=::waitpid(pid,&status,0);::close(pipes[0]);check(ok&&killed==0&&waited==pid&&WIFSIGNALED(status)&&WTERMSIG(status)==SIGKILL,"writer failed or kill not observed");
  auto start=std::chrono::steady_clock::now();{stonekv::Options o;o.memtable_max_bytes=4096;o.compaction_trigger=4;stonekv::DB db(dbpath,o);for(const auto& k:confirmed)check(db.Get(k)==value,"acknowledged key missing or changed");if(round%20==19){db.Flush();db.Compact();}}
  const auto us=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count();log<<round<<",20261115,"<<nacks<<",1,"<<us<<','<<confirmed.size()<<'\n';log.flush();check(bool(log),"log write failure");if(round%10==0)std::cout<<"PASS round="<<round<<" acknowledged="<<confirmed.size()<<std::endl;
 }
 std::cout<<"PASS cycles="<<cycles<<" acknowledged="<<confirmed.size()<<" output="<<root<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
