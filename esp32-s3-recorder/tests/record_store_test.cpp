#define MC2_RECORD_HOST_TEST 1
#include <cassert>
#include <iostream>
#include <vector>
#include <algorithm>
#include "../firmware/MC2_Live/record_store.h"
using namespace mc2record;
static unsigned checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::cerr << "FAIL line " << __LINE__ << ": " #x << std::endl; std::abort(); } } while (0)
static int16_t pcm[4096], review[4096];
static void reset() { mockPsramAvailable=true; LittleFS = MockLittleFS{}; std::fill(mockPartitionBytes.begin(), mockPartitionBytes.end(), 0xFF); }
static void add(const char* path, uint32_t samples, bool incomplete=false) {
  uint8_t h[WAV_HEADER]; makeWavHeader(h, incomplete?0:samples);
  auto f=LittleFS.open(path,"w");f.write(h,sizeof(h));
  for(uint32_t n=0;n<samples;++n){uint8_t s[2];put16(s,uint16_t(int16_t(n-100)));f.write(s,2);}f.close();
}
static void finish(RecordStore& s){for(int n=0;n<1024&&s.busy();++n)s.tick();CHECK(!s.busy());s.tick();}
int main() {
  for(unsigned n=0;n<4096;++n)pcm[n]=int16_t(n-2000);
  uint8_t header[44];makeWavHeader(header,48000);
  CHECK(inspectWav(header,44,96044,false).valid);
  CHECK(inspectWav(header,44,96044,false).samples==48000);
  CHECK(!inspectWav(header,44,96042,false).valid);
  CHECK(!inspectWav(header,43,96044,false).valid);
  header[22]=2;CHECK(!inspectWav(header,44,96044,false).valid);header[22]=1;
  makeWavHeader(header,0);CHECK(inspectWav(header,44,51,true).samples==3);
  CHECK(!inspectWav(header,44,51,false).valid);
  CHECK(recordingName("/D0001.wav").demo);
  CHECK(recordingName("/M9999.part").partial);
  CHECK(recordingName("rec_001.wav").legacy);
  CHECK(!recordingName("/important.wav").owned);
  CHECK(!recordingName("/D0001.wav.backup").owned);
  CHECK(!recordingName("/D00x1.wav").owned);
  reset();
  {
    RecordStore s;CHECK(s.begin());CHECK(s.ready());CHECK(s.count()==0);
    CHECK(s.start(true));CHECK(std::string(s.activeName())=="D0001.wav");
    CHECK(s.enqueue(pcm,4096));CHECK(s.writtenSamples()==0);
    s.tick();CHECK(s.writtenSamples()==0);CHECK(s.acceptedSamples()==4096);s.requestStop();finish(s);
    CHECK(s.count()==1);CHECK(s.entry(0).valid);CHECK(s.entry(0).demo);CHECK(!s.entry(0).partial);CHECK(s.entry(0).samples==4096);
    CHECK(s.openReview(0));CHECK(s.readReviewFrame(review,4096)==4096);CHECK(!std::memcmp(pcm,review,sizeof(pcm)));
    CHECK(s.readReviewFrame(review,4096)==0);CHECK(std::all_of(std::begin(review),std::end(review),[](int16_t v){return v==0;}));
    CHECK(s.rewindReview());CHECK(s.readReviewFrame(review,16)==16);CHECK(review[0]==pcm[0]);s.closeReview();
    CHECK(s.start(false));CHECK(std::string(s.activeName())=="M0002.wav");
    CHECK(s.enqueue(pcm,300));s.requestStop();finish(s);CHECK(s.count()==2);CHECK(s.entry(1).samples==300);
    CHECK(s.openReview(1));CHECK(s.readReviewFrame(review,4096)==300);CHECK(review[299]==pcm[299]);CHECK(review[300]==0);
    CHECK(s.deleteAt(1));CHECK(s.reviewTotalSamples()==0);CHECK(s.count()==1);CHECK(!LittleFS.exists("/M0002.wav"));
  }
  // Persisted earlier files and incomplete recordings remain reachable after a new boot.
  {
    add("/rec_008.wav",20);add("/M0003.part",12,true);
    auto keep=LittleFS.open("/notes.txt","w");keep.write(reinterpret_cast<const uint8_t*>("KEEP"),4);keep.close();
    RecordStore reboot;CHECK(reboot.begin());CHECK(reboot.count()==3);
    CHECK(std::string(reboot.entry(1).name)=="M0003.part");CHECK(reboot.entry(1).valid&&reboot.entry(1).partial);
    CHECK(reboot.entry(2).legacy);CHECK(reboot.openReview(1));CHECK(reboot.readReviewFrame(review,4096)==12);CHECK(review[12]==0);
    CHECK(reboot.deleteAll());CHECK(reboot.count()==0);CHECK(LittleFS.exists("/notes.txt"));CHECK(LittleFS.files.size()==1);
  }
  reset();
  {
    RecordStore s;CHECK(s.begin());CHECK(s.start(true));
    for(unsigned n=0;n<12;++n){CHECK(s.enqueue(pcm,4096));s.tick();}
    CHECK(s.recording());CHECK(s.acceptedSamples()==12*4096);CHECK(s.writtenSamples()==0);
    CHECK(LittleFS.openWriteCalls==0);CHECK(LittleFS.writeCalls==0);CHECK(LittleFS.files.empty());
    s.requestStop();finish(s);CHECK(s.writtenSamples()==12*4096);
    CHECK(s.entry(0).valid);CHECK(s.entry(0).samples==s.writtenSamples());
    CHECK(mockPsramBytes==960000);
    CHECK(std::all_of(static_cast<uint8_t*>(mockPsramAllocation),static_cast<uint8_t*>(mockPsramAllocation)+mockPsramBytes,[](uint8_t v){return v==0;}));
  }
  reset();
  {
    RecordStore s;CHECK(s.begin());CHECK(s.start(true));
    for(unsigned n=0;n<118;++n){CHECK(s.enqueue(pcm,4096));s.tick();}
    finish(s);CHECK(s.writtenSamples()==480000);CHECK(s.entry(0).samples==480000);CHECK(s.entry(0).valid);
    CHECK(LittleFS.writeCalls==119); // one final header and118 PCM blocks; no seek/rewrite.
    CHECK(std::string(s.lastReason())=="TIME LIMIT");
    CHECK(s.openReview(0));uint32_t total=0;size_t n;
    while((n=s.readReviewFrame(review,4096)))total+=n;
    CHECK(total==480000);
  }
  reset();
  {
    LittleFS.capacity=RecordStore::SPACE_RESERVE+44+100;
    RecordStore s;CHECK(s.begin());CHECK(!s.start(true));CHECK(std::string(s.lastReason())=="STORAGE FULL");CHECK(s.count()==0);
  }
  reset();
  {
    RecordStore s;CHECK(s.begin());CHECK(s.start(true));LittleFS.writeBudget=44+7;
    CHECK(s.enqueue(pcm,4096));s.requestStop();s.tick();CHECK(!s.busy());CHECK(std::string(s.lastReason())=="WRITE FAILED");
    CHECK(s.count()==1);CHECK(s.entry(0).partial&&s.entry(0).valid);CHECK(s.entry(0).samples==3);
    CHECK(s.openReview(0));CHECK(s.readReviewFrame(review,4096)==3);CHECK(review[0]==pcm[0]);CHECK(review[3]==0);
    LittleFS.writeBudget=-1;CHECK(s.deleteAt(0));CHECK(s.count()==0);
  }
  reset();
  {
    RecordStore s;CHECK(s.begin());CHECK(s.start(true));CHECK(s.enqueue(pcm,4096));s.tick();
    LittleFS.writeBudget=44+8192;s.requestStop();finish(s);
    CHECK(std::string(s.lastReason())=="STOPPED");CHECK(!s.entry(0).partial&&s.entry(0).valid);CHECK(s.entry(0).samples==4096);
    CHECK(LittleFS.writeCalls==2);CHECK(LittleFS.writeBudget==0); // one final header + PCM, no rewrite.
  }
  reset();
  {
    RecordStore s;CHECK(s.begin());CHECK(s.start(true));CHECK(s.enqueue(pcm,4096));
    LittleFS.truncateOnClose=0;s.requestStop("TIME LIMIT");finish(s);
    CHECK(std::string(s.lastReason())=="VERIFY FAILED");CHECK(s.count()==1);
    CHECK(s.entry(0).partial&&!s.entry(0).valid);CHECK(!LittleFS.exists("/D0001.wav"));
  }
  reset();
  {
    RecordStore s;CHECK(s.begin());CHECK(s.start(true));CHECK(s.enqueue(pcm,4096));
    LittleFS.truncateOnClose=44+10;s.requestStop();finish(s);
    CHECK(std::string(s.lastReason())=="VERIFY FAILED");CHECK(s.entry(0).partial&&s.entry(0).valid);
    CHECK(s.entry(0).samples==5);CHECK(!LittleFS.exists("/D0001.wav"));
    CHECK(s.openReview(0));CHECK(s.readReviewFrame(review,4096)==5);CHECK(review[5]==0);
  }
  reset();
  {
    RecordStore s;CHECK(s.begin());CHECK(s.start(true));CHECK(s.enqueue(pcm,4096));s.tick();
    LittleFS.renameOk=false;s.requestStop();finish(s);
    CHECK(std::string(s.lastReason())=="RENAME FAILED");CHECK(s.entry(0).partial&&s.entry(0).valid);
    LittleFS.removeOk=false;CHECK(!s.deleteAt(0));CHECK(s.count()==1);CHECK(LittleFS.exists("/D0001.part"));
  }
  reset();
  {
    // Existing folders beyond the new-record cap are fully individually
    // browseable, including a selected recording in a later cache page.
    for(unsigned i=1;i<=70;++i){char name[24];snprintf(name,sizeof(name),"/D%04u.wav",i);add(name,2);}
    auto keep=LittleFS.open("/important.txt","w");keep.close();
    RecordStore s;CHECK(s.begin());CHECK(s.count()==70);CHECK(s.listOverflow());
    CHECK(std::string(s.entry(64).name)=="D0065.wav");CHECK(s.entry(64).valid);
    CHECK(std::string(s.entry(0).name)=="D0001.wav");
    CHECK(s.openReview(69));CHECK(s.readReviewFrame(review,4096)==2);CHECK(review[2]==0);
    CHECK(!s.start(true));CHECK(std::string(s.lastReason())=="FILE LIMIT");
    CHECK(s.deleteAt(69));CHECK(!LittleFS.exists("/D0070.wav"));CHECK(s.count()==69);
    CHECK(std::string(s.entry(68).name)=="D0069.wav");
    CHECK(s.deleteAll());CHECK(s.count()==0);CHECK(LittleFS.exists("/important.txt"));CHECK(LittleFS.files.size()==1);
  }
  reset();
  {
    RecordStore s;CHECK(s.begin());CHECK(s.start(true));CHECK(s.enqueue(pcm,4096));
    s.requestStop("TIMING GAP");finish(s);CHECK(std::string(s.lastReason())=="TIMING GAP");
  }
  reset();
  {
    LittleFS.mountOk=false;RecordStore s;CHECK(!s.begin());CHECK(LittleFS.formatCalls==0);
    mockPartitionBytes.back()=0;CHECK(!s.formatBlankPartition());CHECK(LittleFS.formatCalls==0);CHECK(std::string(s.lastReason())=="NOT BLANK");
    mockPartitionBytes.back()=0xFF;CHECK(s.formatBlankPartition());CHECK(LittleFS.formatCalls==1);CHECK(s.ready());
  }
  reset();
  {
    // No silent overwrite and no startup cleanup: invalid .part can be deleted.
    auto f=LittleFS.open("/D0001.part","w");uint8_t x[3]={1,2,3};f.write(x,3);f.close();
    RecordStore s;CHECK(s.begin());CHECK(s.count()==1&&!s.entry(0).valid);CHECK(!s.openReview(0));
    CHECK(s.start(true));CHECK(std::string(s.activeName())=="D0002.wav");s.requestStop();finish(s);
    CHECK(LittleFS.files.at("/D0001.part")->data.size()==3);CHECK(s.deleteAll());CHECK(LittleFS.files.empty());
  }
  reset();
  {
    mockPsramAvailable=false;add("/D0001.wav",12);
    RecordStore s;CHECK(s.begin());CHECK(s.ready());CHECK(!s.recordingAvailable());
    CHECK(s.count()==1);CHECK(s.openReview(0));CHECK(s.readReviewFrame(review,4096)==12);
    CHECK(!s.start(true));CHECK(std::string(s.lastReason())=="NEED PSRAM");
    CHECK(s.deleteAll());CHECK(s.count()==0);
  }
  reset();
  {
    RecordStore s;CHECK(s.begin());CHECK(s.start(true));CHECK(s.enqueue(pcm,4096));
    LittleFS.writeBudget=3;CHECK(s.writtenSamples()==0);CHECK(LittleFS.files.empty());
    s.requestStop();finish(s);CHECK(std::string(s.lastReason())=="WRITE FAILED");
    CHECK(s.count()==1);CHECK(s.entry(0).partial&&!s.entry(0).valid);CHECK(s.deleteAt(0));
  }
  std::cout<<"PASS "<<checks<<" recording-store checks: WAV integrity/length, source names, PSRAM-only capture/full/write/header/commit/rename failures, EOF/zero padding, historical/partial browse, scoped deletion, blank-only initialization"<<std::endl;
}
