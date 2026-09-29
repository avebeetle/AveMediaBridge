#pragma once
#include "AveMediaBridge/AveMediaBridgeContainerApi.h"
#include <array>
#include <cstdint>
#include <functional>
#include <string>

namespace AveMediaBridge::Probe::Container {
constexpr uint32_t tag(char a,char b,char c,char d) {
    return (uint32_t(uint8_t(a))<<24)|(uint32_t(uint8_t(b))<<16)|(uint32_t(uint8_t(c))<<8)|uint8_t(d);
}
constexpr uint32_t tag(const char (&s)[5]) { return tag(s[0],s[1],s[2],s[3]); }
struct Unknown { const char* reason; };
struct Fault { int status; };
void require(bool value,const char* reason="malformedStructure");
struct Box { uint64_t data=0,end=0;uint32_t type=0;uint64_t size()const{return end-data;} };
class Reader {
public:
    explicit Reader(const AMBI_SourceV1& source):source_(source){}
    uint64_t size()const{return source_.byteSize;}
    void charge(uint64_t units=1);
    uint64_t integer(uint64_t offset,uint32_t length);
    void walk(uint64_t begin,uint64_t end,uint32_t depth,const std::function<void(const Box&)>& visit);
private:
    AMBI_SourceV1 source_;
    // No complete table or moov allocation. One 64KiB cache, even for >4GiB files.
    std::array<uint8_t,65536> cache_{};
    uint64_t cacheOffset_=0,work_=0,boxes_=0;
    uint32_t cached_=0;
};
struct Proof {
    bool external=false;
    std::string unknown,limitReason;
    void reject(const char* reason);
};
std::string classify(const AMBI_SourceV1& source);
}
