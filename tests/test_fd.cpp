// Unit tests for the Fd RAII wrapper: a descriptor is closed exactly once, by
// whichever Fd owns it last.
#include "fd.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <utility>

#include <gtest/gtest.h>

namespace {

bool isOpen(int fd) { return ::fcntl(fd, F_GETFD) != -1 || errno != EBADF; }

struct Pipe {
    int readEnd = -1;
    int writeEnd = -1;
    Pipe() {
        int fds[2];
        if (::pipe(fds) == 0) {
            readEnd = fds[0];
            writeEnd = fds[1];
        }
    }
};

}  // namespace

TEST(Fd, ClosesDescriptorOnDestruction) {
    Pipe p;
    ASSERT_GE(p.readEnd, 0);
    {
        Fd r(p.readEnd);
        Fd w(p.writeEnd);
        EXPECT_TRUE(isOpen(p.readEnd));
    }
    EXPECT_FALSE(isOpen(p.readEnd));
    EXPECT_FALSE(isOpen(p.writeEnd));
}

TEST(Fd, MoveConstructionTransfersOwnership) {
    Pipe p;
    ASSERT_GE(p.readEnd, 0);
    Fd w(p.writeEnd);
    {
        Fd a(p.readEnd);
        Fd b(std::move(a));
        EXPECT_FALSE(a.valid());  // NOLINT(bugprone-use-after-move): checking the moved-from state is the point
        EXPECT_EQ(b.get(), p.readEnd);
        EXPECT_TRUE(isOpen(p.readEnd));
    }
    EXPECT_FALSE(isOpen(p.readEnd));
}

TEST(Fd, MoveAssignmentClosesThePreviousDescriptor) {
    Pipe first;
    Pipe second;
    ASSERT_GE(first.readEnd, 0);
    ASSERT_GE(second.readEnd, 0);
    Fd firstWrite(first.writeEnd);
    Fd secondWrite(second.writeEnd);

    Fd a(first.readEnd);
    Fd b(second.readEnd);
    a = std::move(b);

    EXPECT_FALSE(isOpen(first.readEnd));  // a's old descriptor was released
    EXPECT_EQ(a.get(), second.readEnd);
    EXPECT_FALSE(b.valid());  // NOLINT(bugprone-use-after-move)
    EXPECT_TRUE(isOpen(second.readEnd));
}
