#include "../searcher.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

TEST(CorpusSearchTest, IdSetFollowedBy)
{
    auto a = idset::from_vec({{2, 3}, {2, 4}, {2, 5}, {5, 1}, {6, 1}, {6, 2}, {7, 1}});
    auto b = idset::from_vec({{2, 1}, {2, 3}, {2, 4}, {5, 2}, {6, 3}, {7, 0}});

    auto c = a.followed_by(b);

    ASSERT_TRUE(!c.is_all());
    ASSERT_EQ(c.size(), 3);
    ASSERT_EQ(c.data.value()[0].sent_id, 2);
    ASSERT_EQ(c.data.value()[0].pos, 3);
    ASSERT_EQ(c.data.value()[1].sent_id, 5);
    ASSERT_EQ(c.data.value()[1].pos, 1);
    ASSERT_EQ(c.data.value()[2].sent_id, 6);
    ASSERT_EQ(c.data.value()[2].pos, 2);
}
