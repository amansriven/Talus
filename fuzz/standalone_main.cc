// A main() for building the fuzz targets without libFuzzer (any compiler).
// Every file named on the command line, or found in a named directory, goes
// through LLVMFuzzerTestOneInput once. CTest uses this to run each target
// over its seed corpus as a regression test in every build configuration.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

namespace {

void RunFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  const std::vector<char> bytes((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
  LLVMFuzzerTestOneInput(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
}

}  // namespace

int main(int argc, char** argv) {
  size_t inputs = 0;
  for (int i = 1; i < argc; ++i) {
    const std::filesystem::path arg = argv[i];
    if (std::filesystem::is_directory(arg)) {
      for (const auto& entry : std::filesystem::directory_iterator(arg)) {
        RunFile(entry.path());
        ++inputs;
      }
    } else {
      RunFile(arg);
      ++inputs;
    }
  }
  std::printf("ran %zu inputs\n", inputs);
  return inputs > 0 ? 0 : 1;
}
