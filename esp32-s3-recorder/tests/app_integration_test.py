#!/usr/bin/env python3
"""Compile CURRENT main .ino with real DSP/UI and fake peripherals/storage.
This verifies application glue and UI state transitions, not flash durability,
LittleFS, ADC timing, GPIO or physical display quality.
"""
from pathlib import Path
import hashlib, json, subprocess, tempfile, datetime
ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "firmware/MC2_Live"
MOCK = r"""
#pragma once
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "signal_core.h"
static uint64_t testUs=0;
static bool greenDown=false,redDown=false,adcStartOkay=true;
inline uint32_t millis(){return uint32_t(testUs/1000);}
inline uint32_t micros(){return uint32_t(testUs);}
inline void delay(unsigned n){testUs+=n*1000;}
constexpr int SERIAL_8N1=0;
struct MockSerial {
 std::string log;
 void begin(int,int,int,int){}
 void println(const char*s){log+=s;log+='\n';}
 void printf(const char*f,...){char b[2048];va_list a;va_start(a,f);vsnprintf(b,sizeof b,f,a);va_end(a);log+=b;}
 int available(){return 0;} int read(){return -1;}
} Serial0;
struct MockESP {unsigned getFreeHeap(){return 200000;} unsigned getFlashChipSize(){return 8388608;} const char*getSdkVersion(){return "MOCK";}} ESP;
namespace mc2hw {
 struct ButtonEvents {};
 struct LiveDisplays {
  std::string first,second;unsigned renders=0,clears=0;
  bool begin(){return true;} void tick(){} ButtonEvents takeButtons(){return {};}
  bool buttonsArmed(){return true;} int greenLevel(){return !greenDown;} int redLevel(){return !redDown;}
  void setText(const char*a,const char*b){first=std::string(a).substr(0,16);second=std::string(b).substr(0,16);}
  void showRows(const uint8_t[8][4]){++renders;}
  void clearMatrix(){++clears;} void wakeMatrix(bool=true){}
  bool lcdReady(){return true;} bool lcdFault(){return false;}unsigned i2cErrors(){return 0;}bool retryLcd(){return true;}
 };
}
namespace mc2live {
 struct AudioCapture {
  bool running_=false;
  bool start(int){running_=adcStartOkay;return running_;} void stop(){running_=false;}
  void poll(){} bool running(){return running_;} bool popFrame(int16_t*,FrameStats&){return false;}
  uint64_t samples(){return 0;}uint32_t droppedFrames(){return 0;}uint32_t poolOverflows(){return 0;}
 };
}
namespace mc2record {
 struct Entry {char name[24]={};uint32_t samples=0;bool demo=false,partial=false,valid=true,legacy=false;};
 struct RecordStore {
  std::vector<Entry> entries;std::vector<std::vector<int16_t>> data;
  std::vector<int16_t> pending;std::string active,reason="READY",deleted;
  bool ready_=true,recording_=false,busy_=false,failRead=false,unrelatedFile=true;
  uint32_t saved=0,accepted=0;size_t reviewing=0,readAt=0;bool reviewOpen=false;
  bool begin(){return ready_;}bool ready()const{return ready_;}bool recording()const{return recording_;}bool busy()const{return busy_;}
  bool formatBlankPartition(){ready_=true;return true;}
  uint32_t acceptedSamples()const{return accepted;}uint32_t writtenSamples()const{return saved;}const char*activeName()const{return active.c_str();}const char*lastReason()const{return reason.c_str();}
  size_t count()const{return entries.size();}const Entry&entry(size_t i)const{return entries.at(i);}
  unsigned storageTotalBytes()const{return 1572864;}unsigned storageUsedBytes()const{return 4096;}unsigned freeBytes()const{return 1568768;}
  void seed(const std::string&name,bool demo,const std::vector<int16_t>&samples){Entry e;snprintf(e.name,sizeof e.name,"%s",name.c_str());e.demo=demo;e.samples=samples.size();entries.push_back(e);data.push_back(samples);}
  bool start(bool demo){if(!ready_||busy_)return false;char n[24];unsigned id=1;for(;;++id){snprintf(n,sizeof n,"%c%04u.wav",demo?'D':'M',id);bool used=false;for(auto&e:entries){std::string x=e.name;if(x.substr(1,4)==std::string(n).substr(1,4))used=true;}if(!used)break;}active=n;pending.clear();saved=accepted=0;recording_=busy_=true;reason="RECORDING";return true;}
  bool enqueue(const int16_t*p,size_t n){if(!recording_)return false;pending.insert(pending.end(),p,p+n);accepted+=n;return true;}
  void requestStop(const char*r="STOPPED"){recording_=false;reason=r;}
  void tick(){if(busy_&&!recording_){saved=accepted;seed(active,active[0]=='D',pending);pending.clear();busy_=false;}}
  void closeReview(){reviewOpen=false;readAt=0;}
  bool openReview(size_t i){if(i>=count()||!entries[i].valid)return false;reviewing=i;readAt=0;reviewOpen=true;return true;}
  size_t readReviewFrame(int16_t*out,size_t n){std::fill(out,out+n,0);if(failRead){reason="READ FAILED";reviewOpen=false;return 0;}if(!reviewOpen)return 0;size_t take=std::min(n,data[reviewing].size()-readAt);std::copy_n(data[reviewing].data()+readAt,take,out);readAt+=take;return take;}
  bool deleteAt(size_t i){if(i>=count())return false;deleted=entries[i].name;entries.erase(entries.begin()+i);data.erase(data.begin()+i);return true;}
  bool deleteAll(){entries.clear();data.clear();deleted="ALL";return true;}
 };
}
"""
TEST = r"""
#include <iostream>
#include <cstdlib>
static unsigned checks=0;
static void check(bool ok,const char*label){++checks;if(!ok){std::cerr<<"FAIL "<<label<<" mode="<<modeName()<<" selected="<<selected<<" LCD="<<hardware.first<<"/"<<hardware.second<<"\n";std::exit(1);}}
static void commandLine(const char*s){char b[96];snprintf(b,sizeof b,"%s",s);handleCommand(b);}
static void tickMs(uint32_t n){testUs+=uint64_t(n)*1000;loop();}
static void tap(bool green){if(green)greenDown=true;else redDown=true;tickMs(1);tickMs(80);if(green)greenDown=false;else redDown=false;tickMs(1);}
static void holdGreen(){greenDown=true;tickMs(1);tickMs(1200);greenDown=false;tickMs(1);}
static void holdRed(){redDown=true;tickMs(1);tickMs(1200);redDown=false;tickMs(1);}
static std::vector<int16_t> tone(size_t mode,size_t n){std::vector<int16_t>x(n);size_t at=0;int16_t block[FFT_N];while(at<n){fillDemo(block,mode,at);size_t take=std::min(n-at,FFT_N);std::copy_n(block,take,x.data()+at);at+=take;}return x;}
static void resetApp(){greenDown=redDown=false;adcStartOkay=true;testUs=0;hardware=mc2hw::LiveDisplays{};recordings=mc2record::RecordStore{};controls=mc2ui::Controller{};acquisition=AudioCapture{};source=Source::DEMO;view=View::FREQ;demoMode=1;selected=0;demoSample=renderedFrames=replaySamples=0;hasFrame=false;digitalReady=false;reviewEnd=deleteAllSelected=false;visualGain=1;nextFrameUs=nextReplayUs=heartbeatAt=lastAdcFrameAt=toastUntil=demoLateFrames=0;toast[0]=0;deleteTarget[0]=0;Serial0.log.clear();setup();loop();}
int main(){
 resetApp();
 check(source==Source::DEMO && !acquisition.running(),"boot DEMO no microphone");
 check(hardware.first=="DEMO LIVE FREQ","LCD explicit DEMO source and domain");
 tap(true);check(controls.mode()==Mode::RECORDING && recordings.recording(),"button starts recording");
 check(hardware.first.find("D0001 REC FREQ")==0,"LCD current recording id REC domain");
 tickMs(100);check(recordings.acceptedSamples()>0,"recording receives PCM into capture buffer");
 check(recordings.writtenSamples()==0,"no flash PCM writes during recording");
 char expectedElapsed[48];snprintf(expectedElapsed,sizeof expectedElapsed,"DEMO %.1fs G:STOP",float(recordings.acceptedSamples())/SAMPLE_RATE);
 check(hardware.second==std::string(expectedElapsed).substr(0,16),"REC LCD elapsed uses accepted samples before flash save");
 check(recordings.pending.size()>=FFT_N && std::equal(pcm,pcm+FFT_N,recordings.pending.end()-FFT_N),"record and live share identical PCM");
 auto before=recordings.acceptedSamples();tap(false);check(view==View::TIME && recordings.recording(),"red switches TIME while recording");
 tap(false);check(view==View::FREQ && recordings.recording(),"red switches FREQ while recording");
 check(recordings.acceptedSamples()>=before,"domain switch does not stop capture buffer");
 commandLine("DEMO 3");check(source==Source::DEMO && demoMode==1 && controls.mode()==Mode::RECORDING,"DEMO source changes blocked during recording");
 commandLine("ADC CONFIRM");check(source==Source::DEMO && !acquisition.running(),"ADC source changes blocked during recording");
 commandLine("OFF");check(recordings.recording(),"input OFF rejected while recording");
 tap(true);check(controls.mode()==Mode::FINALIZING && recordings.busy(),"green stops into saving state");
 tickMs(1);check(controls.mode()==Mode::BROWSE && recordings.count()==1 && selected==0,"finalized first file browsable");
 check(hardware.first.find("D0001")==0,"LCD browsed recording id");
 holdGreen();check(controls.mode()==Mode::LIVE,"green hold exits files");
 tap(true);tickMs(100);tap(true);tickMs(1);
 check(recordings.count()==2 && selected==1 && std::string(recordings.entry(selected).name)=="D0002.wav","newest recording selected after second stop");
 tap(true);check(selected==2 && hardware.first.find("ALL FILES")==0,"browser includes ALL virtual entry");
 tap(true);check(selected==0,"next wraps from ALL to first historical file");
 tap(true);check(selected==1,"next reaches second file");
 holdRed();check(controls.mode()==Mode::REVIEW && hasFrame,"red hold opens saved recording review");
 check(pcm[1]==recordings.data[1][1],"review uses saved PCM");
 tap(false);check(view==View::TIME && controls.mode()==Mode::REVIEW,"TIME while saved review");
 commandLine("LIVE");commandLine("OFF");commandLine("REC");
 check(source==Source::NONE && controls.mode()==Mode::LIVE && !recordings.recording(),"input NONE cannot record");
 commandLine("DEMO 1");adcStartOkay=false;commandLine("ADC CONFIRM");
 check(source==Source::NONE && !hasFrame && !acquisition.running(),"failed ADC start clears DEMO frame and marks NONE");

 // Replay a deliberately different known tone from the current DEMO signal.
 resetApp();recordings.seed("D0042.wav",true,tone(2,FFT_N+1000));
 commandLine("FILES");commandLine("PLAY");loop();
 check(controls.mode()==Mode::REVIEW && hasFrame,"saved playback starts");
 check(std::fabs(spectrum.peakHz-3000)<12,"saved 3000Hz analysed, not live 750Hz");
 auto firstSampleCount=replaySamples;loop();check(replaySamples==firstSampleCount,"review paced, not FFT-speed EOF");
 tickMs(86);check(replaySamples==FFT_N+1000,"partial final saved frame counted exactly");
 std::vector<int16_t> finalPcm(pcm,pcm+FFT_N);auto finalSpectrum=spectrum;
 tickMs(30);check(reviewEnd && controls.mode()==Mode::REVIEW,"exact EOF becomes END");
 check(std::equal(pcm,pcm+FFT_N,finalPcm.begin()),"EOF keeps last PCM");
 check(spectrum.peakHz==finalSpectrum.peakHz,"EOF keeps matching spectrum");
 commandLine("TIME");check(hasFrame && reviewEnd && std::equal(pcm,pcm+FFT_N,finalPcm.begin()),"TIME toggle at EOF retains coherent last saved frame");
 commandLine("FREQ");check(spectrum.peakHz==finalSpectrum.peakHz,"FREQ toggle at EOF retains spectrum");
 commandLine("PLAY");check(!reviewEnd && replaySamples==0,"PLAY restarts selected recording");
 recordings.failRead=true;loop();
 check(controls.mode()==Mode::BROWSE && !hasFrame && !reviewEnd,"read failure is BROWSE error, not END");
 check(std::all_of(pcm,pcm+FFT_N,[](int16_t p){return p==0;}),"read error clears cached PCM");

 resetApp();recordings.seed("D0001.wav",true,tone(1,FFT_N));recordings.seed("M0002.wav",false,tone(2,FFT_N));
 commandLine("FILES");commandLine("NEXT");commandLine("DELETE");
 check(controls.mode()==Mode::DELETE_CONFIRM && std::string(deleteTarget)=="M0002.wav","delete prompt captures selected file");
 commandLine("NEXT");check(selected==1,"navigation blocked in delete prompt");
 commandLine("CANCEL");check(recordings.count()==2 && recordings.deleted.empty(),"cancel deletes nothing");
 commandLine("DELETE");selected=0; // Simulate external index drift after prompt.
 commandLine("CONFIRM");check(recordings.deleted=="M0002.wav" && recordings.count()==1,"confirmation deletes captured filename, not drifted index");
 check(!hasFrame && replaySamples==0 && std::all_of(pcm,pcm+FFT_N,[](int16_t p){return p==0;}),"confirmed deletion clears review PCM and position");
 commandLine("NEXT");check(selected==recordings.count(),"ALL entry reachable after deletion");
 commandLine("DELETE");check(deleteAllSelected && std::string(deleteTarget)=="ALL AUDIO","ALL prompt explicit");
 commandLine("CONFIRM");check(recordings.count()==0 && recordings.unrelatedFile,"ALL only removes owned recording entries");
 check(hardware.first=="FILES EMPTY 0/0","LCD empty file state");
 holdGreen();check(controls.mode()==Mode::LIVE,"empty browser exits by hold");

 // Full physical-style chord path, with release gating and red cancellation.
 resetApp();recordings.seed("D0007.wav",true,tone(1,FFT_N));holdGreen();
 greenDown=redDown=true;tickMs(1);tickMs(1500);
 check(controls.mode()==Mode::DELETE_CONFIRM,"button chord opens delete prompt");
 greenDown=false;tickMs(1);redDown=false;tickMs(1);
 check(controls.mode()==Mode::DELETE_CONFIRM && recordings.count()==1,"chord release cannot confirm or cancel");
 tap(false);check(controls.mode()==Mode::BROWSE && recordings.count()==1,"button cancel preserves selected file");
 greenDown=redDown=true;tickMs(1);tickMs(1500);greenDown=redDown=false;tickMs(1);tap(true);
 check(controls.mode()==Mode::BROWSE && recordings.count()==0,"button confirmation deletes selected file");
 std::cout<<"PASS "<<checks<<" actual main app integration checks\n";
}
"""

