#include "fsstate.h"
#include "guest.h"
#include "ncurses_ui.h"
#include "cereal/archives/binary.hpp"
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <sstream>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
extern "C" detsim::ui::NCursesUI* get_ncurses_ui() { return nullptr; }
static void require(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
static bool collide=false;
extern "C" unsigned long long __real_XXH3_64bits(const void*,size_t);
extern "C" unsigned long long __wrap_XXH3_64bits(const void* bytes,size_t length) {
  return collide?7:__real_XXH3_64bits(bytes,length);
}
static uint64_t number(const char* bytes) {uint64_t value;memcpy(&value,bytes,8);return value;}
static long native_get(int fd,void* pointer,unsigned count) {
  errno=0;long result=syscall(SYS_getdents64,fd,pointer,count);return result<0?-errno:result;
}
int main() {
  try {
    auto manager=std::make_shared<FdManager>(); FileSystemState fs(manager);
    fs.ensure_dir_exists("/dir/sub");
    for(const char* path:{"/dir/.hidden","/dir/a","/dir/sub/inside"}) {
      VFSNode node{};node.metadata.st_mode=S_IFREG|0600;fs.filesystem[path]=node;
    }
    VFSNode link{};link.metadata.st_mode=S_IFLNK|0777;link.content={'a'};fs.filesystem["/dir/link"]=link;
    int fd=fs.do_open("/dir",O_RDONLY|O_DIRECTORY,0);
    int independent=fs.do_open("/dir",O_RDONLY|O_DIRECTORY,0);
    char output[256];memset(output,0x5a,sizeof(output));
    auto get=[&](void* p,unsigned count){return fs.do_getdents64(fd,p,count,getpid());};
    require(get(nullptr,0)==-EINVAL,"empty count with remaining entry");
    require(get(nullptr,0x80000000u)==-EINVAL,"signed callback count");
    require(get(nullptr,23)==-EINVAL,"short record before pointer validation");
    require(get(output,24)==24 && !strcmp(output+19,".") && number(output+8)==1,"dot and ordinal cookie");
    require(output[21]==0x5a && output[22]==0x5a && output[23]==0x5a,"untouched record padding");
    struct stat st{};require(fs.do_fstat(fd,&st)==0 && st.st_ino==number(output),"dot fstat inode agreement");
    require(fs.do_getdents64(independent,output,24,getpid())==24 && number(output+8)==1,"independent open cursor");
    FileSystemState branch=fs;
    std::stringstream archive;{cereal::BinaryOutputArchive ar(archive);ar(fs);}
    FileSystemState restored;{cereal::BinaryInputArchive ar(archive);ar(restored);} restored.set_fd_manager(manager);
    require(get(output,24)==24 && !strcmp(output+19,".."),"dotdot entry");
    require(fs.do_stat("/",&st)==0 && st.st_ino==number(output),"dotdot parent inode");
    char replay[256];require(restored.do_getdents64(fd,replay,24,getpid())==24 && !memcmp(output,replay,21),"archive cursor replay");
    require(branch.do_getdents64(fd,replay,24,getpid())==24 && branch.open_files.at(fd).offset==2,"branch cursor isolation");
    const long bytes=get(output,sizeof(output));
    require(bytes>0,"remaining directory bytes");
    std::vector<std::string> names;size_t position=0;
    while(position<size_t(bytes)) {
      uint16_t length;memcpy(&length,output+position+16,2);
      require(length>=24 && length%8==0 && position+length<=size_t(bytes),"record alignment and bounds");
      const auto terminator=static_cast<const char*>(memchr(output+position+19,0,length-19));
      require(terminator,"bounded name terminator");
      names.emplace_back(static_cast<const char*>(output+position+19),terminator);
      require(number(output+position)!=0 && number(output+position+8)==names.size()+2,"nonzero inode and next cookie");
      require(fs.do_stat("/dir/"+names.back(),&st,false)==0 && st.st_ino==number(output+position),"child nofollow stat inode agreement");
      if(names.back()=="link")require(static_cast<unsigned char>(output[position+18])==DT_LNK,"symlink dirent type");
      position+=length;
    }
    require(names==std::vector<std::string>({".hidden","a","link","sub"}),"lexical immediate children exclude grandchildren");
    require(get(nullptr,0)==0 && get(nullptr,0xffffffffu)==0,"EOF ignores pointer and count");
    require(fs.do_lseek(fd,0,SEEK_END)==-EINVAL && fs.do_lseek(fd,-1,SEEK_SET)==-EINVAL,"directory seek errors");
    require(fs.do_lseek(fd,0,SEEK_SET)==0 && fs.do_lseek(fd,1,SEEK_CUR)==1,"directory seek cookies");
    bool scratch_rejected=false;try{get(reinterpret_cast<void*>(scratch_page),24);}catch(const std::exception&){scratch_rejected=true;}
    require(scratch_rejected && fs.open_files.at(fd).offset==1,"scratch overlap rejects before cursor mutation");
    require(fs.do_stat("/",&st)==0,"root identity");
    const auto inode=st.st_ino;fs.do_open("/new",O_CREAT|O_RDWR,0600);require(fs.do_stat("/",&st)==0 && st.st_ino==inode,"creation keeps existing inode");
    int regular=fs.do_open("/dir/a",O_RDWR,0);fs.do_fstat(regular,&st);auto old=st.st_ino;
    fs.do_open("/dir/a",O_WRONLY|O_TRUNC,0);require(fs.do_fstat(regular,&st)==0 && st.st_ino==old,"truncation keeps inode");
    require(fs.do_getdents64(regular,output,256,getpid())==-ENOTDIR && fs.do_getdents64(-1,output,256,getpid())==-EBADF,"descriptor errors");
    collide=true;bool rejected=false;try{fs.do_stat("/",&st);}catch(const std::exception&){rejected=true;}collide=false;
    require(rejected,"all-path inode collision explicitly rejected");
    auto malformed_rejected=[&](FileSystemState& invalid,int directory_fd) {
      char untouched[64];memset(untouched,0x5a,sizeof(untouched));
      const auto cursor=invalid.open_files.at(directory_fd).offset;
      bool failed=false;try{invalid.do_getdents64(directory_fd,untouched,sizeof(untouched),getpid());}
      catch(const std::exception&){failed=true;}
      require(failed && invalid.open_files.at(directory_fd).offset==cursor,"invalid namespace rejects before directory cursor mutation");
      for(char byte:untouched)require(byte==0x5a,"invalid namespace rejects before any dot or child output");
    };
    for(size_t length:{size_t(256),size_t(65536)}) {
      auto invalid=fs;invalid.do_lseek(fd,0,SEEK_SET);
      invalid.ensure_dir_exists("/dir/"+std::string(length,'x'));
      malformed_rejected(invalid,fd);
    }
    {
      auto invalid=fs;invalid.do_lseek(fd,0,SEEK_SET);
      invalid.filesystem[std::string("/dir/bad\0name",13)]=VFSNode{};
      malformed_rejected(invalid,fd);
    }
    char* pages=static_cast<char*>(mmap(nullptr,8192,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    require(pages!=MAP_FAILED && !mprotect(pages+4096,4096,PROT_NONE),"fault fixture");
    char directory[]="/tmp/detsim-getdents-native-XXXXXX";require(mkdtemp(directory),"native namespace fixture");
    for(size_t length:{size_t(256),size_t(65536)}) {
      FileSystemState invalid(std::make_shared<FdManager>());
      invalid.init_from_mappings({{directory,"/"+std::string(length,'x')}});
      const int root=invalid.do_open("/",O_RDONLY|O_DIRECTORY,0);
      malformed_rejected(invalid,root);
    }
    int native=open(directory,O_RDONLY|O_DIRECTORY);require(native>=0,"native directory open");
    require(native_get(-1,nullptr,0)==-EBADF,"native bad descriptor priority");
    for(unsigned count:{0u,23u,0x80000000u,0xffffffffu}) {
      lseek(native,0,SEEK_SET);require(native_get(native,nullptr,count)==-EINVAL,"native count before pointer");
    }
    for(int available=0;available<=72;++available) {
      char* buffer=pages+4096-available;memset(pages,0x5a,4096);fs.do_lseek(fd,0,SEEK_SET);
      long result=get(buffer,48);
      long expected=available<21?-EFAULT:available<46?24:48;
      require(result==expected,"protected-page record fault matrix");
      require(fs.open_files.at(fd).offset==(available<21?0:available<46?1:2),"fault cursor matrix");
      if(available<16) for(int i=0;i<available;++i) require(buffer[i]==0x5a,"first cross-page d_off remains atomic");
      if(available>=24 && available<32) for(int i=24;i<available;++i) require(buffer[i]==0x5a,"later cross-page inode remains atomic");
      if(available==41) require(buffer[40]==0x5a,"cross-page reclen remains atomic");
      std::vector<char> projected(buffer,buffer+available);
      memset(pages,0x5a,4096);lseek(native,0,SEEK_SET);
      require(native_get(native,buffer,48)==expected,"native protected-page raw return oracle");
      for(int i=0;i<available;++i) {
        // Inodes and filesystem cookies deliberately use the virtual profile;
        // all other bytes, including failed-record prefixes/padding, match.
        if(i%24<16) continue;
        require(projected[i]==buffer[i],"native partial byte and padding oracle");
      }
    }
    require(native_get(native,nullptr,0)==0,"native EOF ignores invalid pointer");
    close(native);
    const auto hostfile=std::string(directory)+"/saved";
    int host=open(hostfile.c_str(),O_CREAT|O_WRONLY,0600);require(host>=0,"captured host file");close(host);
    FileSystemState captured(std::make_shared<FdManager>());
    captured.init_from_mappings({{directory,"/capture"}});
    require(unlink(hostfile.c_str())==0 && rmdir(directory)==0,"remove captured host namespace");
    int captured_fd=captured.do_open("/capture",O_RDONLY|O_DIRECTORY,0);
    require(captured.do_getdents64(captured_fd,output,sizeof(output),getpid())==80 &&
            !strcmp(output+48+19,"saved"),"listing uses capture after host deletion");
    munmap(pages,8192);
    puts("PASS getdents captured namespace, inode identity, archive, padding and fault matrix");return 0;
  }catch(const std::exception& error){fprintf(stderr,"FAIL %s\n",error.what());return 1;}
}
