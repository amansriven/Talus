#include "util/crc32c.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace talus::crc32c {
namespace {

struct Impl {
  const char* name;
  internal::ExtendFn fn;
};

// Every implementation available on this machine. The dispatched entry point
// is always included; the hardware path is included when the CPU has it.
std::vector<Impl> AvailableImpls() {
  std::vector<Impl> impls = {{"software", &internal::ExtendSoftware}, {"dispatch", &Extend}};
  if (internal::ExtendFn hw = internal::HardwareExtend()) {
    impls.push_back({"hardware", hw});
  }
  return impls;
}

uint32_t Crc(internal::ExtendFn fn, const std::string& s) { return fn(0, s.data(), s.size()); }

// Test vectors from RFC 3720, appendix B.4.
TEST(Crc32cTest, Rfc3720Vectors) {
  std::string zeros(32, '\0');
  std::string ones(32, '\xff');
  std::string ascending(32, '\0');
  std::string descending(32, '\0');
  for (int i = 0; i < 32; ++i) {
    ascending[i] = static_cast<char>(i);
    descending[i] = static_cast<char>(31 - i);
  }
  // An iSCSI SCSI Read (10) command PDU.
  const unsigned char read_pdu[48] = {
      0x01, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00,
      0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x18, 0x28, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  };
  const std::string pdu(reinterpret_cast<const char*>(read_pdu), sizeof(read_pdu));

  for (const Impl& impl : AvailableImpls()) {
    SCOPED_TRACE(impl.name);
    EXPECT_EQ(Crc(impl.fn, zeros), 0x8a9136aau);
    EXPECT_EQ(Crc(impl.fn, ones), 0x62a8ab43u);
    EXPECT_EQ(Crc(impl.fn, ascending), 0x46dd794eu);
    EXPECT_EQ(Crc(impl.fn, descending), 0x113fdb5cu);
    EXPECT_EQ(Crc(impl.fn, pdu), 0xd9963a56u);
    EXPECT_EQ(Crc(impl.fn, "123456789"), 0xe3069283u);
    EXPECT_EQ(Crc(impl.fn, ""), 0u);
  }
}

// The hardware paths have separate head/body/tail loops and alignment
// handling, so compare every implementation against the software one over
// all small lengths at every starting alignment.
TEST(Crc32cTest, ImplementationsAgreeOnAllLengthsAndAlignments) {
  std::mt19937 rng(301);
  std::string buf(1024 + 16, '\0');
  for (char& c : buf) {
    c = static_cast<char>(rng());
  }
  const auto impls = AvailableImpls();
  for (size_t offset = 0; offset < 16; ++offset) {
    for (size_t len = 0; len <= 1024; ++len) {
      const char* p = buf.data() + offset;
      const uint32_t expected = internal::ExtendSoftware(0, p, len);
      for (const Impl& impl : impls) {
        ASSERT_EQ(impl.fn(0, p, len), expected)
            << impl.name << " offset=" << offset << " len=" << len;
      }
    }
  }
}

TEST(Crc32cTest, ExtendComposes) {
  const std::string a = "hello ";
  const std::string b = "world, this is a longer tail to cross the 8-byte loop";
  for (const Impl& impl : AvailableImpls()) {
    SCOPED_TRACE(impl.name);
    const uint32_t whole = Crc(impl.fn, a + b);
    const uint32_t split = impl.fn(Crc(impl.fn, a), b.data(), b.size());
    EXPECT_EQ(whole, split);
  }
}

TEST(Crc32cTest, DifferentInputsDiffer) {
  EXPECT_NE(Value("a", 1), Value("foo", 3));
  EXPECT_NE(Value("foo", 3), Value("bar", 3));
}

TEST(Crc32cTest, MaskRoundTripsAndChangesValue) {
  const uint32_t crc = Value("foo", 3);
  EXPECT_NE(crc, Mask(crc));
  EXPECT_NE(crc, Mask(Mask(crc)));
  EXPECT_EQ(crc, Unmask(Mask(crc)));
  EXPECT_EQ(crc, Unmask(Unmask(Mask(Mask(crc)))));
}

TEST(Crc32cTest, ReportsImplementation) {
  const std::string name = ImplementationName();
  if (IsHardwareAccelerated()) {
    EXPECT_TRUE(name == "sse4.2" || name == "armv8-crc") << name;
  } else {
    EXPECT_EQ(name, "software");
  }
#if defined(__APPLE__) && defined(__aarch64__)
  // Every Apple Silicon CPU has the CRC extension.
  EXPECT_TRUE(IsHardwareAccelerated());
#endif
}

}  // namespace
}  // namespace talus::crc32c
