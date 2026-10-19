#include "Talus/status.h"

#include <gtest/gtest.h>

#include <utility>

namespace talus {
namespace {

TEST(StatusTest, DefaultIsOk) {
  Status s;
  EXPECT_TRUE(s.ok());
  EXPECT_EQ(s.code(), Status::Code::kOk);
  EXPECT_TRUE(s.message().empty());
  EXPECT_EQ(s.ToString(), "OK");
}

TEST(StatusTest, FactoriesSetCode) {
  EXPECT_TRUE(Status::NotFound().IsNotFound());
  EXPECT_TRUE(Status::Corruption().IsCorruption());
  EXPECT_TRUE(Status::IOError().IsIOError());
  EXPECT_TRUE(Status::InvalidArgument().IsInvalidArgument());
  EXPECT_TRUE(Status::Busy().IsBusy());
  EXPECT_FALSE(Status::NotFound().ok());
}

TEST(StatusTest, MessagesAreJoined) {
  Status s = Status::Corruption("bad block", "000012.sst");
  EXPECT_EQ(s.message(), "bad block: 000012.sst");
  EXPECT_EQ(s.ToString(), "Corruption: bad block: 000012.sst");
  EXPECT_EQ(Status::IOError("disk full").ToString(), "IOError: disk full");
  EXPECT_EQ(Status::NotFound().ToString(), "NotFound");
}

TEST(StatusTest, CopyIsDeepAndMoveTransfers) {
  Status original = Status::InvalidArgument("x");
  Status copy = original;
  EXPECT_EQ(copy.ToString(), original.ToString());
  EXPECT_NE(copy.message().data(), original.message().data());

  Status assigned;
  assigned = copy;
  EXPECT_TRUE(assigned.IsInvalidArgument());
  assigned = Status::OK();
  EXPECT_TRUE(assigned.ok());
  EXPECT_TRUE(assigned.message().empty());

  Status moved = std::move(copy);
  EXPECT_EQ(moved.message(), "x");
}

}  // namespace
}  // namespace talus
