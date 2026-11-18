#include "standalone_test.hpp"
#include "slrr/gameref.hpp"
#include "slrr/jvm.hpp"
using namespace slrr;
namespace {
struct Fixture { Object target, alias;Fixture(){gameref_upsert(&target,GameRefKind::PhysicsRef);gameref_upsert(&alias);gameref_bind_alias(&alias,&target);}~Fixture(){gameref_remove(&alias);gameref_remove(&target);} };
SLRR_TEST(mask){Fixture f;gameref_set_event_mask(&f.alias,3);SLRR_REQUIRE(gameref_get(&f.target)->event_mask==3);SLRR_REQUIRE(gameref_get(&f.target)->kind==GameRefKind::PhysicsRef);gameref_clear_event_mask(&f.alias,1);SLRR_REQUIRE(gameref_get(&f.target)->event_mask==2);}
SLRR_TEST(timer){Fixture f;gameref_add_timer(&f.alias,10,77);SLRR_REQUIRE(gameref_get(&f.target)->timers.size()==1);auto due=gameref_poll_timers(10);SLRR_REQUIRE(due.size()==1&&due[0].key==&f.target&&due[0].timer_id==77);SLRR_REQUIRE(gameref_poll_timers(10).empty());gameref_add_timer(&f.alias,20,88);gameref_remove_all_timers(&f.alias);SLRR_REQUIRE(gameref_poll_timers(30).empty());}
SLRR_TEST(unbound){Object ref;gameref_set_event_mask(&ref,4);gameref_add_timer(&ref,100,2);SLRR_REQUIRE(gameref_get(&ref)->kind==GameRefKind::GameType);SLRR_REQUIRE(gameref_get(&ref)->event_mask==4);SLRR_REQUIRE(gameref_get(&ref)->timers.size()==1);gameref_remove(&ref);}
}
int main(){const standalone::Test tests[]={mask,timer,unbound};return standalone::run("gameref_alias",tests);}
