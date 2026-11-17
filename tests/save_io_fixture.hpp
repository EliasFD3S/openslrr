#pragma once
#include "standalone_test.hpp"
#include "natives_internal.hpp"
#include "slrr/natives_boot.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace {
namespace fs = std::filesystem;
struct Fixture {
  fs::path base, root;
  slrr::NativeRegistry registry;
  slrr::Jvm vm{&registry};
  slrr::RpkIndex packs;
  slrr::SectionMachine sections{&vm};
  slrr::Object file, name;
  Fixture() {
    static unsigned serial = 0;
    base = fs::absolute("build/save-io-fixtures");
    root = base / ("test-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(++serial));
    SLRR_REQUIRE(!fs::exists(root));
    fs::create_directories(root);
    vm.set_game_root(root.string());
    packs.set_root(root.string());
    slrr::register_io_natives(vm, packs, sections);
    file.fields["name"] = slrr::Value::make_obj(&name);
  }
  ~Fixture() {
    slrr::g_host_files.erase(&file);
    // Cleanup is restricted to this process's isolated test directory.
    if (root.parent_path() == base && root.filename().string().starts_with("test-")) {
      std::error_code ec;
      fs::remove_all(root, ec);
    }
  }
  slrr::Value invoke(const char* method, const char* signature,
                     const std::vector<slrr::Value>& args = {}, slrr::Object* receiver = nullptr) {
    return registry.invoke("java.io.File", method, signature, receiver, args);
  }
  int erase(const std::string& relative) {
    slrr::Object path; path.str = relative;
    return invoke("delete", "(Ljava.lang.String;)I", {slrr::Value::make_obj(&path)}).v.i;
  }
  int open(const std::string& relative, int mode) {
    name.str = relative;
    return invoke("open", "(I)I", {slrr::Value::make_int(mode)}, &file).v.i;
  }
  int write_int(int value) {return invoke("write", "(I)I", {slrr::Value::make_int(value)}, &file).v.i;}
  void close() {invoke("close", "()V", {}, &file);}
  int read_int() {return invoke("readInt", "()I", {}, &file).v.i;}
  int read_resource() {return invoke("readResID", "()I", {}, &file).v.i;}
  int write_resource(int32_t id, int restype = 0) {
    slrr::Object resource;
    resource.fields["id"] = slrr::Value::make_int(id);
    resource.fields["typeof"] = slrr::Value::make_int(restype);
    return invoke("write", "(Ljava.util.resource.ResourceRef;)I",
                  {slrr::Value::make_obj(&resource)}, &file).v.i;
  }
  void sentinel(const fs::path& path) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary); out << "keep";
    SLRR_REQUIRE(bool(out));
  }
};
} // namespace
