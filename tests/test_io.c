#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <yaml99/yaml.h>

#define CHECK(condition) do {                                           \
    if (!(condition)) {                                                 \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);   \
      exit(1);                                                          \
    }                                                                   \
} while (0)

/* Inject a failure at each individual callback in an otherwise valid parse. */
typedef struct Reader {
  SioSeekableReader reader;
  const char* text;
  size_t size;
  size_t position;
  size_t calls;
  size_t fail;
  int error;
} Reader;

static int
operation(
  Reader* reader
) {
  reader->calls++;
  if (reader->calls == reader->fail) {
    return reader->error;
  }
  return SIO_OK;
}

static int
read_byte(
  void* user,
  uint8_t* out
) {
  Reader* reader = user;
  int result = operation(reader);
  if (result != SIO_OK) {
    return result;
  }
  if (reader->position == reader->size) {
    return SIO_ERROR_EOF;
  }
  if (out != NULL) {
    *out = (uint8_t) reader->text[reader->position];
  }
  reader->position++;
  return SIO_OK;
}

static int
peek_byte(
  void* user,
  uint8_t* out,
  bool* at_end
) {
  Reader* reader = user;
  int result = operation(reader);
  if (result != SIO_OK) {
    return result;
  }
  bool end = reader->position == reader->size;
  if (at_end != NULL) {
    *at_end = end;
  }
  if (!end && out != NULL) {
    *out = (uint8_t) reader->text[reader->position];
  }
  return SIO_OK;
}

static int
tell_position(
  void* user,
  uint64_t* out
) {
  Reader* reader = user;
  int result = operation(reader);
  if (result == SIO_OK) {
    *out = reader->position;
  }
  return result;
}

static int
seek_position(
  void* user,
  uint64_t position
) {
  Reader* reader = user;
  int result = operation(reader);
  if (result != SIO_OK) {
    return result;
  }
  CHECK(position <= reader->size);
  reader->position = (size_t) position;
  return SIO_OK;
}

static int
run(
  Reader* reader
) {
  YamlParser parser;
  YamlValue root;
  int result = yaml_parser_init((SioSeekableReader*) reader, 8, &parser, &root);
  if (result != YAML_OK) {
    return result;
  }
  return yaml_value_advance(&root);
}

typedef struct Writer {
  size_t count;
  size_t fail;
  int error;
} Writer;

static int
write_byte(
  void* user,
  uint8_t c
) {
  Writer* writer = user;
  (void) c;
  bool fail = writer->count == writer->fail;
  writer->count++;
  if (fail) {
    return writer->error;
  }
  return SIO_OK;
}


static size_t
map_operation_count(
  unsigned entries
) {
  char text[64 * 8 + 1];
  CHECK(entries <= 64);
  for (unsigned i = 0; i < entries; i++) {
    CHECK(snprintf(text + i * 8, 9, "k%03u: v\n", i) == 8);
  }
  Reader reader = { .text = text, .size = entries * 8 };
  reader.reader = (SioSeekableReader) {
    .reader = { .user = &reader, .read = read_byte, .peek = peek_byte },
    .tell = tell_position,
    .seek = seek_position,
  };
  CHECK(run(&reader) == YAML_OK);
  return reader.calls;
}

int
main(void) {
  // Doubling a flat map must not quadruple reader work by rescanning prior keys.
  size_t small = map_operation_count(32);
  size_t large = map_operation_count(64);
  CHECK(large > small);
  CHECK(large < 3 * small);
  const char* text = "a: \"hello\\u03b1\"\r\nb:\n - |\n   world\n";
  Reader reader = {
    .text = text,
    .size = strlen(text),
    .error = 123,
  };
  reader.reader = (SioSeekableReader) {
    .reader = { .user = &reader, .read = read_byte, .peek = peek_byte },
    .tell = tell_position,
    .seek = seek_position,
  };
  CHECK(run(&reader) == YAML_OK);
  size_t calls = reader.calls;
  for (size_t fail = 1; fail <= calls; fail++) {
    reader.position = 0;
    reader.calls = 0;
    reader.fail = fail;
    CHECK(run(&reader) == YAML_ERROR_IO);
  }
  reader.text = "\"hello\\u03b1\"";
  reader.size = strlen(reader.text);
  reader.fail = 0;
  const int errors[] = { SIO_ERROR_EOF, SIO_ERROR_INVALID, SIO_ERROR_NO_MEMORY, 123 };
  for (size_t e = 0; e < sizeof(errors) / sizeof(*errors); e++) {
    for (size_t fail = 0; fail < 7; fail++) {
      reader.position = 0;
      YamlParser parser;
      YamlValue root;
      CHECK(yaml_parser_init(
        (SioSeekableReader*) &reader,
        8,
        &parser,
        &root
      ) == YAML_OK);
      Writer sink = { .fail = fail, .error = errors[e] };
      SioWriter writer = { .user = &sink, .write = write_byte };
      CHECK(yaml_value_read_string(&root, &writer) == YAML_ERROR_IO);
    }
  }
  return 0;
}
