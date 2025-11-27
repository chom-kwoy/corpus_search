#include "searcher.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

TEST(CorpusSearchTest, IdSetFollowedBy)
{
    auto a = candset::from_vec({{2, 3}, {2, 4}, {2, 5}, {5, 1}, {6, 1}, {6, 2}, {7, 1}});
    auto b = candset::from_vec({{2, 1}, {2, 3}, {2, 4}, {5, 2}, {6, 3}, {7, 0}});

    auto c = a.followed_by(b);

    ASSERT_TRUE(!c.is_all());
    ASSERT_EQ(c.size(), 3);

    auto v = c.sent_ids();

    ASSERT_EQ(v[0], 2);
    ASSERT_EQ(v[1], 5);
    ASSERT_EQ(v[2], 6);
}
