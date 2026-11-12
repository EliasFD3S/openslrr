#include "slrr/format/rest.hpp"

#include <cmath>
#include <string>
#include <unordered_set>

namespace slrr {
namespace {

void fail(std::string* error, const char* text) {
  if (error && error->empty()) *error = text;
}

}  // namespace

bool rest_read(std::string_view text, std::vector<RestSpot>* out, std::string* error) {
  if (!out) return false;
  out->clear();
  if (error) error->clear();
  if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xef &&
      static_cast<unsigned char>(text[1]) == 0xbb &&
      static_cast<unsigned char>(text[2]) == 0xbf)
    text.remove_prefix(3);
  std::unordered_set<std::string> keys;
  size_t cursor = 0;
  while (cursor < text.size()) {
    size_t end = text.find('\n', cursor);
    if (end == std::string_view::npos) end = text.size();
    std::string_view line = text.substr(cursor, end - cursor);
    cursor = end + 1;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
      line.remove_suffix(1);
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
      line.remove_prefix(1);
    if (line.empty()) continue;
    const size_t semi = line.find(';');
    const size_t space = line.rfind(' ', semi == std::string_view::npos ? line.size() : semi);
    if (semi == std::string_view::npos || space == std::string_view::npos || space + 1 >= semi) {
      fail(error, "Invalid pedestrian line header");
      out->clear();
      return false;
    }
    RestSpot spot;
    const std::string_view box_text = line.substr(0, space);
    size_t at = 0;
    for (int i = 0; i < 6; ++i) {
      size_t stop = 0;
      try {
        spot.box[i] = std::stof(std::string(box_text.substr(at)), &stop);
      } catch (...) {
        fail(error, "Invalid pedestrian bounds");
        out->clear();
        return false;
      }
      if (!std::isfinite(spot.box[i])) {
        fail(error, "Invalid pedestrian bounds");
        out->clear();
        return false;
      }
      at += stop;
      if (i < 5) {
        if (at >= box_text.size() || box_text[at] != ',') {
          fail(error, "Invalid pedestrian bounds");
          out->clear();
          return false;
        }
        ++at;
      }
    }
    if (at != box_text.size() || spot.box[0] > spot.box[3] || spot.box[2] > spot.box[5]) {
      fail(error, "Invalid pedestrian bounds");
      out->clear();
      return false;
    }
    try {
      spot.area = std::stoi(std::string(line.substr(space + 1, semi - space - 1)));
    } catch (...) {
      fail(error, "Invalid pedestrian record ID");
      out->clear();
      return false;
    }
    if (spot.area < 1) {
      fail(error, "Invalid pedestrian record ID");
      out->clear();
      return false;
    }
    const std::string payload(line.substr(semi + 1));
    const size_t comma = payload.find(',');
    if (comma == std::string::npos) {
      fail(error, "Invalid pedestrian rest payload");
      out->clear();
      return false;
    }
    try {
      spot.flag = std::stoi(payload.substr(0, comma));
      spot.index = std::stoi(payload.substr(comma + 1));
    } catch (...) {
      fail(error, "Invalid pedestrian rest payload");
      out->clear();
      return false;
    }
    if (payload.find(',', comma + 1) != std::string::npos || spot.flag != 1 || spot.index < 0) {
      fail(error, "Invalid pedestrian rest payload");
      out->clear();
      return false;
    }
    const std::string key = std::to_string(spot.area) + ':' + std::to_string(spot.index);
    if (!keys.insert(key).second) {
      fail(error, "Duplicate pedestrian rest index");
      out->clear();
      return false;
    }
    out->push_back(spot);
  }
  return true;
}

}  // namespace slrr
