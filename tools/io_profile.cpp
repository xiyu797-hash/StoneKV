// Separate instrumented workload: counts StoneKV's direct I/O syscalls, not
// storage-device traffic. Instrumentation overhead invalidates timing comparisons.
#define main stonekv_workload_main
#include "benchmark_workloads.cpp"
#undef main
#include <unistd.h>

static std::atomic<std::uint64_t> read_bytes{0}, write_bytes{0};
extern "C" ssize_t __real_pread(int, void*, size_t, off_t);
extern "C" ssize_t __real_write(int, const void*, size_t);
extern "C" ssize_t __real_pwrite(int, const void*, size_t, off_t);
extern "C" ssize_t __wrap_pread(int fd, void* p, size_t n, off_t offset) {
    const auto result=__real_pread(fd,p,n,offset);
    if(result>0) read_bytes.fetch_add(static_cast<std::uint64_t>(result),std::memory_order_relaxed);
    return result;
}
extern "C" ssize_t __wrap_write(int fd, const void* p, size_t n) {
    const auto result=__real_write(fd,p,n);
    if(result>0) write_bytes.fetch_add(static_cast<std::uint64_t>(result),std::memory_order_relaxed);
    return result;
}
extern "C" ssize_t __wrap_pwrite(int fd, const void* p, size_t n, off_t offset) {
    const auto result=__real_pwrite(fd,p,n,offset);
    if(result>0) write_bytes.fetch_add(static_cast<std::uint64_t>(result),std::memory_order_relaxed);
    return result;
}
int main(int argc,char** argv) {
    const int result=stonekv_workload_main(argc,argv);
    if(result!=0) return result;
    try {
        const auto count=std::stoull(argv[2]);
        const auto seed=std::stoull(argv[4]);
        std::vector<std::size_t> order(count);
        std::iota(order.begin(),order.end(),0);
        std::mt19937_64 gen(seed);
        std::shuffle(order.begin(),order.end(),gen);
        std::uint64_t returned_records=count+count/2+2; // random Get, concurrent Get, successful verification Get
        for(std::size_t j=0;j<std::min<std::size_t>(count,100);++j)
            returned_records+=std::min<std::size_t>(16,count-order[j]);
        constexpr std::uint64_t key_bytes=13, value_bytes=128;
        const std::uint64_t logical_read=returned_records*(key_bytes+value_bytes);
        const std::uint64_t logical_write=(3*count+(count+1)/2)*(key_bytes+value_bytes)+(count/4)*key_bytes;
        std::ofstream out(fs::path(argv[1])/"io_profile.json");
        if(!out) throw std::runtime_error("cannot create io_profile.json");
        out<<"{\"syscall_pread_bytes\":"<<read_bytes.load()
           <<",\"syscall_write_pwrite_bytes\":"<<write_bytes.load()
           <<",\"logical_returned_key_value_bytes\":"<<logical_read
           <<",\"logical_mutation_key_value_bytes\":"<<logical_write<<"}\n";
        out.flush();if(!out)throw std::runtime_error("cannot write io_profile.json");
        return 0;
    } catch(const std::exception& e) { std::cerr<<"ERROR: "<<e.what()<<'\n'; return 1; }
}
