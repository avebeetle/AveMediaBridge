#include "Probe/MovSingleFilePolicy.hpp"
#include <algorithm>
#include <initializer_list>
#include <map>
#include <vector>

namespace AveMediaBridge::Probe::Container {
namespace {
// Policy v1 is an explicit parent-specific allowlist, not a demux heuristic.
// root: ftyp, moov, mdat, free, skip; moov: mvhd, trak, udta;
// trak: tkhd, edts(elst), mdia(mdhd,hdlr,minf);
// minf: smhd/vmhd,dinf(dref),stbl; stbl: stsd,stts,stsc,stsz,
// stco/co64,ctts,stss,sdtp and paired roll-only sgpd v1/sbgp v0.
// Roll distances refer only to previous samples of this track, never locators.
// No fragments, cmov, iloc, stz2, other groups,
// encryption, unknown handlers/entries/boxes or versions are certified.
// mp4a v0 carries esds; avc1 carries avcC. Optional btrt/pasp contain only
// numeric bitrate/pixel-aspect fields. avcC/ASC contain inline codec bytes,
// never container locators. ES descriptors are separately bounded/validated.
// udta/meta(v0)/ilst allows only named iTunes text/covr entries with data
// children: their typed payload is inline text/image data, not structure.
// free/skip and mdat are opaque by definition. No other opaque box is allowed.
// Additional conservative caps:4096 entries in any retained box/ref/mdat list;
// depth32, visited boxes1m, aggregate work4m (Reader), 64KiB table window.
// Nested size=0 boxes are unsupported; only a root box may extend to EOF.
using Boxes=std::map<uint32_t,Box>;
bool oneOf(uint32_t t,std::initializer_list<uint32_t> allowed) {return std::find(allowed.begin(),allowed.end(),t)!=allowed.end();}
void unknown(Proof& p,const char* reason){p.reject(reason);}
Boxes children(Reader& r,Box b,uint32_t depth,std::initializer_list<uint32_t> allowed,Proof& p) {
    Boxes result;r.walk(b.data,b.end,depth,[&](Box child){
        if(!oneOf(child.type,allowed)){unknown(p,"unknownStructure");return;}
        require(result.emplace(child.type,child).second,"duplicateStructure");
    });return result;
}
Box needed(const Boxes& b,uint32_t t){const auto it=b.find(t);require(it!=b.end(),"missingStructure");return it->second;}
uint64_t full(Reader& r,Box b,uint32_t version=0,uint32_t flags=0) {
    require(b.size()>=4);const auto v=r.integer(b.data,4);require((v>>24)==version&&(v&0xffffff)==flags,"unsupportedVersion");return b.data+4;
}
uint32_t table(Reader& r,Box b,uint32_t width,uint32_t version=0) {
    full(r,b,version);require(b.size()>=8);auto count=static_cast<uint32_t>(r.integer(b.data+4,4));
    r.charge(count);require(b.size()-8==uint64_t(count)*width,"tableLength");return count;
}
void fixed(Reader& r,Box b,uint64_t size0,uint64_t size1=0,uint32_t flags=0) {
    require(b.size()>=4);const auto v=r.integer(b.data,4);const auto version=v>>24;
    require((v&0xffffff)==flags&&((version==0&&b.size()==size0)||(version==1&&size1&&b.size()==size1)),"unsupportedVersion");
}
struct Descriptor { uint8_t type;uint64_t begin,end; };
Descriptor descriptor(Reader& r,uint64_t& pos,uint64_t end) {
    require(pos<end);auto type=static_cast<uint8_t>(r.integer(pos++,1));uint32_t len=0;
    for(int i=0;i<4;++i){require(pos<end);auto b=r.integer(pos++,1);len=(len<<7)|static_cast<uint32_t>(b&127);
        if(!(b&128)){require(len<=end-pos);auto start=pos;pos+=len;return {type,start,pos};}}
    throw Unknown{"descriptorLength"};
}
void esds(Reader& r,Box b,Proof& proof) {
    auto pos=full(r,b);auto es=descriptor(r,pos,b.end);require(es.type==3&&pos==b.end&&es.end-es.begin>=3,"unknownDescriptor");
    auto flags=r.integer(es.begin+2,1);if(flags&0x40)proof.external=true;
    require(flags==0,"descriptorDependency");pos=es.begin+3;
    auto config=descriptor(r,pos,es.end);require(config.type==4&&config.end-config.begin>=13,"unknownDescriptor");
    require(r.integer(config.begin,1)==0x40&&(r.integer(config.begin+1,1)&0xfd)==0x15,"unsupportedCodec");
    auto inner=config.begin+13;auto asc=descriptor(r,inner,config.end);
    require(asc.type==5&&asc.end>asc.begin&&inner==config.end,"unknownDescriptor");
    // ASC bytes are inline codec setup, no further MPEG-4 descriptors.
    auto sl=descriptor(r,pos,es.end);require(sl.type==6&&sl.end-sl.begin==1&&r.integer(sl.begin,1)==2&&pos==es.end,"unknownDescriptor");
}
void metadata(Reader& r,Box udta,Proof& p) {
    auto u=children(r,udta,3,{tag("meta")},p);if(u.empty())return;
    auto meta=u.begin()->second;meta.data=full(r,meta);
    auto m=children(r,meta,4,{tag("hdlr"),tag("ilst")},p);
    auto h=needed(m,tag("hdlr"));full(r,h);require(h.size()>=24&&r.integer(h.data+8,4)==tag("mdir"),"metadataHandler");
    auto list=needed(m,tag("ilst"));r.walk(list.data,list.end,5,[&](Box item){
        if(!oneOf(item.type,{tag('\251','n','a','m'),tag('\251','A','R','T'),tag('\251','a','l','b'),tag('\251','d','a','y'),
            tag('\251','c','m','t'),tag('\251','t','o','o'),tag("covr")})){unknown(p,"unknownMetadata");return;}
        bool any=false;r.walk(item.data,item.end,6,[&](Box data){
            if(data.type!=tag("data")){unknown(p,"unknownMetadata");return;}
            require(data.size()>=8);const auto type=r.integer(data.data,4);
            require(item.type==tag("covr")?(type==13||type==14):type==1,"metadataType");any=true;
        });require(any,"emptyMetadata");
    });
}
std::vector<bool> references(Reader& r,Box dinf,Proof& p) {
    auto boxes=children(r,dinf,6,{tag("dref")},p);auto b=needed(boxes,tag("dref"));full(r,b);require(b.size()>=8);
    auto count=r.integer(b.data+4,4);require(count>0&&count<=4096,"listLimit");r.charge(count);std::vector<bool> refs;
    r.walk(b.data+8,b.end,7,[&](Box entry){
        require(refs.size()<count,"referenceCount");bool local=false;
        if(entry.type==tag("url ")) {
            require(entry.size()>=4);auto vf=r.integer(entry.data,4);
            if(vf==0)p.external=true;
            else if(vf==1&&entry.size()==4)local=true;
            else unknown(p,"unsupportedDataReference");
        }else if(entry.type==tag("urn ")||entry.type==tag("alis"))p.external=true;
        else unknown(p,"unknownDataReference");
        refs.push_back(local);
    });require(refs.size()==count,"referenceCount");return refs;
}
uint32_t descriptions(Reader& r,Box stsd,uint32_t handler,const std::vector<bool>& refs,Proof& p) {
    full(r,stsd);require(stsd.size()>=8);auto count=r.integer(stsd.data+4,4);require(count>0&&count<=4096,"listLimit");r.charge(count);uint32_t seen=0;
    r.walk(stsd.data+8,stsd.end,7,[&](Box entry){
        require(++seen<=count&&entry.size()>=8);auto index=r.integer(entry.data+6,2);
        require(index>0&&index<=refs.size(),"dataReferenceIndex");if(!refs[static_cast<size_t>(index-1)]&&!p.external)unknown(p,"unprovedDataReference");
        uint64_t header=0;uint32_t requiredConfig=0;
        if(entry.type==tag("mp4a")&&handler==tag("soun")) {
            require(entry.size()>=28&&r.integer(entry.data+8,2)==0,"sampleEntryVersion");header=28;requiredConfig=tag("esds");
        }else if(entry.type==tag("avc1")&&handler==tag("vide")){require(entry.size()>=78);header=78;requiredConfig=tag("avcC");}
        else {unknown(p,"unsupportedSampleEntry");return;}
        Box extensions=entry;extensions.data+=header;
        auto config=children(r,extensions,8,{requiredConfig,tag("btrt"),tag("pasp")},p);
        auto codec=needed(config,requiredConfig);
        if(requiredConfig==tag("esds"))esds(r,codec,p);
        else require(codec.size()>=7&&r.integer(codec.data,1)==1,"codecConfiguration");
        if(config.count(tag("btrt")))require(config.at(tag("btrt")).size()==12);
        if(config.count(tag("pasp")))require(config.at(tag("pasp")).size()==8);
    });require(seen==count,"descriptionCount");return seen;
}
void tables(Reader& r,Box stbl,uint32_t handler,const std::vector<bool>& refs,const std::vector<Box>& mdats,Proof& p) {
    auto b=children(r,stbl,6,{tag("stsd"),tag("stts"),tag("stsc"),tag("stsz"),tag("stco"),tag("co64"),tag("ctts"),tag("stss"),tag("sdtp"),tag("sgpd"),tag("sbgp")},p);
    auto desc=descriptions(r,needed(b,tag("stsd")),handler,refs,p);
    auto sizes=needed(b,tag("stsz"));full(r,sizes);require(sizes.size()>=12);
    auto fixedSize=r.integer(sizes.data+4,4),samples=r.integer(sizes.data+8,4);r.charge(samples);
    require(samples>0&&sizes.size()-12==(fixedSize?0:samples*4),"sampleCount");
    if(b.count(tag("sgpd"))||b.count(tag("sbgp"))) {
        auto descriptions=needed(b,tag("sgpd")),groups=needed(b,tag("sbgp"));
        full(r,descriptions,1);full(r,groups);require(descriptions.size()>=16&&groups.size()>=12,"groupLength");
        require(r.integer(descriptions.data+4,4)==tag("roll")&&r.integer(groups.data+4,4)==tag("roll"),"unknownGroup");
        require(r.integer(descriptions.data+8,4)==2,"groupLength");
        auto nd=r.integer(descriptions.data+12,4),ng=r.integer(groups.data+8,4);r.charge(nd);r.charge(ng);
        require(nd>0&&nd<=65535&&descriptions.size()-16==nd*2&&groups.size()-12==ng*8,"groupLength");
        for(uint64_t i=0;i<nd;++i){auto distance=r.integer(descriptions.data+16+i*2,2);require(distance==0||distance>=0x8000,"rollDistance");}
        uint64_t covered=0;
        for(uint64_t i=0;i<ng;++i){auto at=groups.data+12+i*8;auto count=r.integer(at,4),index=r.integer(at+4,4);
            require(count>0&&count<=samples-covered&&index<=nd,"groupReference");covered+=count;}
        require(covered==samples,"groupCoverage");
    }
    auto time=needed(b,tag("stts"));auto nt=table(r,time,8);uint64_t total=0;
    for(uint32_t i=0;i<nt;++i){auto count=r.integer(time.data+8+uint64_t(i)*8,4);require(count>0&&count<=samples-total,"timeCount");total+=count;}
    require(total==samples,"timeCount");
    if(b.count(tag("ctts"))){auto t=b.at(tag("ctts"));auto v=static_cast<uint32_t>(r.integer(t.data,1));require(v<=1,"unsupportedVersion");auto n=table(r,t,8,v);total=0;
        for(uint32_t i=0;i<n;++i){auto count=r.integer(t.data+8+uint64_t(i)*8,4);require(count>0&&count<=samples-total,"compositionCount");total+=count;}require(total==samples,"compositionCount");}
    if(b.count(tag("stss"))){auto t=b.at(tag("stss"));auto n=table(r,t,4);uint64_t prev=0;
        for(uint32_t i=0;i<n;++i){auto index=r.integer(t.data+8+uint64_t(i)*4,4);require(index>prev&&index<=samples,"syncSampleIndex");prev=index;}}
    if(b.count(tag("sdtp"))){auto t=b.at(tag("sdtp"));full(r,t);require(t.size()-4==samples,"dependencyCount");r.charge(samples);}
    require(b.count(tag("stco"))+b.count(tag("co64"))==1,"chunkTable");
    auto chunks=b.count(tag("stco"))?b.at(tag("stco")):b.at(tag("co64"));uint32_t width=chunks.type==tag("stco")?4:8;
    auto nc=table(r,chunks,width);require(nc>0,"chunkCount");auto map=needed(b,tag("stsc"));auto nm=table(r,map,12);require(nm>0,"chunkMap");
    uint64_t consumed=0,previous=0;
    for(uint32_t i=0;i<nm;++i){auto at=map.data+8+uint64_t(i)*12;auto first=r.integer(at,4),per=r.integer(at+4,4),description=r.integer(at+8,4);
        auto next=i+1<nm?r.integer(at+12,4):uint64_t(nc)+1;
        require(first>previous&&(i!=0||first==1)&&first<=nc&&next>first&&next<=uint64_t(nc)+1&&per>0&&description>0&&description<=desc,"chunkMap");previous=first;
        require(per<=samples&&(next-first)<=(samples-consumed)/per,"chunkSampleCount");
        for(auto chunk=first;chunk<next;++chunk){r.charge();uint64_t bytes=0;
            if(fixedSize){bytes=per*fixedSize;consumed+=per;}
            else for(uint64_t j=0;j<per;++j){auto size=r.integer(sizes.data+12+consumed*4,4);require(size<=r.size()-std::min(bytes,r.size()),"sampleExtent");bytes+=size;++consumed;}
            auto offset=r.integer(chunks.data+8+(chunk-1)*width,width);bool local=false;
            for(auto mdat:mdats){r.charge();if(offset>=mdat.data&&offset<=mdat.end&&bytes<=mdat.end-offset){local=true;break;}}
            require(local,"chunkOutsideMdat");
        }
    }require(consumed==samples,"chunkSampleCount");
}
void track(Reader& r,Box t,const std::vector<Box>& mdats,Proof& p) {
    auto b=children(r,t,3,{tag("tkhd"),tag("edts"),tag("mdia")},p);
    auto tk=needed(b,tag("tkhd"));require(tk.size()>=4);auto flags=static_cast<uint32_t>(r.integer(tk.data,4)&0xffffff);require((flags&~15u)==0);fixed(r,tk,84,96,flags);
    if(b.count(tag("edts"))){auto e=children(r,b.at(tag("edts")),4,{tag("elst")},p);auto elst=needed(e,tag("elst"));auto v=static_cast<uint32_t>(r.integer(elst.data,1));require(v<=1,"unsupportedVersion");table(r,elst,v?20:12,v);}
    auto m=children(r,needed(b,tag("mdia")),4,{tag("mdhd"),tag("hdlr"),tag("minf")},p);fixed(r,needed(m,tag("mdhd")),24,36);
    auto h=needed(m,tag("hdlr"));full(r,h);require(h.size()>=24);auto handler=static_cast<uint32_t>(r.integer(h.data+8,4));
    auto minf=children(r,needed(m,tag("minf")),5,{tag("smhd"),tag("vmhd"),tag("dinf"),tag("stbl")},p);
    auto refs=references(r,needed(minf,tag("dinf")),p);
    if(handler==tag("soun")){fixed(r,needed(minf,tag("smhd")),8);require(!minf.count(tag("vmhd")));}
    else if(handler==tag("vide")){fixed(r,needed(minf,tag("vmhd")),12,0,1);require(!minf.count(tag("smhd")));}
    else unknown(p,"unsupportedHandler");
    tables(r,needed(minf,tag("stbl")),handler,refs,mdats,p);
}
}
void movSingleFile(Reader& r,Proof& p) {
    std::vector<Box> mdats;Box moov;bool ftyp=false;
    r.walk(0,r.size(),1,[&](Box b){
        if(b.type==tag("moov")){require(moov.end==0,"duplicateMoov");moov=b;}
        else if(b.type==tag("mdat")){require(mdats.size()<4096,"listLimit");mdats.push_back(b);}
        else if(b.type==tag("ftyp")){require(!ftyp&&b.size()>=8&&(b.size()-8)%4==0);ftyp=true;}
        else if(b.type!=tag("free")&&b.type!=tag("skip"))unknown(p,"unknownStructure");
    });require(ftyp&&moov.end&&!mdats.empty(),"unsupportedContainer");
    uint32_t tracks=0;bool header=false,meta=false;
    r.walk(moov.data,moov.end,2,[&](Box b){
        try {
            if(b.type==tag("mvhd")){require(!header,"duplicateStructure");header=true;fixed(r,b,100,112);}
            else if(b.type==tag("trak")){require(tracks<4096,"listLimit");++tracks;track(r,b,mdats,p);}
            else if(b.type==tag("udta")){require(!meta,"duplicateStructure");meta=true;metadata(r,b,p);}
            else unknown(p,"unknownStructure");
        } catch(const Unknown& x){unknown(p,x.reason);}
    });require(header&&tracks>0,"missingTracks");
}
}
