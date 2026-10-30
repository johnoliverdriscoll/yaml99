#include <stdlib.h>
#include <string.h>
#include <yaml99/yaml.h>
#include <sio99/sio_buffer_reader.h>

int
main(void) {
  const char text[] = "- hello\n- \"a\\0b\"\n- ''\n";
  SioBufferReader reader;
  sio_buffer_reader_init(&reader, text, sizeof(text) - 1);
  YamlParser parser;
  YamlValue root, child;
  if (yaml_parser_init((SioSeekableReader*) &reader, 8, &parser, &root)
    || yaml_value_enter_container(&root, &child)) {
    return 1;
  }
  char* string = NULL;
  if (yaml_value_get_cstring(&child, &string) || strcmp(string, "hello") != 0) {
    return 1;
  }
  free(string);
  void* bytes = NULL;
  size_t size = 0;
  if (yaml_value_get_bytes(&child, &bytes, &size)
    || size != 3 || memcmp(bytes, "a\0b", 3) != 0) {
    return 1;
  }
  free(bytes);
  if (yaml_value_get_bytes(&child, &bytes, &size) || bytes != NULL || size != 0) {
    return 1;
  }
  if (yaml_value_leave_container(&root, &child)) {
    return 1;
  }
  const char nul[] = "\"a\\0b\"";
  sio_buffer_reader_init(&reader, nul, sizeof(nul) - 1);
  if (yaml_parser_init((SioSeekableReader*) &reader, 8, &parser, &root)) {
    return 1;
  }
  if (yaml_value_get_cstring(&root, &string) != YAML_ERROR_INVALID || string != NULL) {
    return 1;
  }
  return 0;
}
