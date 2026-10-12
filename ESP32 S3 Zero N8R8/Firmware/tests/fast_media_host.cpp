#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <vector>
#include "zifi/spi_link_core.hpp"
#include "zifi/protocol.hpp"
using namespace zifi;
struct TestLink {
  SpiLinkCore core;
  std::vector<uint8_t> input, output;
  TestLink(size_t capacity=16384):input(capacity),output(4096) {
    assert(core.begin(input.data(),input.size(),output.data(),output.size()));
  }
  std::vector<uint8_t> request(uint8_t id,uint8_t flags,const std::vector<uint8_t>& control={},
                               const std::vector<uint8_t>& dma={},size_t capacity=8192) {
    uint8_t req[60]={0xC3,id,flags,static_cast<uint8_t>(control.size()),0},resp[60];
    memcpy(req+5,control.data(),control.size());
    req[5+control.size()]=SpiLinkCore::checksum(req,5+control.size());
    SpiLinkCore::PutInput put;
    put.available=true;put.capacity=capacity;
    put.received=dma.empty()?nullptr:dma.data();put.receivedLength=dma.size();
    size_t n=0;
    assert(core.handleRequest(req,resp,n,SpiLinkCore::kBulkFree,nullptr,put));
    return {resp,resp+n};
  }
  std::vector<uint8_t> read() {
    std::vector<uint8_t> data(16384);
    data.resize(core.read(data.data(),data.size()));return data;
  }
};
bool collect(void* p,const uint8_t* data,size_t n) {
  auto& out=*static_cast<std::vector<uint8_t>*>(p);
  out.insert(out.end(),data,data+n);return true;
}
int main() {
  unsigned cases=0;
  for (uint8_t command: {uint8_t(kAudioData),uint8_t(kModPut)}) {
    for (size_t size: {1,2,511,512,513,1023,1024,4095,4096,8191,8192}) {
      TestLink t;
      const std::vector<uint8_t> prefix = command==kModPut ? std::vector<uint8_t>{0x78,0x56,0x34,0x12} : std::vector<uint8_t>{};
      std::vector<uint8_t> control={command,uint8_t(size),uint8_t(size>>8)};
      control.insert(control.end(),prefix.begin(),prefix.end());
      auto ready=t.request(1,SpiLinkCore::kFlagMedia,control);
      assert(ready[2]&SpiLinkCore::kStatusPutReady);
      assert(t.request(1,SpiLinkCore::kFlagMedia,control)==ready);
      const size_t padded=(size+511)&~size_t(511);
      assert(t.core.putDmaSize()==padded);
      assert(t.read().empty()); // control bytes must never leak into the packet stream
      std::vector<uint8_t> dma(padded,0xE5),expected=prefix;
      for(size_t i=0;i<size;++i) dma[i]=uint8_t(i*17+(i>>8));
      expected.insert(expected.end(),dma.begin(),dma.begin()+size);
      auto done=t.request(2,SpiLinkCore::kFlagPutDone,{},dma);
      assert(done[2]&SpiLinkCore::kStatusPutOk);
      auto bytes=t.read();
      assert(bytes.size()==expected.size()+4);
      FrameParser parser;PacketView packet;unsigned packets=0;
      for(size_t i=0;i<bytes.size();++i) {
        if(parser.feed(bytes[i],uint32_t(i/17),packet)) {
          ++packets;assert(i==bytes.size()-1);
          assert(packet.command==command&&packet.length==expected.size());
          assert(memcmp(packet.data,expected.data(),expected.size())==0);
        }
      }
      assert(packets==1&&parser.badChecksumCount()==0);
      assert(t.request(2,SpiLinkCore::kFlagPutDone,{},dma)==done);
      assert(t.read().empty()); // lost DONE reply must not duplicate bytes
      // Checked framing still works immediately after raw framing.
      std::vector<uint8_t> ordinary;
      assert(writeFrame(collect,&ordinary,kEcho,expected.data(),1));
      packets=0;
      for(auto b:ordinary) packets+=parser.feed(b,1000,packet);
      assert(packets==1&&packet.command==kEcho&&packet.length==1);
      ordinary.back()^=1;
      for(auto b:ordinary) assert(!parser.feed(b,1100,packet));
      assert(parser.badChecksumCount()==1);
      ++cases;
    }
  }
  {
    TestLink t;
    assert(t.request(1,0x10,{0x31,0,0x20})[2]&0x10);
    assert(t.request(2,8,{},std::vector<uint8_t>(8191))[2]&0x40);
    assert(t.read().empty()); // truncated DMA is rejected by its length
    assert(!(t.request(3,0x10,{0x31,1,0x20})[2]&0x10)); // over limit
    assert(!(t.request(4,0x10,{0x31,0,0})[2]&0x10));
    assert(!(t.request(5,0x10,{0x52,0,2})[2]&0x10)); // no unchecked filesystem writes
    assert(!(t.request(6,0x10,{0x6A,0,2})[2]&0x10)); // missing offset
    assert(!(t.request(7,0x10,{0x31,0,4},{},512)[2]&0x10)); // RX already armed small
  }
  {
    TestLink t(8192);
    assert(!(t.request(1,0x10,{0x31,0,0x20})[2]&0x10)); // also need frame header space
  }
  {
    FrameParser p;PacketView v;
    for(auto b:std::vector<uint8_t>{0xA6,0x52,0,2}) assert(!p.feed(b,0,v));
    assert(p.resyncCount()==1);
    for(auto b:std::vector<uint8_t>{0xA6,0x31,1,0x20}) assert(!p.feed(b,1,v));
    assert(p.resyncCount()==2);
    for(auto b:std::vector<uint8_t>{0xA6,0x31,0,2,1,2}) assert(!p.feed(b,2,v));
    assert(p.checkTimeout(1000));
    std::vector<uint8_t> frame;assert(writeFrame(collect,&frame,kPing));
    unsigned count=0;for(auto b:frame) count+=p.feed(b,1001,v);
    assert(count==1&&v.command==kPing);
  }
  printf("raw media C++: %u round trips; padding, duplicate, bounds, truncation, timeout and checked framing OK\n",cases);
}
