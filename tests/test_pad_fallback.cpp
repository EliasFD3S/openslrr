#include "standalone_test.hpp"
#include "../src/platform/host_input_mapping.hpp"
#include <array>
using namespace slrr;
int main() {
 ControllerInput input;
 std::array<float,24> pad{};
 std::array<float,256> keyboard{};
 auto physical=[&](int device,int index){
   if(index<0)return 0.f;
   if(device==2 && index<int(pad.size()))return pad[index];
   if(device==0 && index<int(keyboard.size()))return keyboard[index];
   return 0.f;
 };
 auto axis=[&](int id){return input_mapping::mapped_axis(input,id,physical,pad);};
 auto analog=[&](){return input_mapping::steering_is_analog(input,physical,pad);};
 auto binding=[](int logical,int device,int channel,float low=0,float high=1){return InputBinding{logical,device,channel,0,1,low,high};};
 pad[0]=.6f;pad[9]=.7f;pad[8]=.4f;pad[21]=1;
 auto defaults=[&](){SLRR_REQUIRE(axis(1)==.6f && axis(28)==.7f && axis(29)==.4f && axis(48)==1 && analog());};
 defaults();
 input.bindings={binding(54,2,20)};pad[20]=1;defaults();SLRR_REQUIRE(axis(54)==1);
 input.bindings={binding(78,2,23)};defaults();
 // Explicit pad steering overrides the stick; other driving defaults survive.
 input.bindings={binding(1,2,12)};
 SLRR_REQUIRE(axis(1)==0 && !analog() && axis(28)==.7f);
 pad[12]=1;SLRR_REQUIRE(axis(1)==1 && !analog());pad[12]=0;
 // Claiming RT for another action suppresses RT's throttle fallback only.
 input.bindings={binding(54,2,9)};
 SLRR_REQUIRE(axis(28)==0 && axis(54)==.7f && axis(29)==.4f && axis(1)==.6f);
 input.bindings={binding(28,2,20)};pad[20]=.25f;
 SLRR_REQUIRE(axis(28)==.25f && axis(29)==.4f);
 input.bindings={binding(54,2,21)};
 SLRR_REQUIRE(axis(48)==0 && axis(54)==1 && axis(28)==.7f);
 // Steering halves are claimed independently.
 input.bindings={binding(4,2,0)};
 SLRR_REQUIRE(axis(1)==0 && !analog());
 pad[1]=.3f;SLRR_REQUIRE(axis(1)==-.3f && analog());
 input.bindings.push_back(binding(5,2,1));SLRR_REQUIRE(axis(1)==0 && !analog());
 // Keyboard overrides fallback steering and keeps the digital steering ramp.
 input.bindings={binding(1,0,30)};keyboard[30]=1;
 SLRR_REQUIRE(axis(1)==1 && !analog());
 keyboard[30]=0;SLRR_REQUIRE(axis(1)==pad[0]-pad[1] && analog());
 // Same numeric physical index on keyboard must not claim a pad channel.
 input.bindings={binding(54,0,9)};SLRR_REQUIRE(axis(28)==.7f);
 // Explicit analog remaps retain their transfer function and analog flag.
 input.bindings={binding(1,2,4,0,-1)};pad[4]=.8f;
 SLRR_REQUIRE(axis(1)==-.8f && analog());
 SLRR_REQUIRE(!input_mapping::pad_fallback(input,100,pad));
 input.bindings.clear();
 std::array<float,1> short_pad{.5f};
 SLRR_REQUIRE(input_mapping::pad_fallback(input,1,short_pad)==.5f);
 SLRR_REQUIRE(input_mapping::pad_fallback(input,28,short_pad)==0);
 std::puts("PASS pad defaults, extra horn/camera buttons, per-axis overrides, physical conflicts, half-stick claims, keyboard priority and analog classification");
}