def main():
    path=SRC/'MC2_Live.ino'
    original=path.read_text()
    transformed=original.replace('#include <Arduino.h>','#include "app_mocks.h"')
    for name in ['audio_capture.h','live_displays.h','record_store.h']:
        transformed=transformed.replace(f'#include "{name}"','')
    transformed=transformed.replace('using mc2ui::Action;','using mc2ui::Action;\nstatic void handleAction(Action);')
    with tempfile.TemporaryDirectory(prefix='mc2-v05-app-') as folder:
        temp=Path(folder)
        (temp/'app_mocks.h').write_text(MOCK)
        (temp/'app_test.cpp').write_text(transformed+'\n'+TEST)
        cmd=['c++','-std=c++17','-Wall','-Wextra','-Werror','-pedantic','-I',str(SRC),'-I',str(temp),str(temp/'app_test.cpp'),'-o',str(temp/'app_test')]
        built=subprocess.run(cmd,capture_output=True,text=True)
        if built.returncode:
            print(built.stdout+built.stderr)
            raise SystemExit(built.returncode)
        ran=subprocess.run([str(temp/'app_test')],capture_output=True,text=True)
        print(ran.stdout+ran.stderr,end='')
        report={'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'scope':'Actual CURRENT main .ino + real signal_core/recorder_ui; peripheral/storage APIs mocked','source_sha256':{n:hashlib.sha256((SRC/n).read_bytes()).hexdigest() for n in ['MC2_Live.ino','signal_core.h','recorder_ui.h']},'success':ran.returncode==0,'stdout':ran.stdout,'stderr':ran.stderr,'compiler_flags':cmd[1:6],'limitations':['Not real LittleFS or WAV writer validation; separate record_store tests apply','No real ADC, GPIO, storage timing or rendered physical LCD checks']}
        (ROOT/'tests/app-integration-verification.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n')
        raise SystemExit(ran.returncode)
if __name__=='__main__': main()
