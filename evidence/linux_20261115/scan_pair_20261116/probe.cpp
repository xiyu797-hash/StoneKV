#include "stonekv/db.hpp"
#include <chrono>
#include <cstdio>
#include <iostream>
#include <string>
int main(int argc,char**argv){if(argc!=2)return 2;stonekv::Options o;o.sync_writes=false;o.memtable_max_bytes=128*1024;o.compaction_trigger=1000;stonekv::DB db(argv[1],o);char key[32];for(int i=0;i<200;++i){std::snprintf(key,sizeof(key),"s%012d",i);db.Put(key,std::string(128,'x'));}db.Flush();for(int i=0;i<200;++i){std::snprintf(key,sizeof(key),"r%012d",i);db.Put(key,std::string(128,'x'));}auto start=std::chrono::steady_clock::now();for(int j=0;j<5000;++j){std::snprintf(key,sizeof(key),"s%012d",j%200);auto rows=db.Scan(key,"",16);if(rows.empty()||rows[0].first!=key)return 1;}std::cout<<"scan_5000_ms="<<std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count()<<'\n';}
