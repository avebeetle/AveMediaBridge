#include "Probe/ContainerDependency.hpp"
#include "Probe/MovSingleFilePolicy.hpp"
#include "Input/StableInputContract.hpp"
#include <algorithm>

namespace AveMediaBridge::Probe::Container {
void require(bool value,const char* reason) { if(!value)throw Unknown{reason}; }
void Proof::reject(const char* reason) {
    const std::string value(reason);
    if(value=="workLimit"||value=="boxLimit"||value=="depthLimit"||value=="listLimit")limitReason=value;
    if(unknown.empty())unknown=value;
}
void Reader::charge(uint64_t units) {
    // Deterministic aggregate cap: each box, scalar/config read request,
    // declared table entry and derived sample/chunk costs one unit. Cached and
    // zero-length-table work still polls cancellation. Exhaustion never proves
    // independence. The cap intentionally excludes complex valid containers.
    AMBI_Status rc=AMBI_IO_ERROR;
    try { rc=source_.checkCancel(source_.user); }
    catch(...) { throw Fault{AMBR_INPUT_FAILED}; }
    if(rc==AMBI_CANCELED)throw Fault{AMBR_CANCELED};
    if(rc!=AMBI_OK)throw Fault{AMBR_INPUT_FAILED};
    require(units<=4000000-work_,"workLimit");work_+=units;
}
uint64_t Reader::integer(uint64_t offset,uint32_t length) {
    charge();require(length<=8 && offset<=size() && length<=size()-offset);
    uint64_t result=0;
    for(uint32_t i=0;i<length;++i) {
        const auto pos=offset+i;
        if(pos<cacheOffset_||pos-cacheOffset_>=cached_) {
            cacheOffset_=pos;
            const auto count=static_cast<uint32_t>(std::min<uint64_t>(cache_.size(),size()-pos));
            const auto rc=Input::readSource(source_,pos,cache_.data(),count,cached_);
            if(rc==AMBI_CANCELED)throw Fault{AMBR_CANCELED};
            if(rc!=AMBI_OK||cached_!=count)throw Fault{AMBR_INPUT_FAILED};
        }
        result=(result<<8)|cache_[static_cast<size_t>(pos-cacheOffset_)];
    }
    return result;
}
void Reader::walk(uint64_t begin,uint64_t end,uint32_t depth,const std::function<void(const Box&)>& visit) {
    require(depth<=32,"depthLimit");require(begin<=end&&end<=size());
    charge(0);
    while(begin<end) {
        require(boxes_<1000000,"boxLimit");++boxes_;charge();require(end-begin>=8);
        auto length=integer(begin,4);auto type=static_cast<uint32_t>(integer(begin+4,4));uint64_t header=8;
        if(length==1){require(end-begin>=16);length=integer(begin+8,8);header=16;}
        else if(length==0){require(depth==1,"nestedZeroSize");length=end-begin;}
        require(length>=header&&length<=end-begin);
        visit({begin+header,begin+length,type});begin+=length;
    }
}
std::string classify(const AMBI_SourceV1& source) {
    Reader reader(source);Proof proof;
    try { movSingleFile(reader,proof); } catch(const Unknown& x){proof.reject(x.reason);}
    // Cancellation/faults throw beyond policy results and cannot become Unknown.
    reader.charge(0);
    // A cap always reports Unknown, even after known external evidence, because
    // the policy explicitly refuses any completed classification on truncation.
    const char* value=!proof.limitReason.empty()?"unknown":proof.external?"externalDependencies":proof.unknown.empty()?"singleFile":"unknown";
    const std::string reason=!proof.limitReason.empty()?proof.limitReason:proof.external?"externalDataReference":proof.unknown;
    constexpr char hex[]="0123456789abcdef";std::string token;
    for(auto b:source.sourceToken){token+=hex[b>>4];token+=hex[b&15];}
    return "{\"version\":1,\"sourceToken\":\""+token+"\",\"byteSize\":\""+std::to_string(source.byteSize)+
        "\",\"classification\":\""+value+"\",\"policy\":\"mov-single-file\",\"policyVersion\":1,\"reason\":\""+reason+"\"}";
}
}
