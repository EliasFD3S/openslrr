#include "save_io_fixture.hpp"
#include "slrr/script_compiler.hpp"
#include "slrr/tree_eval.hpp"
#include <cstring>
using namespace slrr;
uint32_t u32(const std::vector<uint8_t>& b,size_t p){return uint32_t(b[p])|(uint32_t(b[p+1])<<8)|(uint32_t(b[p+2])<<16)|(uint32_t(b[p+3])<<24);}
void put(std::vector<uint8_t>& b,size_t p,uint32_t v){for(int i=0;i<4;++i)b[p+i]=uint8_t(v>>(8*i));}
int main(int argc,char** argv) {
  if(argc>1 && std::string(argv[1])=="--corpus") {
    size_t files=0,failed=0,classes=0;uint32_t maxlocals=0;
    for(const auto& entry:fs::recursive_directory_iterator("game")) {
      if(entry.path().extension()!=".class")continue;
      ++files;std::vector<TufaClass> blobs;std::string error;
      if(!tufa_load_file(entry.path().string(),&blobs,&error)) {
        ++failed;if(failed<=12)std::printf("REJECT %s: %s\n",entry.path().string().c_str(),error.c_str());
      } else {classes+=blobs.size();for(const auto& c:blobs)for(const auto& m:c.static_methods)maxlocals=std::max(maxlocals,m.locals);}
    }
    std::printf("CORPUS files=%zu accepted_classes=%zu rejected_files=%zu max_static_locals=%u\n",files,classes,failed,maxlocals);
    return 0; // Compare rejection set with baseline, not an assumed pristine install.
  }
  Fixture f;fs::create_directories(f.root/"probe/src");
  std::ofstream(f.root/"probe/src/Validation.java")<<R"JAVA(package probe;
public class Validation {
 static int seed=1234;
 static int other=seed+1;
 public static int run(int a){int value=a+1;return value;}
}
class Peer {}
)JAVA";
  auto report=script_compile_all(f.root.string(),"probe");SLRR_REQUIRE(report.succeeded==1&&!report.failed);
  auto path=f.root/"probe/Validation.class";
  std::ifstream in(path,std::ios::binary|std::ios::ate);auto n=in.tellg();in.seekg(0);std::vector<uint8_t> bytes(size_t(n),0);in.read(reinterpret_cast<char*>(bytes.data()),n);in.close();
  std::vector<TufaClass> blobs;std::string error;SLRR_REQUIRE(tufa_parse(bytes.data(),bytes.size(),&blobs,&error)&&blobs.size()==2);
  std::unordered_map<std::string,size_t> chunks;
  for(size_t p=12;p+8<=bytes.size();) {
    std::string tag(reinterpret_cast<const char*>(bytes.data()+p),4);chunks[tag]=p+8;
    const auto size=u32(bytes,p+4);p+=8+size;if(tag=="TREE")break;
  }
  auto reject=[&](std::vector<uint8_t> broken,const char* label){
    std::vector<TufaClass> out;std::string err;
    SLRR_REQUIRE(!tufa_parse(broken.data(),broken.size(),&out,&err));
    SLRR_REQUIRE(out.empty()&&!err.empty());std::printf("PASS reject %s: %s\n",label,err.c_str());
  };
  for(const char* name:{"CONS","FILD","MTHD","TREE"}){auto bad=bytes;put(bad,chunks.at(name),UINT32_MAX);reject(std::move(bad),name);}
  {auto bad=bytes;put(bad,chunks.at("CLSS")+16,UINT32_MAX);reject(std::move(bad),"interfaces");}
  {auto bad=bytes;put(bad,chunks.at("TREE")+4,UINT32_MAX);reject(std::move(bad),"node count");}
  {auto bad=bytes;put(bad,chunks.at("MTHD")+20,UINT32_MAX);reject(std::move(bad),"method locals");}
  size_t declaration=0,literal=0;
  size_t p=chunks.at("TREE");auto trees=u32(bytes,p);p+=4;
  for(uint32_t t=0;t<trees;++t){auto count=u32(bytes,p);p+=4;for(uint32_t i=0;i<count;++i){auto op=bytes[p];bool wide=(kTreeOpFlags[op]>>12)!=0;if(op==2)declaration=p;if(op==0x0b&&!literal)literal=p;p+=wide?7:3;}}
  SLRR_REQUIRE(declaration&&literal);
  for(uint32_t slot:{65536u,0x7fffffffu,0xffffffffu}){auto bad=bytes;put(bad,declaration+3,slot);reject(std::move(bad),"declare slot");}
  auto bad=bytes;bad[literal]=0x09;put(bad,literal+3,UINT32_MAX);reject(bad,"static string constant");
  {auto rid=bytes;rid[literal]=0x0e;put(rid,literal+3,UINT32_MAX);reject(rid,"static RID constant");}
  auto broken=f.root/"broken.class";std::ofstream output(broken,std::ios::binary);output.write(reinterpret_cast<char*>(bad.data()),bad.size());output.close();SLRR_REQUIRE(!f.vm.load_class_file(broken.string()));
  SLRR_REQUIRE(f.vm.load_class_file(path.string()));
  auto result=f.vm.invoke("probe.Validation","run","(I)I",{Value::make_int(4)},true);SLRR_REQUIRE(result.tag==ValueTag::Int&&result.v.i==5);
  TufaClass memory=blobs[0];TufaMember method;for(const auto& m:memory.static_methods)if(memory.utf8(m.name)=="run")method=m;
  memory.trees[method.tree]={{0x02,0,true,UINT32_MAX}};
  auto rejected=tree_eval(&f.vm,memory,method,nullptr,{});SLRR_REQUIRE(!rejected.ok&&rejected.error.find("local")!=std::string::npos);
  method.locals=UINT32_MAX;rejected=tree_eval(&f.vm,memory,method,nullptr,{});SLRR_REQUIRE(!rejected.ok);
  auto tail=bytes;tail.push_back(0);reject(tail,"invalid trailing blob (atomic container)");
  std::puts("PASS valid concatenated classes, clean VM rejection and runtime local guard");
  return 0;
}
