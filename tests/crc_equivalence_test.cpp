// Include the implementation to test its private checksum primitive against an
// independent bitwise IEEE CRC-32, including unaligned and streamed inputs.
#include "../src/db.cpp"
#include <iostream>
#include <random>
int main() {
    std::mt19937 gen(20261116);
    std::vector<unsigned char> bytes(4100);
    for (auto& byte : bytes) byte=static_cast<unsigned char>(gen());
    for (std::size_t off=0; off<4; ++off) for (std::size_t n=0; n<=4096; ++n) {
        std::uint32_t expected=0xffffffffU;
        for(std::size_t i=0;i<n;++i) {
            expected ^= bytes[off+i];
            for(int bit=0;bit<8;++bit) expected=(expected>>1U)^((expected&1U)?0xedb88320U:0U);
        }
        auto actual=stonekv::crcUpdate(0xffffffffU,bytes.data()+off,n/2);
        actual=stonekv::crcUpdate(actual,bytes.data()+off+n/2,n-n/2);
        if(actual!=expected) { std::cerr<<"CRC mismatch offset="<<off<<" length="<<n<<'\n'; return 1; }
    }
    std::cout<<"PASS bitwise CRC equivalence\n";
}
