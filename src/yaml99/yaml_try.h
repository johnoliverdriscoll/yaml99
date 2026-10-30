#ifndef YAML_TRY_H
#define YAML_TRY_H

#include <yaml99/yaml.h>

#define YAML_TRY(call)                          \
  do {                                          \
    int yaml_try_result_ = (call);              \
    if (yaml_try_result_ != YAML_OK) {          \
      return yaml_try_result_;                  \
    }                                           \
  } while(0)

#endif /* YAML_TRY_H */
