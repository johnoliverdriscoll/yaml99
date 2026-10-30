#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <yaml99/yaml.h>
#include <sio99/sio_buffer_reader.h>
#include <sio99/sio_buffer_writer.h>

#define CHECK(condition) do {                                           \
    if (!(condition)) {                                                 \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);   \
      exit(1);                                                          \
    }                                                                   \
  } while (0)
#define OK(call) CHECK((call) == YAML_OK)

static void
init(
  const char* text,
  SioBufferReader* reader,
  YamlParser* parser,
  YamlValue* root
) {
  sio_buffer_reader_init(reader, text, strlen(text));
  OK(yaml_parser_init((SioSeekableReader*) reader, 32, parser, root));
}

static void
string(
  YamlValue* it,
  const char* expected
) {
  uint8_t output[4096];
  SioBufferWriter writer;
  sio_buffer_writer_init(&writer, output, sizeof(output));
  CHECK(yaml_value_is_string(it));
  int result = yaml_value_read_string(it, (SioWriter*) &writer);
  if (result != YAML_OK) {
    fprintf(stderr, "string error %d expected [%s] at %llu\n", result,
      expected, (unsigned long long) it->parser->mark.position);
  }
  OK(result);
  size_t length = sio_buffer_writer_get_size(&writer);
  if (length != strlen(expected) || memcmp(output, expected, length) != 0) {
    fprintf(stderr, "expected [%s], got [%.*s] (%zu bytes)\n", expected,
      (int) length, output, length);
    exit(1);
  }
}

static void
test_scalars(void) {
  const char* cases[][2] = {
    { "", "" },
    { " \t# comment\r\n\t \n", "" },
    { "hello # comment\n", "hello" },
    { "true", "true" },
    { "123", "123" },
    { "null", "null" },
    { "https://example.com/a#b", "https://example.com/a#b" },
    { "'it''s fine'", "it's fine" },
    { "\"\\x41\\u03b1\\U0001f600\"", "A\xce\xb1\xf0\x9f\x98\x80" },
    { "\"a\\t b\\n c\"", "a\t b\n c" },
    { "\"a  \n  b\"", "a b" },
    { "'a\n\n b'", "a\nb" },
    { "\"a\\\n   b\"", "ab" },
    { "one\n  two\n\n  three\n", "one two\nthree" },
    { "|\n  one\n  two\n", "one\ntwo\n" },
    { "|-\n  one\n\n", "one" },
    { "|+\n  one\n\n", "one\n\n" },
    { ">\n  one\n  two\n\n  three\n", "one two\nthree\n" },
    { ">\n  one\n    two\n  three\n", "one\n  two\nthree\n" },
    { "|2\n    Indented\n  Not indented\n", "  Indented\nNot indented\n" },
    { "|\n\n  # text\n\n  last", "\n# text\n\nlast" },
    { "|\n\n", "" },
    { "|\ntext\n...\n", "text\n" },
    { "|+\n\n", "\n" },
    { "---\nhello\n...\n# end\n", "hello" },
    { "\xef\xbb\xbfhello", "hello" },
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
    SioBufferReader reader;
    YamlParser parser;
    YamlValue root;
    init(cases[i][0], &reader, &parser, &root);
    string(&root, cases[i][1]);
    CHECK(yaml_value_at_end(&root));
    OK(yaml_value_advance(&root));
  }
}

