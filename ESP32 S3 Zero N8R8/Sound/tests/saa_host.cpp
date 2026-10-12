// Standalone SAA decoder test: lifetime, chunk boundaries, end, seek and bad input.
#include "zm_source.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <vector>
extern "C" int saa_main(int, char**);
static std::map<void*, size_t> allocations;
static int fail_after=-1;
static size_t peak=0, used=0;
static void* allocate(size_t n, uint32_t) {
  if (fail_after==0) return nullptr;
  if (fail_after>0) --fail_after;
  void* p=malloc(n); if(p){ allocations[p]=n; used+=n; peak=std::max(peak,used); } return p;
}
static void release(void* p) { if(p){ if(!allocations.count(p))abort(); used-=allocations[p];allocations.erase(p);free(p); } }
static void check(bool yes, const char* what) { if(!yes){fprintf(stderr,"FAIL: %s\n",what);exit(1);} }
int main(int argc,char**argv) {
  check(argc>=2,"input required");
  std::ifstream f(argv[1],std::ios::binary);
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),{});
  check(!bytes.empty(),"input exists");
  zm_host_t host{};host.size=sizeof(host);host.abi_major=ZM_SOURCE_ABI_MAJOR;host.alloc=allocate;host.free=release;
  const zm_source_t* source=nullptr;char* args[]={nullptr,(char*)&host,(char*)&source};
  check(saa_main(3,args)==1 && source,"ABI");
  check(source->probe(bytes.data(),bytes.size())==100,"probe");
  void* a=source->open((uint32_t)bytes.size(),bytes.data());check(a!=nullptr,"open");
  zm_info_t meta{};source->info(a,&meta);
  check(meta.rate==44100 && meta.channels==2 && meta.voices==6 && meta.duration_ms,"info");
  const size_t allocation_count=allocations.size();
  std::vector<float> first;float buf[2048];uint32_t got=0;uint64_t frames=0,hash=1469598103934665603ULL;
  double sum=0,diff=0;float max_value=0;
  FILE* raw=argc>2?fopen(argv[2],"wb"):nullptr;
  for(;;){
    int r=source->render(a,buf,1024,&got);
    check(r==ZM_RENDER_OK || r==ZM_RENDER_END,"render status");
    check(got<=1024 && (r==ZM_RENDER_END || got),"render progress");
    if(!got)break;
    if(first.size()<44100*2) first.insert(first.end(),buf,buf+std::min<uint32_t>(got*2,44100*2-(uint32_t)first.size()));
    for(unsigned j=0;j<got*2;++j){ check(std::isfinite(buf[j]) && fabs(buf[j])<=1,"PCM bounds"); sum+=buf[j]*buf[j]; max_value=std::max(max_value,fabsf(buf[j]));uint32_t bits;memcpy(&bits,buf+j,4);hash=(hash^bits)*1099511628211ULL; }
    for(unsigned j=0;j<got;++j)diff+=fabs(buf[2*j]-buf[2*j+1]);
    if(raw)fwrite(buf,sizeof(float),got*2,raw);
    frames+=got;check(frames<=uint64_t(meta.duration_ms)*44100/1000,"finite length");
  }
  if(raw)fclose(raw);
  check(frames==uint64_t(meta.duration_ms)*44100/1000,"exact duration");
  check(sum>0.01,"non silent");source->info(a,&meta);check(meta.position_ms==meta.duration_ms,"end position");
  uint32_t offset=0;check(source->seek(a,INT32_MIN,&offset)==0 && offset==UINT32_MAX,"seek start");
  size_t at=0;while(at<first.size()){
    uint32_t n=std::min<uint32_t>(137,(uint32_t)(first.size()-at)/2);
    check(source->render(a,buf,n,&got)==ZM_RENDER_OK && got==n,"fragment render");
    check(!memcmp(buf,first.data()+at,got*2*sizeof(float)),"chunk-independent PCM and reset");at+=got*2;
  }
  check(source->seek(a,10000,&offset)==0,"seek forward");source->info(a,&meta);
  check(meta.position_ms==std::min<uint32_t>(11000,meta.duration_ms),"seek position");
  uint8_t levels[6];check(source->levels(a,levels,6)==6,"meters");
  check(source->seek(a,INT32_MAX,&offset)==0,"seek end");check(source->render(a,buf,1024,&got)==ZM_RENDER_END && !got,"seek end status");
  source->close(a);check(allocations.empty(),"close frees all");
  for(size_t n=0;n<allocation_count+2;++n){fail_after=(int)n;a=source->open((uint32_t)bytes.size(),bytes.data());if(a)source->close(a);check(allocations.empty(),"allocation failure cleanup");}
  fail_after=-1;
  for(size_t n=0;n<bytes.size();++n){a=source->open((uint32_t)n,bytes.data());if(a)source->close(a);check(allocations.empty(),"truncation cleanup");}
  uint32_t seed=1;auto mutated=bytes;
  for(unsigned n=0;n<1000;++n){seed=seed*1664525+1013904223;size_t p=seed%mutated.size();uint8_t old=mutated[p];mutated[p]^=(uint8_t)(1u<<(seed%8));a=source->open((uint32_t)mutated.size(),mutated.data());if(a){source->render(a,buf,1024,&got);source->close(a);}mutated[p]=old;check(allocations.empty(),"mutation cleanup");}
  printf("OK ms=%u orders=%u samples=%u frames=%llu peak=%.6f rms=%.6f stereo=%.3f memory=%zu hash=%llx\n",meta.duration_ms,meta.order_length,meta.samples,(unsigned long long)frames,max_value,sqrt(sum/(frames*2)),diff,peak,(unsigned long long)hash);
}
