#ifndef YAML_ASSERT_H
#define YAML_ASSERT_H

#include <assert.h>

#define YAML_ASSERT(cond) assert(cond)

#define YAML_ASSERT_EQ(a, b) YAML_ASSERT((a) == (b))

#define YAML_ASSERT_NE(a, b) YAML_ASSERT((a) != (b))

#define YAML_ASSERT_LT(a, b) YAML_ASSERT((a) < (b))

#define YAML_ASSERT_LTE(a, b) YAML_ASSERT((a) <= (b))

#define YAML_ASSERT_GT(a, b) YAML_ASSERT((a) > (b))

#define YAML_ASSERT_GTE(a, b) YAML_ASSERT((a) >= (b))

#endif /* YAML_ASSERT_H */
