#include "AveMediaBridge/AveMediaBridgeContainerApi.h"
#include "Input/StableInputContract.hpp"
#include "Probe/ContainerDependency.hpp"
#include <cstring>
extern "C" AMBR_API int __cdecl AveMediaBridge_ClassifyContainerV1(
    const AMBI_SourceV1* source, char* utf8, uint32_t capacity, uint32_t* requiredBytes) {
    if(requiredBytes)*requiredBytes=0;
    if(!requiredBytes||(!utf8&&capacity)||AveMediaBridge::Input::validateSource(source)!=AMBI_OK)return AMBR_INVALID_ARGUMENT;
    try {
        const AMBI_SourceV1 copied=*source;
        const auto json=AveMediaBridge::Probe::Container::classify(copied);
        if(json.size()+1>AMBC_MAX_JSON_BYTES)return AMBR_INTERNAL_ERROR;
        *requiredBytes=static_cast<uint32_t>(json.size()+1);
        if(!utf8||capacity<*requiredBytes)return AMBC_BUFFER_TOO_SMALL;
        std::memcpy(utf8,json.c_str(),*requiredBytes);return AMBR_OK;
    }catch(const AveMediaBridge::Probe::Container::Fault& fault){return fault.status;}
    catch(...){return AMBR_INTERNAL_ERROR;}
}
