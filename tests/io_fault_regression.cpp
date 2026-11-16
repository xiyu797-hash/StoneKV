// Linux fault regression, link with -Wl,--wrap=write -Wl,--wrap=fsync.
// The wrapper injects a single error into the selected DB file only. All state
// is observed through the public DB API and each run keeps its own fixtures.
#include "stonekv/db.hpp"
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>
#include <sys/stat.h>
#include <cstdlib>
#include <cstdio>

namespace fs = std::filesystem;
namespace {
struct Injection { bool active=false, seen=false; std::string path_part; int error=0; bool on_sync=false; bool partial=false, waiting=false, dir_only=false; int skip=0; } inj;
bool trace_dirs = false;
std::vector<std::string> synced_dirs;
std::string fdPath(int fd) {
    char link[64], path[4096];
    std::snprintf(link,sizeof(link),"/proc/self/fd/%d",fd);
    const auto n=::readlink(link,path,sizeof(path)-1);
    if(n<0)return {};
    path[n]='\0'; return path;
}
bool targets(int fd) {
    struct stat st{};
    return fdPath(fd).find(inj.path_part)!=std::string::npos &&
           (!inj.dir_only || (::fstat(fd,&st)==0 && S_ISDIR(st.st_mode)));
}
void check(bool b,const std::string& message) { if(!b)throw std::runtime_error(message); }
template<class F> void must_throw(F f,const std::string& what) {
    bool threw=false; try { f(); } catch(const std::exception&) { threw=true; }
    check(threw,what+" did not throw");
}
struct Case { const char* name; const char* target; int code; bool sync; bool batch; bool partial=false; bool dir_only=false; int skip=0; bool compact=false; };
}
extern "C" ssize_t __real_write(int,const void*,size_t);
extern "C" int __real_fsync(int);
extern "C" ssize_t __wrap_write(int fd,const void* data,size_t size) {
    if(inj.active && !inj.seen && !inj.on_sync && targets(fd)) {
        if(inj.partial && !inj.waiting && size>1) { inj.waiting=true; return __real_write(fd,data,1); }
        inj.seen=true; errno=inj.error; return -1;
    }
    return __real_write(fd,data,size);
}
extern "C" int __wrap_fsync(int fd) {
    struct stat st{};
    if(trace_dirs && ::fstat(fd,&st)==0 && S_ISDIR(st.st_mode)) synced_dirs.push_back(fdPath(fd));
    if(inj.active && !inj.seen && inj.on_sync && targets(fd)) {
        if(inj.skip>0) --inj.skip; else { inj.seen=true; errno=inj.error; return -1; }
    }
    return __real_fsync(fd);
}
int main(int argc,char** argv) {
 try {
    if(argc>3)throw std::runtime_error("usage: io_fault_regression [NEW_EMPTY_OUTPUT_DIRECTORY [RANDOM_ROUNDS]]");
    const int random_rounds=argc==3?std::stoi(argv[2]):20;
    check(random_rounds>=0 && random_rounds<=10000,"random rounds must be 0..10000");
    fs::path base;
    if(argc>=2) {
      base=fs::absolute(argv[1]);
      check(fs::create_directory(base),"output directory already exists or could not be created");
    } else {
      std::string pattern=(fs::temp_directory_path()/"stonekv-io-fault-XXXXXX").string();
      std::vector<char> buffer(pattern.begin(),pattern.end()); buffer.push_back('\0');
      const char* created=::mkdtemp(buffer.data());
      check(created!=nullptr,"cannot create unique fixture directory"); base=created;
    }
    {
      const auto root=base/"directory_order";
      const auto nested=root/"nested"/"database";
      synced_dirs.clear(); trace_dirs=true;
      { stonekv::DB db(nested.string()); db.Put("ack","value"); }
      trace_dirs=false;
      check(synced_dirs.size()>=3,"missing ancestor fsync calls");
      check(synced_dirs[0]==base.string() && synced_dirs[1]==root.string() && synced_dirs[2]==(root/"nested").string(),"new directory entries not synced parent-first");
      { stonekv::DB reopened(nested.string()); check(reopened.Get("ack")=="value","nested database reopen"); }
      std::cout<<"PASS nested directory parent fsync order\n";
    }
    {
      const auto parent=base/"directory_fsync_failure";
      synced_dirs.clear(); trace_dirs=true;
      inj={true,false,base.string(),EIO,true,false,false,true,0};
      must_throw([&]{stonekv::DB db((parent/"nested"/"database").string());},"ancestor directory fsync failure");
      inj.active=false; trace_dirs=false;
      check(inj.seen,"ancestor fsync injection not reached");
      check(!synced_dirs.empty() && synced_dirs.front()==base.string(),"error must be on first newly-created directory parent");
      check(!fs::exists(parent/"nested"),"failure before descendant creation");
      std::cout<<"PASS ancestor directory fsync EIO propagated\n";
    }
    const std::vector<Case> cases={
      {"wal_write_enospc","active.wal",ENOSPC,false,false},
      {"wal_write_eio","active.wal",EIO,false,false},
      {"wal_partial_write_eio","active.wal",EIO,false,false,true},
      {"wal_fsync_eio","active.wal",EIO,true,false},
      {"batch_write_enospc","active.wal",ENOSPC,false,true},
      {"batch_partial_write_eio","active.wal",EIO,false,true,true},
      {"batch_fsync_eio","active.wal",EIO,true,true},
      {"sst_write_enospc",".sst.tmp",ENOSPC,false,false},
      {"sst_write_eio",".sst.tmp",EIO,false,false},
      {"sst_fsync_eio",".sst.tmp",EIO,true,false},
      {"manifest_write_enospc","MANIFEST.tmp",ENOSPC,false,false},
      {"manifest_write_eio","MANIFEST.tmp",EIO,false,false},
      {"manifest_fsync_eio","MANIFEST.tmp",EIO,true,false},
      {"sst_dir_fsync_eio","<dir>",EIO,true,false,false,true,0},
      {"manifest_dir_fsync_eio","<dir>",EIO,true,false,false,true,1},
      {"compact_sst_enospc",".sst.tmp",ENOSPC,false,false,false,false,0,true},
      {"compact_sst_fsync_eio",".sst.tmp",EIO,true,false,false,false,0,true},
      {"compact_manifest_eio","MANIFEST.tmp",EIO,false,false,false,false,0,true},
      {"compact_manifest_dir_fsync_eio","<dir>",EIO,true,false,false,true,1,true},
    };
    for(const auto& c:cases) {
      const auto path=base/c.name;
      {
        stonekv::Options opt; opt.memtable_max_bytes=1024*1024; opt.compaction_trigger=100; opt.sync_writes=true;
        stonekv::DB db(path.string(),opt);
        db.Put("acknowledged","preserved");
        if(c.compact) { db.Put("version","old"); db.Flush(); db.Put("version","new"); db.Delete("removed"); db.Flush(); }
        inj={true,false,c.dir_only?path.string():c.target,c.code,c.sync,c.partial,false,c.dir_only,c.skip};
        const std::string name(c.name);
        if(c.compact) {
          must_throw([&]{db.Compact();},name);
        } else if(name.rfind("sst_",0)==0 || name.rfind("manifest_",0)==0) {
          must_throw([&]{db.Flush();},name);
        } else if(c.batch) {
          must_throw([&]{db.WriteBatch({{stonekv::BatchOperation::Type::Put,"batch_a","a"},{stonekv::BatchOperation::Type::Put,"batch_b","b"}});},name);
        } else {
          must_throw([&]{db.Put("uncertain","value");},name);
        }
        inj.active=false;
        check(inj.seen,name+" injection was not reached");
        must_throw([&]{(void)db.Get("acknowledged");},name+" poisoned read");
        must_throw([&]{db.Put("later","must_not_commit");},name+" poisoned write");
        must_throw([&]{db.Flush();},name+" poisoned flush");
      }
      {
        stonekv::DB reopened(path.string());
        check(reopened.Get("acknowledged")=="preserved",std::string(c.name)+" lost acknowledged data");
        check(!reopened.Get("later"),std::string(c.name)+" accepted post-failure write");
        if(c.compact) { check(reopened.Get("version")=="new",std::string(c.name)+" lost newest version"); check(!reopened.Get("removed"),std::string(c.name)+" resurrected deletion"); }
        if(c.batch){ const auto a=reopened.Get("batch_a"),b=reopened.Get("batch_b"); check(bool(a)==bool(b),std::string(c.name)+" partial batch"); }
        // The failed operation has an uncertain outcome after I/O/fsync failure.
        std::cout<<"PASS "<<c.name<<" reopened; uncertain="<<(reopened.Get("uncertain")?"present":"absent")<<'\n';
      }
    }
    // Vary both the acknowledged reference-model history and the injected
    // syscall/file/error. A failed mutation may appear after recovery, but a
    // failed batch must remain all-or-nothing.
    std::mt19937 rng(20261116);
    std::ofstream random_log(base/"random_faults.csv");
    check(bool(random_log),"cannot open random fault log");
    random_log<<"round,seed,scenario,error,partial,acknowledged_ops,reference_keys,uncertain_present\n";
    for(int round=0;round<random_rounds;++round) {
      const auto path=base/("random_"+std::to_string(round));
      const unsigned scenario=rng()%8U;
      const int code=(rng()%2U)?EIO:ENOSPC;
      const bool is_batch=scenario==2 || scenario==3;
      const bool is_wal=scenario<4;
      const bool is_sync=(scenario%2U)==1U;
      const bool partial=!is_sync && (rng()%2U)!=0;
      const char* target=is_wal?"active.wal":(scenario<6?".sst.tmp":"MANIFEST.tmp");
      std::map<std::string,std::string> model;
      const int operations=10+static_cast<int>(rng()%21U);
      {
        stonekv::Options opt; opt.memtable_max_bytes=1024*1024; opt.compaction_trigger=100; opt.sync_writes=true;
        stonekv::DB db(path.string(),opt);
        for(int i=0;i<operations;++i) {
          const auto k="k"+std::to_string(rng()%25U);
          if(rng()%4U==0) { db.Delete(k); model.erase(k); }
          else { const auto v="v"+std::to_string(rng()); db.Put(k,v); model[k]=v; }
        }
        // Ensure flush always has data, including when all random operations deleted.
        db.Put("model_anchor","committed"); model["model_anchor"]="committed";
        inj={true,false,target,code,is_sync,partial,false,false,0};
        if(!is_wal) must_throw([&]{db.Flush();},"random flush fault");
        else if(is_batch) must_throw([&]{db.WriteBatch({{stonekv::BatchOperation::Type::Put,"uncertain_a","a"},{stonekv::BatchOperation::Type::Put,"uncertain_b","b"}});},"random batch fault");
        else must_throw([&]{db.Put("uncertain_a","a");},"random WAL fault");
        inj.active=false;
        check(inj.seen,"random injection was not reached");
        must_throw([&]{db.Put("post_failure","bad");},"random poisoned write");
      }
      stonekv::DB recovered(path.string());
      const auto a=recovered.Get("uncertain_a"), b=recovered.Get("uncertain_b");
      if(is_batch) check(bool(a)==bool(b),"random batch partially recovered");
      if(a) model["uncertain_a"]="a";
      if(b) model["uncertain_b"]="b";
      const std::vector<std::pair<std::string,std::string>> expected(model.begin(),model.end());
      check(recovered.Scan("","",1000)==expected,"random recovery differs from acknowledged reference model");
      random_log<<round<<",20261116,"<<scenario<<','<<code<<','<<partial<<','<<operations+1<<','<<model.size()<<','<<bool(a)<<'\n';
      random_log.flush(); check(bool(random_log),"random fault log write failed");
    }
    std::cout<<"PASS all "<<cases.size()<<" I/O fault cases and "<<random_rounds<<" random reference-model rounds; fixtures="<<base<<'\n';
    return 0;
 } catch(const std::exception& e) { inj.active=false; std::cerr<<"FAIL "<<e.what()<<'\n'; return 1; }
}
