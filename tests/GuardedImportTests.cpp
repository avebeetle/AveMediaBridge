#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "AveMediaBridge/AveMediaBridgeApi.hpp"
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace {
constexpr std::uint32_t frames = 96000;
constexpr float reference = 0.125f;
void check(bool condition, const char* why) { if (!condition) throw std::runtime_error(why); }
void fixture(const fs::path& path) {
    std::ofstream wav(path,std::ios::binary);
    auto integer=[&](std::uint32_t value,int bytes) {
        for(int i=0;i<bytes;++i) wav.put(static_cast<char>(value >> (8*i)));
    };
    wav.write("RIFF",4); integer(36+2*frames,4); wav.write("WAVEfmt ",8);
    integer(16,4); integer(1,2); integer(1,2); integer(48000,4); integer(96000,4);
    integer(2,2); integer(16,2); wav.write("data",4); integer(2*frames,4);
    for (std::uint32_t i=0;i<frames;++i) integer(4096,2);
    wav.close(); check(bool(wav),"fixture");
}
bool canWrite(const fs::path& path) {
    HANDLE h=CreateFileW(path.c_str(), FILE_WRITE_DATA, FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h==INVALID_HANDLE_VALUE) {
        const auto error=GetLastError();
        check(error==ERROR_ACCESS_DENIED || error==ERROR_SHARING_VIOLATION,"unexpected write probe error");
        return false;
    }
    CloseHandle(h); return true;
}
bool identity(std::array<unsigned long,3>& out) {
    return AveMediaBridge_GuardedPcmFileIdV1(&out[0],&out[1],&out[2])==1;
}
struct Run {
    fs::path dir;
    bool guarded, expectsProof, cancel, observed=false, failed=false;
    Run* nested=nullptr;
    fs::path source;
    int import();
};
void AVEMEDIABRIDGE_CALL observe(const AveMediaBridgeImportProgress* p, void* user) {
    auto& r=*static_cast<Run*>(user);
    if (r.observed || !p || !p->framesWritten || p->framesWritten>=frames) return;
    r.observed=true;
    try {
        std::array<unsigned long,3> first{}, after{};
        check(identity(first)==r.expectsProof,"callback identity");
        check(canWrite(r.dir/"original_f32.bin")!=r.expectsProof,"writer sharing while decoding");
        if (r.nested) {
            check(r.nested->import()==0 && r.nested->observed && !r.nested->failed, "nested legacy import");
            check(identity(after)==r.expectsProof && after==first,"nested import changed outer identity");
        }
    } catch(...) { r.failed=true; }
}
int AVEMEDIABRIDGE_CALL stop(void* user) {
    const auto& r=*static_cast<Run*>(user); return r.failed || (r.cancel && r.observed);
}
int Run::import() {
    auto in=source.wstring(), out=dir.wstring(); fs::create_directories(dir);
    AveMediaBridgeImportOptions o{}; o.structSize=sizeof(o); o.inputPath=in.c_str(); o.sessionMediaDir=out.c_str();
    o.onProgress=observe; o.shouldCancel=stop; o.userData=this;
    return guarded ? AveMediaBridge_ImportAudioToSessionGuardedV1(&o) : AveMediaBridge_ImportAudioToSessionEx(&o);
}
}
int main() try {
    const auto root=fs::temp_directory_path() / (L"GuardedBridge-"+std::to_wstring(GetCurrentProcessId())+
        L"-"+std::to_wstring(GetTickCount64()));
    check(fs::create_directory(root),"fresh root");
    struct Cleanup { fs::path root; ~Cleanup(){std::error_code ec; fs::remove_all(root,ec);} } cleanup{root};
    const auto src=root/"test.wav"; fixture(src);
    wchar_t volume[32768]{}, fsName[32]{}; DWORD flags=0;
    const bool isNtfs=GetVolumePathNameW(root.c_str(),volume,32768) && GetDriveTypeW(volume)!=DRIVE_REMOTE &&
        GetVolumeInformationW(volume,nullptr,0,nullptr,nullptr,&flags,fsName,32) && (flags&FILE_PERSISTENT_ACLS) &&
        _wcsicmp(fsName,L"NTFS")==0;
    Run legacy{root/"legacy",false,false,false}; legacy.source=src;
    Run normal{root/"guarded",true,isNtfs,false}; normal.nested=&legacy; normal.source=src;
    check(normal.import()==0 && normal.observed && !normal.failed,"protected import");
    std::array<unsigned long,3> id{}; check(!identity(id),"identity leaked after import");
    const auto pcm=normal.dir/"original_f32.bin";
    check(canWrite(pcm)!=isNtfs,"writer-close handoff");
    std::ifstream in(pcm,std::ios::binary); std::vector<float> values(frames);
    in.read(reinterpret_cast<char*>(values.data()),frames*sizeof(float));
    check(in.gcount()==frames*sizeof(float) && in.peek()==EOF,"PCM extent");
    for (auto value:values) check(value==reference,"PCM bytes");
    in.close();
    check(normal.import()!=0 && fs::file_size(pcm)==frames*sizeof(float),"existing guarded output retained");
    check(legacy.import()==0 && canWrite(legacy.dir/"original_f32.bin"),"legacy overwrite and writable output");
    Run cancel{root/"cancel",true,isNtfs,true}; cancel.source=src;
    check(cancel.import()==4 && !cancel.failed && cancel.observed && fs::is_empty(cancel.dir),"cancel cleanup");
    std::cout<<"PASS guarded and legacy writer contract\n";
    return 0;
} catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
