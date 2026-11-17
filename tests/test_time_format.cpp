#include "slrr/ui_time.hpp"
#include <cstdio>
#include <limits>
int main(){struct Case{float seconds;int flags;const char* expected;};
 Case cases[]={{79.8356f,2,"1:19.835"},{79.8354f,2,"1:19.835"},{0,2,"0:00.000"},{1.2f,2,"0:01.200"},{59.9996f,2,"0:59.999"},{3599.9996f,0,"00:59:59.999"},{59.9996f,1,"00:00"},{13*3600+4*60,33,"1:04pm"},{12*3600,33,"12:00pm"},{0,33,"12:00am"},{1.23456f,6,"1.234"},{std::numeric_limits<float>::quiet_NaN(),2,"0:00.000"}};
 int failed=0;for(auto& c:cases){auto actual=slrr::format_game_time(c.seconds,c.flags);bool ok=actual==c.expected;std::printf("flags=%d expected=%s actual=%s %s\n",c.flags,c.expected,actual.c_str(),ok?"PASS":"FAIL");if(!ok)++failed;}std::printf("time_format %d passed, %d failed\n",int(std::size(cases))-failed,failed);return failed?1:0;}
