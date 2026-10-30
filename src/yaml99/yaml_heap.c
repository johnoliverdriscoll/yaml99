#include <stdlib.h>
#include <yaml99/yaml.h>
#include <sio99/sio_dynamic_buffer_writer.h>
#include "yaml_assert.h"
#include "yaml_try.h"
#include "yaml_require.h"

int
yaml_value_get_bytes(
  YamlValue* it,
  void** out_bytes,
  size_t* out_size
) {
  YAML_ASSERT_NE(out_bytes, NULL);
  YAML_ASSERT_NE(out_size, NULL);

  *out_bytes = NULL;
  *out_size = 0;

  SioDynamicBufferWriter writer;
  sio_dynamic_buffer_writer_init(&writer);

  int res;

  res = yaml_value_read_string(it, (SioWriter*) &writer);
  if (res != YAML_OK) {
    sio_dynamic_buffer_writer_deinit(&writer);
    YAML_TRY(res);
  }

  size_t size = sio_dynamic_buffer_writer_get_size(&writer);

  if (size == 0) {
    sio_dynamic_buffer_writer_deinit(&writer);
    return YAML_OK;
  }

  *out_bytes = malloc(size);
  if (*out_bytes == NULL) {
    sio_dynamic_buffer_writer_deinit(&writer);
  }
  YAML_REQUIRE_NE(*out_bytes, NULL, YAML_ERROR_NO_MEMORY);

  sio_dynamic_buffer_writer_copy_bytes(&writer, *out_bytes);

  *out_size = size;

  sio_dynamic_buffer_writer_deinit(&writer);
  return YAML_OK;
}

int
yaml_value_get_cstring(
  YamlValue* it,
  char** out_cstring
) {
  YAML_ASSERT_NE(out_cstring, NULL);

  *out_cstring = NULL;

  SioDynamicBufferWriter writer;
  sio_dynamic_buffer_writer_init(&writer);
  int res;

  res = yaml_value_read_string(it, (SioWriter*) &writer);
  if (res != YAML_OK) {
    sio_dynamic_buffer_writer_deinit(&writer);
    YAML_TRY(res);
  }

  size_t size = sio_dynamic_buffer_writer_get_size(&writer);

  if (size == SIZE_MAX) {
    sio_dynamic_buffer_writer_deinit(&writer);
  }
  YAML_REQUIRE_NE(size, SIZE_MAX, YAML_ERROR_NO_MEMORY);

  *out_cstring = malloc(size + 1);
  if (*out_cstring == NULL) {
    sio_dynamic_buffer_writer_deinit(&writer);
  }
  YAML_REQUIRE_NE(*out_cstring, NULL, YAML_ERROR_NO_MEMORY);

  res = sio_dynamic_buffer_writer_copy_cstring(
    &writer,
    *out_cstring
  );
  if (res != SIO_OK) {
    sio_dynamic_buffer_writer_deinit(&writer);
    free(*out_cstring);
    *out_cstring = NULL;
  }
  YAML_REQUIRE_NE(res, SIO_ERROR_INVALID, YAML_ERROR_INVALID);
  YAML_REQUIRE_EQ(res, SIO_OK, YAML_ERROR_IO);

  sio_dynamic_buffer_writer_deinit(&writer);
  return YAML_OK;
}
