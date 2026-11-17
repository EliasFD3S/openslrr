#include "save_io_fixture.hpp"
#include "slrr/script_compiler.hpp"
#include "slrr/tree_eval.hpp"
#include <limits>
#include <climits>
using namespace slrr;
int main() {
  Fixture f;
  fs::create_directories(f.root/"probe/src");
  std::ofstream(f.root/"probe/src/Numbers.java") << R"JAVA(
package probe;
public class Numbers {
  static int money;
  public static int add(int a,int b){return a+b;}
  public static int sub(int a,int b){return a-b;}
  public static int mul(int a,int b){return a*b;}
  public static int div(int a,int b){return a/b;}
  public static int mod(int a,int b){return a%b;}
  public static float fmod(float a,float b){return a%b;}
  public static int neg(int a){return -a;}
  public static int inc(int a){++a;return a;}
  public static int dec(int a){--a;return a;}
  public static float mixed(int a,float b){return a+b;}
  public static int neq(float a,float b){return a!=b;}
  public static int cast(float a){return (int)a;}
  public static int charge(int a,int fee){money=a;money-=fee;return money;}
  public static int cmul(int a,int b){a*=b;return a;}
  public static int cdiv(int a,int b){a/=b;return a;}
  public static int cmod(int a,int b){a%=b;return a;}
  public static int cadd(int a,int b){a+=b;return a;}
  public static int fractional(int a,float fee){money=a;money-=fee;return money;}
}
)JAVA";
  auto report=script_compile_all(f.root.string(),"probe");
  if(report.failed || !report.succeeded){std::puts("FAIL fixture compilation");return 2;}
  fs::path compiled;
  for(const auto& entry:fs::recursive_directory_iterator(f.root))
    if(entry.path().extension()==".class")compiled=entry.path();
  if(compiled.empty() || !f.vm.load_class_file(compiled.string()))return 2;
  int failed=0;
  auto check=[&](const char* method,const char* desc,std::vector<Value> args,int32_t expected){
    auto v=f.vm.invoke("probe.Numbers",method,desc,args,true);
    bool ok=v.tag==ValueTag::Int && v.v.i==expected;
    std::printf("%s %s expected=%d actual=%d tag=%d\n",ok?"PASS":"FAIL",method,expected,v.tag==ValueTag::Int?v.v.i:0,int(v.tag));
    std::fflush(stdout);failed+=!ok;
  };
  auto I=[](int32_t n){return Value::make_int(n);};
  auto F=[](float n){return Value::make_float(n);};
  check("add","(II)I",{I(16777216),I(1)},16777217);
  check("sub","(II)I",{I(16777219),I(2)},16777217);
  check("mul","(II)I",{I(5592407),I(3)},16777221);
  check("add","(II)I",{I(INT_MAX),I(1)},INT_MIN);
  check("sub","(II)I",{I(INT_MIN),I(1)},INT_MAX);
  check("mul","(II)I",{I(INT_MAX),I(2)},-2);
  check("neg","(I)I",{I(INT_MIN)},INT_MIN);
  check("inc","(I)I",{I(INT_MAX)},INT_MIN);
  check("dec","(I)I",{I(INT_MIN)},INT_MAX);
  check("charge","(II)I",{I(16877217),I(100000)},16777217);
  check("charge","(II)I",{I(100000),I(100000)},0);
  check("cmul","(II)I",{I(5592407),I(3)},16777221);
  check("cdiv","(II)I",{I(16777217),I(1)},16777217);
  check("cmod","(II)I",{I(16777217),I(2)},1);
  check("cadd","(II)I",{I(16777216),I(1)},16777217);
  check("fractional","(IF)I",{I(100),F(32.377f)},67);
  auto balance=f.vm.tree_get_static("probe.Numbers","money");
  if(balance.tag!=ValueTag::Int || balance.v.i!=67){std::puts("FAIL integer money storage");++failed;}
  check("cast","(F)I",{F(std::numeric_limits<float>::quiet_NaN())},0);
  check("cast","(F)I",{F(std::numeric_limits<float>::infinity())},INT_MAX);
  check("cast","(F)I",{F(-std::numeric_limits<float>::infinity())},INT_MIN);
  check("cast","(F)I",{F(-3.9f)},-3);
  check("div","(II)I",{I(7),I(0)},0);
  check("mod","(II)I",{I(-7),I(3)},-1);
  auto rem=f.vm.invoke("probe.Numbers","fmod","(FF)F",{F(7.5f),F(2.f)},true);
  if(rem.tag!=ValueTag::Float || rem.v.f!=1.5f){std::puts("FAIL float remainder");++failed;}
  auto mixed=f.vm.invoke("probe.Numbers","mixed","(IF)F",{I(3),F(.5f)},true);
  if(mixed.tag!=ValueTag::Float || mixed.v.f!=3.5f){std::puts("FAIL mixed promotion");++failed;}
  check("neq","(FF)I",{F(1.f),F(2.f)},0); // MWM compatibility quirk retained.
  // Run trapping edge cases only after all other failures were recorded.
  if(!failed){
    const auto* loaded=f.vm.find_class("probe.Numbers");
    TufaClass cls=*loaded;
    TufaMember method;
    for(const auto& m:cls.static_methods)if(cls.utf8(m.name)=="add")method=m;
    for(int tag:{15,16,17,18,19}) {
      cls.trees[method.tree]={{0x07,0,true,uint32_t(tag)},{0x01,0,true,1},{0x01,0,true,0},{0x28,0,false,0},{0x2b,0,false,0}};
      const int expected=tag==15?16777216:tag==16?16777218:tag==17?0:16777217;
      auto out=tree_eval(&f.vm,cls,method,nullptr,{I(16777217),I(1)});
      bool ok=out.ok && out.value.tag==ValueTag::Int && out.value.v.i==expected;
      std::printf("%s value-dispatch tag=%d\n",ok?"PASS":"FAIL",tag);failed+=!ok;
    }
    check("div","(II)I",{I(INT_MIN),I(-1)},INT_MIN);
    check("mod","(II)I",{I(INT_MIN),I(-1)},0);
  }
  return failed?1:0;
}
