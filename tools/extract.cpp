// Usage: extract <gameDir> <outDir> <substring>... ; with outDir "-" just lists matches
#include "CascLib.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <sys/stat.h>
static void mkdirs(std::string p){ for(size_t i=1;i<p.size();i++) if(p[i]=='/'){ p[i]=0; mkdir(p.c_str(),0755); p[i]='/'; } }
int main(int argc,char**argv){
  if(argc<4){fprintf(stderr,"usage\n");return 1;}
  HANDLE st; if(!CascOpenStorage(argv[1],0,&st)){fprintf(stderr,"open failed %d\n",GetCascError());return 2;}
  CASC_FIND_DATA fd; HANDLE f=CascFindFirstFile(st,"*",&fd,NULL); if(!f){fprintf(stderr,"find failed\n");return 3;}
  bool list=!strcmp(argv[2],"-"); long n=0,bytes=0;
  do{ std::string name=fd.szFileName; for(auto&c:name) if(c=='\\') c='/';
    bool hit=false; for(int i=3;i<argc;i++) if(name.find(argv[i])!=std::string::npos) hit=true;
    if(!hit||!fd.bFileAvailable) continue;
    if(list){ printf("%s\n",name.c_str()); n++; continue; }
    std::string rel=name; size_t c=rel.find(':'); if(c!=std::string::npos) rel=rel.substr(c+1);
    HANDLE h; if(!CascOpenFile(st,fd.szFileName,0,CASC_OPEN_BY_NAME,&h)){fprintf(stderr,"skip %s\n",name.c_str());continue;}
    std::string out=std::string(argv[2])+"/"+rel; mkdirs(out); FILE*o=fopen(out.c_str(),"wb");
    char buf[65536]; DWORD r; while(CascReadFile(h,buf,sizeof buf,&r)&&r){fwrite(buf,1,r,o);bytes+=r;} fclose(o); CascCloseFile(h); n++;
  }while(CascFindNextFile(f,&fd));
  CascFindClose(f); CascCloseStorage(st); fprintf(stderr,"%ld files, %ld bytes\n",n,bytes); return 0; }
