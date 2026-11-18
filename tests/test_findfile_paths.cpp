#include "save_io_fixture.hpp"
#include <winioctl.h>
#include <cstring>
using namespace slrr;
struct FindFixture:Fixture {
 fs::path game;
 Object finder,pattern;
 FindFixture():game(root/"game"){fs::create_directories(game);vm.set_game_root(game.string());}
 std::vector<std::string> list(const std::string& mask,int flags=0){
  pattern.str=mask;
  auto value=registry.invoke("java.io.FindFile","first","(Ljava.lang.String;I)Ljava.lang.String;",&finder,{Value::make_obj(&pattern),Value::make_int(flags)});
  std::vector<std::string> names;
  while(value.tag==ValueTag::Obj && value.v.o){names.push_back(value.v.o->str);value=registry.invoke("java.io.FindFile","next","()Ljava.lang.String;",&finder,{});}
  return names;
 }
};
// NTFS junctions do not require the symbolic-link privilege. Targets and links
// in these fixtures are all under the one owned test container. Remove only the
// reparse directory before the fixture's ordinary recursive cleanup.
struct Junction {
 fs::path link;
 Junction(const fs::path& path,const fs::path& target):link(path){
  fs::create_directory(link);
  const auto print=fs::absolute(target).wstring(),sub=L"\\??\\"+print;
  const size_t path_bytes=(sub.size()+1+print.size()+1)*sizeof(wchar_t);
  std::vector<uint8_t> data(16+path_bytes,0);
  auto u16=[&](size_t off,uint16_t v){std::memcpy(data.data()+off,&v,2);};
  uint32_t tag=IO_REPARSE_TAG_MOUNT_POINT;std::memcpy(data.data(),&tag,4);
  u16(4,uint16_t(8+path_bytes));u16(8,0);u16(10,uint16_t(sub.size()*2));
  u16(12,uint16_t((sub.size()+1)*2));u16(14,uint16_t(print.size()*2));
  std::memcpy(data.data()+16,sub.c_str(),(sub.size()+1)*2);
  std::memcpy(data.data()+16+(sub.size()+1)*2,print.c_str(),(print.size()+1)*2);
  HANDLE handle=CreateFileW(link.c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,
                            FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr);
  SLRR_REQUIRE(handle!=INVALID_HANDLE_VALUE);
  DWORD bytes=0;const BOOL ok=DeviceIoControl(handle,FSCTL_SET_REPARSE_POINT,data.data(),DWORD(data.size()),nullptr,0,&bytes,nullptr);
  CloseHandle(handle);SLRR_REQUIRE(ok);
 }
 ~Junction(){RemoveDirectoryW(link.c_str());}
};
void root_and_glob_contract(){
 FindFixture f;f.sentinel(f.game/"Z.txt");f.sentinel(f.game/"a.TXT");f.sentinel(f.game/"plain");fs::create_directories(f.game/"folder");
 SLRR_REQUIRE(f.list("*",1)==std::vector<std::string>({"a.TXT","plain","Z.txt"}));
 SLRR_REQUIRE(f.list("*.*",1)==f.list("*",1));
 SLRR_REQUIRE(f.list("*.txt",1)==std::vector<std::string>({"a.TXT","Z.txt"}));
 SLRR_REQUIRE(f.list("*",2)==std::vector<std::string>({"folder"}));
 SLRR_REQUIRE(under_game(f.game.string(),".").empty());
 std::puts("PASS root enumeration, masks, sorting, flags and file root protection");
}
void leading_separators_use_game_directory(){
 FindFixture f;f.sentinel(f.game/"save/main");
 for(const char* mask:{"save/*","save\\*","/save/*","\\save\\*"})
  SLRR_REQUIRE(f.list(mask,1)==std::vector<std::string>({"main"}));
 std::puts("PASS leading separators resolve to the same game-relative directory as File");
}
void escape_patterns_clear_previous_results(){
 FindFixture f;f.sentinel(f.game/"inside");f.sentinel(f.root/"outside/marker");
 const auto absolute=(f.root/"outside").string()+"\\*";
 for(const std::string& mask:std::vector<std::string>{"../outside/*","save/../../outside/*",absolute,"C:relative*",std::string("*\0extra",7)}){
  SLRR_REQUIRE(!f.list("*",1).empty());SLRR_REQUIRE(f.list(mask,1).empty());
  SLRR_REQUIRE(f.finder.find_entries.empty() && f.finder.find_index==0);
 }
 SLRR_REQUIRE(fs::exists(f.root/"outside/marker"));
 std::puts("PASS traversal/absolute/invalid patterns are rejected and old enumeration state is cleared");
}
void dotted_names_and_delete_wrapper(){
 FindFixture f;f.sentinel(f.game/"save/J..R-1/main");f.sentinel(f.game/"save/J..R-1/keep.txt");
 auto names=f.list("\\save\\J..R-1\\*",1);SLRR_REQUIRE(names.size()==2);
 for(const auto& name:names)SLRR_REQUIRE(f.erase("\\save\\J..R-1\\"+name)==1);
 SLRR_REQUIRE(f.list("save/J..R-1/*",1).empty());SLRR_REQUIRE(f.erase("save/J..R-1")==1);
 std::puts("PASS dotted careers enumerate and delete-by-mask targets the same files");
}
void junctions_obey_physical_root(){
 FindFixture f;f.sentinel(f.game/"save/main");f.sentinel(f.root/"game-other/outside");
 Junction inside(f.game/"inside",f.game/"save"),outside(f.game/"escape",f.root/"game-other");
 SLRR_REQUIRE(f.list("inside/*",1)==std::vector<std::string>({"main"}));
 SLRR_REQUIRE(!under_game(f.game.string(),"inside/main").empty());
 SLRR_REQUIRE(f.list("escape/*",1).empty());SLRR_REQUIRE(under_game(f.game.string(),"escape/outside").empty());
 SLRR_REQUIRE(f.erase("escape/outside")==0 && fs::exists(f.root/"game-other/outside"));
 std::puts("PASS internal NTFS junction allowed, external same-prefix sibling junction rejected");
}
int main(){const standalone::Test tests[]={root_and_glob_contract,leading_separators_use_game_directory,escape_patterns_clear_previous_results,dotted_names_and_delete_wrapper,junctions_obey_physical_root};return standalone::run("findfile_paths",tests);}