static void
test_containers(void) {
  const char* text =
    "one: 1\n  \t# Comment\n\t \n"
    "two:\n  2\n"
    "empty:\n"
    "array:\n"
    "- first\n"
    "- name: second\n"
    "  list:\n"
    "    - x\n"
    "    - y\n"
    "last: |2\n"
    "    Indented\n"
    "  Not indented\n";
  SioBufferReader reader;
  YamlParser parser;
  YamlValue root, map, array, nested, list;
  init(text, &reader, &parser, &root);
  CHECK(yaml_value_is_map(&root));
  OK(yaml_value_enter_container(&root, &map));
  string(&map, "one");
  string(&map, "1");
  string(&map, "two");
  string(&map, "2");
  string(&map, "empty");
  string(&map, "");
  string(&map, "array");
  CHECK(yaml_value_is_array(&map));
  OK(yaml_value_enter_container(&map, &array));
  string(&array, "first");
  OK(yaml_value_enter_container(&array, &nested));
  string(&nested, "name");
  string(&nested, "second");
  string(&nested, "list");
  OK(yaml_value_enter_container(&nested, &list));
  string(&list, "x");
  // Leaving validates and skips the remaining y entry.
  OK(yaml_value_leave_container(&nested, &list));
  CHECK(yaml_value_at_end(&nested));
  OK(yaml_value_leave_container(&array, &nested));
  CHECK(yaml_value_at_end(&array));
  OK(yaml_value_leave_container(&map, &array));
  string(&map, "last");
  string(&map, "  Indented\nNot indented\n");
  CHECK(yaml_value_at_end(&map));
  OK(yaml_value_leave_container(&root, &map));
  CHECK(yaml_value_at_end(&root));
}

static void
test_invalid(void) {
  const char* cases[] = {
    "x: [a, b]", "{x: y}", "x: &a y", "x: *a", "x: !!str y",
    "x: \"\\q\"", "x: \"\\uD800\"", "x: \"\\U00110000\"",
    "x: '\xff'", "x: |\n  \xc0\xaf\n", "# \xff\n",
    "x:\n\tvalue\n", "x: |\n   three\n  two\n",
    "x: |0\n  a\n", "x: |22\n  a\n", "x: |+-\n  a\n",
    "x: |\n    \n  a\n", "x: a: b", "x: \"a\" garbage",
    "x: '\nnext: value", "x: a\ny", "x: a\n---\ny: b\n",
    "a: \"\001\"", "a: value\n \tbad: entry\n",
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
    SioBufferReader reader;
    sio_buffer_reader_init(&reader, cases[i], strlen(cases[i]));
    YamlParser parser;
    YamlValue root;
    int result = yaml_parser_init(
      (SioSeekableReader*) &reader,
      32,
      &parser,
      &root
    );
    if (result == YAML_OK) {
      result = yaml_value_advance(&root);
    }
    if (result == YAML_OK) {
      fprintf(stderr, "accepted invalid input: %s\n", cases[i]);
      exit(1);
    }
    CHECK(result == YAML_ERROR_INVALID || result == YAML_ERROR_EOF);
  }
}

static void
test_raw_and_limits(void) {
  SioBufferReader reader;
  YamlParser parser;
  YamlValue root;
  init("\"a\\u0062\\n\"", &reader, &parser, &root);
  uint8_t bytes[64];
  SioBufferWriter writer;
  sio_buffer_writer_init(&writer, bytes, sizeof(bytes));
  OK(yaml_value_read_raw_string(&root, (SioWriter*) &writer));
  CHECK(sio_buffer_writer_get_size(&writer) == 9);
  CHECK(memcmp(bytes, "a\\u0062\\n", 9) == 0);
  init("x: y", &reader, &parser, &root);
  parser.max_depth = 0;
  CHECK(yaml_value_advance(&root) == YAML_ERROR_DEPTH_LIMIT);
  init("x:\n  y: z", &reader, &parser, &root);
  parser.max_depth = 1;
  CHECK(yaml_value_advance(&root) == YAML_ERROR_DEPTH_LIMIT);
  init("hello", &reader, &parser, &root);
  sio_buffer_writer_init(&writer, bytes, 2);
  CHECK(yaml_value_read_string(&root, (SioWriter*) &writer) == YAML_ERROR_IO);
}


static void
test_empty_and_explicit(void) {
  const char* valid[] = {
    "a:", "a: # comment", "a:\n", "a:\n  ", "a:\n  b:",
    "-\n-\n", "a: |+\n  \nb: end\n",
    "? foo\n: bar\n", "? |\n  foo\n: bar\n",
    "? foo\n  bar\n: value\n", "? foo\nnext: value\n",
    "a:\n  same: x\nb:\n  same: y\n",
    "'': value\n", "a: first\r\nb: second\r\nc: third\r",
  };
  for (size_t i = 0; i < sizeof(valid) / sizeof(*valid); i++) {
    SioBufferReader reader;
    YamlParser parser;
    YamlValue root;
    init(valid[i], &reader, &parser, &root);
    OK(yaml_value_advance(&root));
    CHECK(yaml_value_at_end(&root));
  }
  SioBufferReader reader;
  YamlParser parser;
  YamlValue root, map;
  init("? |\n  key\n: value\n", &reader, &parser, &root);
  OK(yaml_value_enter_container(&root, &map));
  string(&map, "key\n");
  string(&map, "value");
  OK(yaml_value_leave_container(&root, &map));
}


