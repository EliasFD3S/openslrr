#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace slrr {

// One row of city.rest. The payload is "1,<index>".
struct RestSpot {
  int area = 0;
  int index = 0;
  int flag = 0;
  float box[6]{};
};

bool rest_read(std::string_view text, std::vector<RestSpot>* out, std::string* error);

}  // namespace slrr
