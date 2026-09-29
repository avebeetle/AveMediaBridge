#include "AveMediaBridge/AveMediaBridgeContainerApi.h"
#include "Probe/ContainerDependency.hpp"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <bcrypt.h>
extern "C" int container_abi_c(void);
static_assert(std::is_same_v<decltype(&AveMediaBridge_ClassifyContainerV1),
    int(__cdecl*)(const AMBI_SourceV1*, char*, uint32_t, uint32_t*)>);
using Bytes = std::vector<unsigned char>;
static int checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::cerr << __LINE__ << ": " #x "\n"; } } while(0)
static std::string sha256(const Bytes& bytes) {
    BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;unsigned char digest[32]{};
    if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)throw std::runtime_error("hash algorithm");
    const auto created=BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0);
    const auto fed=created<0?created:BCryptHashData(hash,const_cast<PUCHAR>(bytes.data()),static_cast<ULONG>(bytes.size()),0);
    const auto finished=fed<0?fed:BCryptFinishHash(hash,digest,32,0);
    if(hash)BCryptDestroyHash(hash);BCryptCloseAlgorithmProvider(algorithm,0);
    if(finished<0)throw std::runtime_error("hash fixture");
    constexpr char digits[]="0123456789abcdef";std::string result;for(auto byte:digest){result+=digits[byte>>4];result+=digits[byte&15];}return result;
}
static void put(Bytes& b, uint32_t v) { for (int n=24;n>=0;n-=8) b.push_back(static_cast<unsigned char>(v>>n)); }
static void patch(Bytes& b, size_t at, uint32_t v) { for(int n=3;n>=0;--n) {b[at+static_cast<size_t>(n)]=static_cast<unsigned char>(v);v>>=8;} }
static Bytes cat(std::initializer_list<Bytes> list) { Bytes b;for(const auto& x:list)b.insert(b.end(),x.begin(),x.end());return b; }
static Bytes box(const char* type, Bytes data={}) { Bytes b;put(b,static_cast<uint32_t>(data.size()+8));b.insert(b.end(),type,type+4);b.insert(b.end(),data.begin(),data.end());return b; }
static Bytes words(std::initializer_list<uint32_t> ws) { Bytes b;for(auto w:ws)put(b,w);return b; }
// One sample, one chunk, explicitly hand-derived local mdat offset 24:
// ftyp=16 bytes, mdat header=8 bytes.
static Bytes track(const char* reference="url ", uint32_t flags=1, uint16_t index=1, uint32_t offset=24, uint32_t size=4,Bytes extra={},Bytes extraRef={}) {
    Bytes audio(28);audio[7]=static_cast<unsigned char>(index);audio[6]=static_cast<unsigned char>(index>>8);
    // ISO ES descriptor: ES_ID=1, flags=0; AAC decoder config; ASC; predefined SL=2.
    Bytes config{0x40,0x15};config.resize(13);config=cat({config,Bytes{5,2,0x11,0x90}});
    const Bytes es=cat({Bytes{3,25,0,1,0,4,17},config,Bytes{6,1,2}});
    const auto entry=box("mp4a",cat({audio,box("esds",cat({words({0}),es}))}));
    const auto tables=cat({box("stsd",cat({words({0,1}),entry})),box("stts",words({0,1,1,1024})),
        box("stsc",words({0,1,1,1,1})),box("stsz",words({0,0,1,size})),box("stco",words({0,1,offset})),extra});
    Bytes handler(24);std::memcpy(handler.data()+8,"soun",4);
    return box("trak",cat({box("tkhd",Bytes(84)),box("mdia",cat({box("mdhd",Bytes(24)),box("hdlr",handler),
        box("minf",cat({box("smhd",Bytes(8)),box("dinf",box("dref",cat({words({0,extraRef.empty()?1u:2u}),box(reference,words({flags})),extraRef}))),box("stbl",tables)}))}))}));
}
static Bytes movie(Bytes tracks=track(), Bytes extra={}) {
    return cat({box("ftyp",cat({Bytes{'i','s','o','m'},words({0})})),box("mdat",Bytes(4)),
        box("moov",cat({box("mvhd",Bytes(100)),tracks,extra}))});
}
struct Source {
    Bytes bytes; AMBI_Status fault=AMBI_OK; bool canceled=false; uint32_t maxRead=0; bool throwCancel=false;
    AMBI_SourceV1 descriptor() { AMBI_SourceV1 s{};s.structSize=sizeof(s);s.abiVersion=1;s.byteSize=bytes.size();s.sourceToken[0]=0x42;s.user=this;
        s.checkCancel=[](void* p)->AMBI_Status{auto& s=*static_cast<Source*>(p);if(s.throwCancel)throw std::runtime_error("callback");return s.canceled?AMBI_CANCELED:AMBI_OK;};
        s.readAt=[](void* p,uint64_t o,void* d,uint32_t n,uint32_t* got)->AMBI_Status {
            auto& s=*static_cast<Source*>(p);*got=0;s.maxRead=std::max(s.maxRead,n);if(s.fault!=AMBI_OK)return s.fault;
            if(o>s.bytes.size()||n>s.bytes.size()-o)return AMBI_IO_ERROR;
            std::memcpy(d,s.bytes.data()+o,n);*got=n;return AMBI_OK;
        };return s; }
};
static std::string report(Source& s) {
    auto input=s.descriptor();uint32_t n=0;
    auto rc=AveMediaBridge_ClassifyContainerV1(&input,nullptr,0,&n);
    CHECK(rc==AMBC_BUFFER_TOO_SMALL);if(rc!=AMBC_BUFFER_TOO_SMALL||n<2||n>65536)return {};
    std::string out(n,'x');uint32_t fetched=0;
    CHECK(AveMediaBridge_ClassifyContainerV1(&input,out.data(),n,&fetched)==AMBR_OK);
    CHECK(fetched==n && out.back()=='\0');out.pop_back();
    CHECK(out.find("\"sourceToken\":\"42000000000000000000000000000000\"")!=std::string::npos);
    CHECK(out.find("\"byteSize\":\""+std::to_string(s.bytes.size())+"\"")!=std::string::npos);
    CHECK(out.find("\"policy\":\"mov-single-file\"")!=std::string::npos);
    CHECK(s.maxRead<=4194304);
    return out;
}
static void expect(const char* name,Bytes bytes,const char* classification,const char* reason=nullptr) {
    const auto hash=sha256(bytes);
    Source s{std::move(bytes)};auto json=report(s);
    const bool ok=json.find(std::string("\"classification\":\"")+classification+"\"")!=std::string::npos;
    CHECK(ok);std::cout<<name<<" sha256="<<hash<<" expected="<<classification<<" "<<(ok?"PASS":"FAIL")<<" "<<json<<'\n';
    if(reason)CHECK(json.find(std::string("\"reason\":\"")+reason+"\"")!=std::string::npos);
}
static size_t findTag(const Bytes& b,const char* tag,size_t instance=0) {
    auto begin=b.begin();
    for(size_t i=0;i<=instance;++i){auto at=std::search(begin,b.end(),tag,tag+4);if(at==b.end())throw std::runtime_error("fixture tag absent");begin=at+4;}
    return static_cast<size_t>(begin-b.begin()-4);
}
static Bytes mutation(Bytes b,const char* tag,size_t displacement,uint32_t value,size_t instance=0) {patch(b,findTag(b,tag,instance)+displacement,value);return b;}
static void budgets() {
    using namespace AveMediaBridge::Probe::Container;
    Source source{box("free")};auto input=source.descriptor();
    Reader exact(input);exact.charge(4000000);exact.charge(0);bool refused=false;
    try{exact.charge(1);}catch(const Unknown& u){refused=std::string(u.reason)=="workLimit";}CHECK(refused);
    Reader depth(input);depth.walk(0,8,32,[](Box){});refused=false;
    try{depth.walk(0,8,33,[](Box){});}catch(const Unknown& u){refused=std::string(u.reason)=="depthLimit";}CHECK(refused);
    // Exactly 1m visited headers is allowed; the next header is refused even
    // though the aggregate cap still has room (3m work units so far).
    Source many;many.bytes.reserve(8000008);const auto free=box("free");
    for(size_t i=0;i<1000001;++i)many.bytes.insert(many.bytes.end(),free.begin(),free.end());
    Reader boxReader(many.descriptor());uint64_t visited=0;boxReader.walk(0,8000000,1,[&](Box){++visited;});CHECK(visited==1000000);refused=false;
    try{boxReader.walk(8000000,8000008,1,[](Box){});}catch(const Unknown& u){refused=std::string(u.reason)=="boxLimit";}CHECK(refused);
    expect("BoxLimitCannotProve",std::move(many.bytes),"unknown","boxLimit");
    expect("DeclaredSampleWorkLimit",mutation(movie(),"stsz",12,4000001),"unknown","workLimit");
    expect("LimitDominatesExternal",movie(cat({track("url ",0),mutation(track(),"stsz",12,4000001)})),"unknown","workLimit");
    Bytes mdats;for(size_t i=0;i<4096;++i){auto m=box("mdat");mdats.insert(mdats.end(),m.begin(),m.end());}
    expect("ListAt4096",cat({movie(),Bytes(mdats.begin(),mdats.end()-8)}),"singleFile");
    expect("ListOver4096",cat({movie(),mdats}),"unknown","listLimit");
}
int wmain(int argc,wchar_t** argv) {
    for(const auto name:{L"AveMediaBridge.dll",L"avformat-61.dll",L"avcodec-61.dll",L"avutil-59.dll",L"swresample-5.dll"}) {
        wchar_t modulePath[32768]{};auto module=GetModuleHandleW(name);CHECK(module!=nullptr);
        if(module){CHECK(GetModuleFileNameW(module,modulePath,32768)>0);std::wcout<<L"loaded "<<name<<L" "<<modulePath<<L'\n';}
    }
    CHECK(container_abi_c());
    expect("SupportedLocalContainer",movie(),"singleFile");
    expect("UnselectedExternalTrackIsNotPortable",movie(cat({track(),track("url ",0)})),"externalDependencies");
    expect("SelectedExternalUrl",movie(track("url ",0)),"externalDependencies");
    expect("ExternalUrn",movie(track("urn ",0)),"externalDependencies");
    expect("ExternalAlias",movie(track("alis",0)),"externalDependencies");
    expect("UnusedExternalReference",movie(track("url ",1,1,24,4,{},box("url ",words({0})))),"externalDependencies");
    expect("UnusedUnknownReference",movie(track("url ",1,1,24,4,{},box("xxxx",words({0})))),"unknown");
    expect("DataReferenceIndexOutOfRange",movie(track("url ",1,2)),"unknown");
    expect("ChunkOutsideMdat",movie(track("url ",1,1,0)),"unknown");
    expect("ChunkEndOutsideMdat",movie(track("url ",1,1,24,5)),"unknown");
    expect("UnknownStructureIsNotProof",movie(track(),box("xxxx")),"unknown");
    for(auto type:{"mvex","cmov","iloc","uuid","moof"})expect(type,movie(track(),box(type)),"unknown");
    expect("DuplicateMoov",cat({movie(),box("moov")}),"unknown");
    auto truncated=movie();truncated.pop_back();expect("TruncatedBox",truncated,"unknown");
    expect("OverflowBox",cat({words({1}),Bytes{'m','o','o','v'},Bytes(8,255)}),"unknown");
    expect("UnsupportedContainer",Bytes{'R','I','F','F',0,0,0,0},"unknown");
    expect("NoTracks",movie({},{}),"unknown");
    const auto roll=box("sgpd",cat({words({0x01000000,0x726f6c6c,2,1}),Bytes{255,255}}));
    const auto groups=box("sbgp",words({0,0x726f6c6c,1,1,1}));
    expect("SupportedLocalRoll",movie(track("url ",1,1,24,4,cat({roll,groups}))),"singleFile");
    expect("RollUnpaired",movie(track("url ",1,1,24,4,roll)),"unknown");
    expect("RollBadDescription",movie(track("url ",1,1,24,4,cat({roll,box("sbgp",words({0,0x726f6c6c,1,1,2}))}))),"unknown");
    expect("RollBadCoverage",movie(track("url ",1,1,24,4,cat({roll,box("sbgp",words({0,0x726f6c6c,1,2,1}))}))),"unknown");
    auto unknownRoll=roll;patch(unknownRoll,12,0x78787878);
    expect("UnknownGroupIsNotProof",movie(track("url ",1,1,24,4,cat({unknownRoll,groups}))),"unknown");
    expect("DuplicateGroup",movie(track("url ",1,1,24,4,cat({roll,groups,roll}))),"unknown","duplicateStructure");
    expect("RollBadVersion",movie(track("url ",1,1,24,4,cat({mutation(roll,"sgpd",4,0x02000000),groups}))),"unknown");
    expect("RollBadFlags",movie(track("url ",1,1,24,4,cat({roll,mutation(groups,"sbgp",4,1)}))),"unknown");
    expect("RollFragmentGroupIndex",movie(track("url ",1,1,24,4,cat({roll,mutation(groups,"sbgp",20,65536)}))),"unknown");
    expect("BadSampleCount",mutation(movie(),"stsz",12,2),"unknown","sampleCount");
    expect("BadTimeCount",mutation(movie(),"stts",12,2),"unknown","timeCount");
    expect("BadChunkDescription",mutation(movie(),"stsc",20,2),"unknown","chunkMap");
    expect("BadSamplesPerChunk",mutation(movie(),"stsc",16,2),"unknown","chunkSampleCount");
    expect("BadFirstChunk",mutation(movie(),"stsc",12,2),"unknown","chunkMap");
    expect("UnknownSampleEntry",mutation(movie(),"mp4a",0,0x656e6361),"unknown","unsupportedSampleEntry");
    expect("UnknownTableVariant",movie(track("url ",1,1,24,4,box("stz2"))),"unknown","unknownStructure");
    expect("BadTableVersion",mutation(movie(),"stsz",4,0x01000000),"unknown","unsupportedVersion");
    expect("DuplicateTable",movie(track("url ",1,1,24,4,box("stco",words({0,1,24})))),"unknown","duplicateStructure");
    auto nestedZero=movie();patch(nestedZero,findTag(nestedZero,"stco")-4,0);
    expect("NestedZeroSizeIsNotProof",nestedZero,"unknown","nestedZeroSize");
    expect("SupportedAuxiliaryTables",movie(track("url ",1,1,24,4,cat({box("ctts",words({0,1,1,0})),box("stss",words({0,1,1})),box("sdtp",cat({words({0}),Bytes{0}}))}))),"singleFile");
    expect("BadCompositionCoverage",movie(track("url ",1,1,24,4,box("ctts",words({0,1,2,0})))),"unknown","compositionCount");
    expect("BadSyncIndex",movie(track("url ",1,1,24,4,box("stss",words({0,1,2})))),"unknown","syncSampleIndex");
    expect("BadDependencyCount",movie(track("url ",1,1,24,4,box("sdtp",words({0})))),"unknown","dependencyCount");
    auto esExternal=movie();esExternal[findTag(esExternal,"esds")+12]=0x40;
    expect("ESDescriptorExternalUrl",esExternal,"externalDependencies");
    expect("MalformedDescriptor",mutation(movie(),"esds",8,0x03ffffff),"unknown");
    expect("SpoofedSingleFileClaim",movie(track(),box("xxxx",Bytes{'s','i','n','g','l','e','F','i','l','e'})),"unknown","unknownStructure");
    budgets();
    Source source{movie()};auto input=source.descriptor();uint32_t n=0;char sentinel='Q';
    CHECK(AveMediaBridge_ClassifyContainerV1(&input,&sentinel,1,&n)==AMBC_BUFFER_TOO_SMALL);CHECK(sentinel=='Q');
    source.fault=AMBI_IO_ERROR;CHECK(AveMediaBridge_ClassifyContainerV1(&input,nullptr,0,&n)==AMBR_INPUT_FAILED);
    source.canceled=true;CHECK(AveMediaBridge_ClassifyContainerV1(&input,nullptr,0,&n)==AMBR_CANCELED);
    source.canceled=false;source.fault=AMBI_OK;source.throwCancel=true;CHECK(AveMediaBridge_ClassifyContainerV1(&input,nullptr,0,&n)==AMBR_INPUT_FAILED);
    source.throwCancel=false;auto invalid=input;invalid.reserved[0]=1;CHECK(AveMediaBridge_ClassifyContainerV1(&invalid,nullptr,0,&n)==AMBR_INVALID_ARGUMENT);
    invalid=input;invalid.byteSize=UINT64_MAX;CHECK(AveMediaBridge_ClassifyContainerV1(&invalid,nullptr,0,&n)==AMBR_INVALID_ARGUMENT);
    CHECK(AveMediaBridge_ClassifyContainerV1(&input,nullptr,1,&n)==AMBR_INVALID_ARGUMENT);
    if(argc==2)for(const auto& entry:std::filesystem::directory_iterator(argv[1])) {
        if(entry.path().extension()!=L".mp4")continue;
        std::ifstream f(entry.path(),std::ios::binary);Bytes b{std::istreambuf_iterator<char>(f),{}};
        expect(entry.path().filename().string().c_str(),b,"singleFile");
        expect("GeneratedSelectedExternal",mutation(b,"url ",4,0),"externalDependencies");
        expect("GeneratedUnknownTable",mutation(b,"stts",0,0x78787878),"unknown");
        expect("GeneratedInvalidOffset",mutation(b,"stco",12,0),"unknown");
        if(entry.path().filename()==L"front.mp4"||entry.path().filename()==L"multiple.mp4")
            expect("GeneratedUnselectedExternal",mutation(b,"url ",4,0,1),"externalDependencies");
        if(entry.path().filename()==L"no-audio.mp4"){
            Source video{b};auto inputVideo=video.descriptor();AMBR_PrepareOptionsV1 options{};options.structSize=sizeof(options);options.abiVersion=1;options.source=&inputVideo;options.displayLabel=L"generated no audio";
            AMBR_PreparedInput* prepared=nullptr;CHECK(AveMediaBridge_ReaderPrepareV1(&options,&prepared)!=AMBR_OK);if(prepared)AveMediaBridge_ReaderDestroyV1(prepared);
        }
    }
    std::cout<<"checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;
}