static void
test_long_keys_and_raw_blocks(void) {
  char input[1024];
  memset(input, 'a', 200);
  memcpy(input + 200, ": one\n", 6);
  memset(input + 206, 'a', 200);
  memcpy(input + 406, ": two\n", 7);
  SioBufferReader reader;
  YamlParser parser;
  YamlValue root;
  init(input, &reader, &parser, &root);
  OK(yaml_value_advance(&root));
  CHECK(yaml_value_at_end(&root));
  input[405] = 'b';
  init(input, &reader, &parser, &root);
  OK(yaml_value_advance(&root));

  init("|-\r\n  first\r\n    second\r\n", &reader, &parser, &root);
  uint8_t bytes[64];
  SioBufferWriter writer;
  sio_buffer_writer_init(&writer, bytes, sizeof(bytes));
  OK(yaml_value_read_raw_string(&root, (SioWriter*) &writer));
  const char* expected = "  first\r\n    second\r\n";
  CHECK(sio_buffer_writer_get_size(&writer) == strlen(expected));
  CHECK(memcmp(bytes, expected, strlen(expected)) == 0);

  init(
    "\"\\0\\a\\b\\t\\n\\v\\f\\r\\e\\ \\\"\\/\\\\\\N\\_\\L\\P\"",
    &reader,
    &parser,
    &root
  );
  sio_buffer_writer_init(&writer, bytes, sizeof(bytes));
  UtfDecoder decoder;
  utf_decoder_init(&decoder, (SioWriter*) &writer);
  OK(yaml_value_read_unicode(&root, &decoder));
  const uint8_t escaped[] = {
    0, 7, 8, 9, 10, 11, 12, 13, 27, 32, 34, 47, 92,
    0xc2, 0x85, 0xc2, 0xa0, 0xe2, 0x80, 0xa8, 0xe2, 0x80, 0xa9,
  };
  CHECK(sio_buffer_writer_get_size(&writer) == sizeof(escaped));
  CHECK(memcmp(bytes, escaped, sizeof(escaped)) == 0);
}


static void
test_repeated_keys(void) {
  const char* text =
    "a: first\n"
    "a: second\n"
    "a: third\n";
  const char* escaped_text =
    "a: first\n"
    "'a': second\n"
    "\"\\u0061\": third\n"
    "? a\n: fourth\n";
  const char* cases[] = { text, escaped_text };
  for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
    SioBufferReader reader;
    YamlParser parser;
    YamlValue root, map;
    init(cases[i], &reader, &parser, &root);
    OK(yaml_value_enter_container(&root, &map));
    string(&map, "a");
    string(&map, "first");
    string(&map, "a");
    string(&map, "second");
    string(&map, "a");
    string(&map, "third");
    if (i == 1) {
      string(&map, "a");
      string(&map, "fourth");
    }
    CHECK(yaml_value_at_end(&map));
    OK(yaml_value_leave_container(&root, &map));
    CHECK(yaml_value_at_end(&root));

    // Skipping and leaving containers must also accept repeated keys.
    init(cases[i], &reader, &parser, &root);
    OK(yaml_value_advance(&root));
    CHECK(yaml_value_at_end(&root));
    init(cases[i], &reader, &parser, &root);
    OK(yaml_value_enter_container(&root, &map));
    string(&map, "a");
    OK(yaml_value_leave_container(&root, &map));
    CHECK(yaml_value_at_end(&root));
  }
}

int
main(void) {
  test_scalars();
  test_containers();
  test_empty_and_explicit();
  test_invalid();
  test_repeated_keys();
  test_raw_and_limits();
  test_long_keys_and_raw_blocks();
  puts("yaml tests passed");
  return 0;
}
