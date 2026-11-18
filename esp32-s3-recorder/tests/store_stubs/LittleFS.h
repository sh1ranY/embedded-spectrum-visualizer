#pragma once
#include <string>
#include <map>
#include <vector>
#include <memory>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <limits>
struct MockBlob { std::vector<uint8_t> data; };
struct MockCursor {std::string path;std::shared_ptr<MockBlob> blob;size_t at=0;bool directory=false,writable=false;std::vector<std::string> children;size_t child=0;};
class File;
class MockLittleFS {
 public:
  std::map<std::string,std::shared_ptr<MockBlob>> files;
  bool mountOk=true,renameOk=true,removeOk=true,openOk=true;
  size_t capacity=0x180000;int formatCalls=0,mountCalls=0,writeCalls=0,openWriteCalls=0;int64_t writeBudget=-1,truncateOnClose=-1;
  bool begin(bool formatOnFail,const char*,uint8_t,const char*){++mountCalls;if(formatOnFail)++formatCalls;return mountOk;}
  bool format(){++formatCalls;files.clear();mountOk=true;return true;}
  bool exists(const char* path)const{return files.count(path);}
  File open(const char* path,const char* mode="r");
  bool rename(const char* a,const char* b){if(!renameOk||!files.count(a)||files.count(b))return false;files[b]=files[a];files.erase(a);return true;}
  bool remove(const char* p){return removeOk&&files.erase(p)==1;}
  size_t totalBytes(){return capacity;}
  size_t usedBytes(){size_t n=0;for(auto& [p,b]:files)n+=b->data.size()+4096;return n;}
};
inline MockLittleFS LittleFS;
class File {
 std::shared_ptr<MockCursor> c_;
 public:
 File()=default;explicit File(std::shared_ptr<MockCursor> c):c_(c){}
 explicit operator bool()const{return bool(c_);}
 bool isDirectory()const{return c_&&c_->directory;}
 const char* name()const{return c_?c_->path.c_str():"";}
 size_t size()const{return c_&&c_->blob?c_->blob->data.size():0;}
 size_t write(const uint8_t* p,size_t n){++LittleFS.writeCalls;if(!c_||!c_->writable)return 0;size_t k=LittleFS.writeBudget<0?n:std::min(n,size_t(LittleFS.writeBudget));if(LittleFS.writeBudget>=0)LittleFS.writeBudget-=k;auto& v=c_->blob->data;v.resize(std::max(v.size(),c_->at+k));std::memcpy(v.data()+c_->at,p,k);c_->at+=k;return k;}
 size_t read(uint8_t* p,size_t n){if(!c_||!c_->blob)return 0;auto& v=c_->blob->data;size_t k=std::min(n,v.size()-c_->at);std::memcpy(p,v.data()+c_->at,k);c_->at+=k;return k;}
 bool seek(size_t at){if(!c_||!c_->blob||at>c_->blob->data.size())return false;c_->at=at;return true;}
 void flush(){}void close(){if(c_&&c_->writable&&LittleFS.truncateOnClose>=0){c_->blob->data.resize(size_t(LittleFS.truncateOnClose));LittleFS.truncateOnClose=-1;}c_.reset();}
 File openNextFile(){if(!isDirectory()||c_->child>=c_->children.size())return {};return LittleFS.open(c_->children[c_->child++].c_str());}
};
inline File MockLittleFS::open(const char* path,const char* mode){
 if(!openOk)return {};auto c=std::make_shared<MockCursor>();c->path=path;
 if(c->path=="/"){c->directory=true;for(auto& [p,b]:files)c->children.push_back(p);return File(c);}
 c->writable=mode&&mode[0]=='w';
 if(c->writable){++openWriteCalls;files[c->path]=std::make_shared<MockBlob>();}else if(!files.count(c->path))return {};
 c->blob=files[c->path];return File(c);
}
