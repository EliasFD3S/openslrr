#include "save_io_fixture.hpp"
#include "slrr/gameref.hpp"
#include <chrono>
using namespace slrr;
int main() {
 gameref_clear();
 Object first,alias,other;
 SLRR_REQUIRE(!gameref_find_live(0) && !gameref_find_live(0x70000000));
 auto* original=gameref_upsert(&first);
 const auto id=gameref_make_live(&first);
 SLRR_REQUIRE(id==0x70000000 && gameref_make_live(&first)==id && gameref_find_live(id)==original);
 const auto alias_id=gameref_make_live(&alias);
 SLRR_REQUIRE(gameref_bind_alias(&alias,&first));
 SLRR_REQUIRE(!gameref_find_live(alias_id) && gameref_make_live(&alias)==id);
 // Some constructor paths detach wrappers by clearing binding on their own entry.
 gameref_upsert(&alias)->binding=nullptr;
 SLRR_REQUIRE(gameref_find_live(alias_id)->key==&alias);
 SLRR_REQUIRE(gameref_bind_alias(&alias,&first));
 gameref_remove(&alias);
 SLRR_REQUIRE(gameref_find_live(id)==original && !gameref_find_live(alias_id));
 std::vector<std::unique_ptr<Object>> objects;
 std::vector<int32_t> ids;
 for(int i=0;i<8000;++i){
   objects.push_back(std::make_unique<Object>());
   ids.push_back(gameref_make_live(objects.back().get()));
 }
 SLRR_REQUIRE(gameref_find_live(id)==original);
 auto start=std::chrono::steady_clock::now();
 for(int i=0;i<100000;++i){const auto n=size_t(i)%ids.size();SLRR_REQUIRE(gameref_find_live(ids[n])->key==objects[n].get());}
 std::printf("PASS 100000 indexed live lookups / 8000 nodes: %lld us (informational)\n",
   (long long)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count());
 const auto gone=ids[4000];gameref_remove(objects[4000].get());
 SLRR_REQUIRE(!gameref_find_live(gone));
 const auto reused=gameref_make_live(objects[4000].get());
 SLRR_REQUIRE(reused!=gone && gameref_find_live(reused)->key==objects[4000].get());
 SLRR_REQUIRE(gameref_bind_alias(&other,&first));
 gameref_remove(&first);
 SLRR_REQUIRE(!gameref_find_live(id) && !gameref_get(&other));
 gameref_clear();
 SLRR_REQUIRE(!gameref_find_live(reused));
 SLRR_REQUIRE(gameref_make_live(&first)==0x70000000);
 gameref_clear();
 std::puts("PASS live IDs, hash rehash stability, alias detach/removal, canonical removal, key reuse and clear");
}
