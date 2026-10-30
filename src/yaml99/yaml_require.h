#ifndef YAML_REQUIRE_H
#define YAML_REQUIRE_H

#define YAML_REQUIRE(cond, val) \
  do { \
    if (!(cond)) { \
      return (val); \
    } \
  } while (0)

#define YAML_REQUIRE_EQ(a, b, val) \
  YAML_REQUIRE((a) == (b), val)

#define YAML_REQUIRE_NE(a, b, val) \
  YAML_REQUIRE((a) != (b), val)

#define YAML_REQUIRE_LT(a, b, val) \
  YAML_REQUIRE((a) < (b), val)

#define YAML_REQUIRE_LTE(a, b, val) \
  YAML_REQUIRE((a) <= (b), val)

#define YAML_REQUIRE_GT(a, b, val) \
  YAML_REQUIRE((a) > (b), val)

#define YAML_REQUIRE_GTE(a, b, val) \
  YAML_REQUIRE((a) >= (b), val)

#endif /* YAML_REQUIRE_H */
