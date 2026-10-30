#ifndef YAML_H
#define YAML_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sio99/sio.h>
#include <utf99/utf.h>

/**
 * Independent yaml99 results. Unexpected reader EOF becomes `YAML_ERROR_EOF`.
 * Other reader errors and all output errors become `YAML_ERROR_IO`.
 */
enum {
  YAML_OK,
  YAML_ERROR_EOF,
  YAML_ERROR_IO,
  YAML_ERROR_INVALID,
  YAML_ERROR_DEPTH_LIMIT,
  YAML_ERROR_NO_MEMORY,
  YAML_ERROR_UNKNOWN,
};

/** Scalar values are strings, without implicit type interpretation. */
typedef enum YamlType {
  YAML_TYPE_STRING,
  YAML_TYPE_ARRAY,
  YAML_TYPE_MAP,
} YamlType;

/**
 * Caller-owned value iterator. Map entries are delivered in source order,
 * including duplicate keys. The owner is responsible for checking key uniqueness.
 */
typedef struct YamlValue YamlValue;
typedef struct YamlParser YamlParser;

/**
 * Initializes a parser at the reader's current position and exposes the root.
 * Borrows `reader`, which must support seeking to previously read positions.
 * No allocation occurs. `max_depth` is the maximum number of nested containers.
 * Empty input produces an empty string. Parsing and validation are incremental.
 * Consume the root to validate the complete document, including trailing input.
 * All arguments must be non-`NULL`. Keep parser and active iterators at stable
 * addresses. Do not use the reader independently while parsing.
 * After any error, reinitialize before using the parser again. Errors may leave
 * consumed input and partial output. Callbacks must not reenter the parser.
 */
int yaml_parser_init(
  SioSeekableReader* reader,
  unsigned max_depth,
  YamlParser* out_parser,
  YamlValue* out_it
);

/** Returns the current value's type. `it` must not be at its end. */
YamlType yaml_value_get_type(
  const YamlValue* it
);

/**
 * Reads and advances a string into an owned byte allocation. Free with `free`.
 * Empty strings return `NULL` and size zero. Outputs are cleared on failure.
 * Available in `yaml99_heap`. Arguments must be non-`NULL`.
 */
int yaml_value_get_bytes(
  YamlValue* it,
  void** out_bytes,
  size_t* out_size
);

/**
 * Reads and advances a string into an owned, NUL-terminated allocation.
 * Rejects embedded NUL. Free with `free`. Clears the output on failure.
 * Available in `yaml99_heap`. Arguments must be non-`NULL`.
 */
int yaml_value_get_cstring(
  YamlValue* it,
  char** out_cstring
);

/**
 * Validates and skips the current value, including all descendants, then advances.
 * A map iterator alternates between string keys and their values. At end, no-op.
 * Uses stack space proportional to nesting, bounded by `max_depth`.
 */
int yaml_value_advance(
  YamlValue* it
);

/**
 * Enters a map or array using a separate caller-owned child iterator.
 * The parent is suspended until `yaml_value_leave_container`. Entering a scalar
 * returns `YAML_ERROR_INVALID`. Exceeding the limit returns `YAML_ERROR_DEPTH_LIMIT`.
 * `it` and `out_recursed` must be non-`NULL` and distinct.
 */
int yaml_value_enter_container(
  YamlValue* it,
  YamlValue* out_recursed
);

/**
 * Validates and skips remaining children, then advances the parent.
 * Supply the parent and exact child passed to `yaml_value_enter_container`.
 */
int yaml_value_leave_container(
  YamlValue* it,
  YamlValue* recursed
);

/** Returns whether the iterator has exhausted its container or root document. */
bool yaml_value_at_end(
  const YamlValue* it
);

/** Returns whether the iterator currently points to a string. */
bool yaml_value_is_string(
  const YamlValue* it
);

/** Returns whether the iterator currently points to an array. */
bool yaml_value_is_array(
  const YamlValue* it
);

/** Returns whether the iterator currently points to a map. */
bool yaml_value_is_map(
  const YamlValue* it
);

/**
 * Validates and decodes a scalar to UTF-8, then advances. No terminator is added.
 * Handles plain, quoted, literal, and folded strings. `writer` may be `NULL`
 * to validate and discard. Returns `YAML_ERROR_INVALID` for non-string values.
 */
int yaml_value_read_string(
  YamlValue* it,
  SioWriter* writer
);

/**
 * Reads a scalar through an initialized UTF decoder and advances. `decoder`
 * must be non-`NULL` and ready for a new character. Calls `utf_decoder_finish`
 * before advancing. Escapes are Unicode scalar values, not UTF-16 surrogate
 * pairs. Decoder errors map to yaml99 results. Reinitialize after failure.
 */
int yaml_value_read_unicode(
  YamlValue* it,
  UtfDecoder* decoder
);

/**
 * Validates and advances a string while preserving its source spelling.
 * Quoted strings omit their surrounding quotes. Block strings omit their header
 * and its line break but retain content indentation and original line endings.
 * Plain strings omit trailing separation and comments. Multiline source spelling
 * is retained without folding. `writer` may be `NULL`. No terminator is added.
 */
int yaml_value_read_raw_string(
  YamlValue* it,
  SioWriter* writer
);

/** Internal source location exposed for caller allocation. */
typedef struct YamlMark {
  uint64_t position;
  uint64_t line;
  unsigned column;
} YamlMark;

/** Internal iterator state. Do not modify or copy active iterators. */
struct YamlValue {
  YamlParser* parser;
  YamlValue* parent;
  YamlType type;
  YamlType container_type;
  unsigned container_indent;
  unsigned depth;
  bool at_end;
  bool is_key;
  bool explicit_key;
  bool empty;
  bool entered;
  bool indentless;
  YamlMark start;
  uint64_t key_end;
};

/** Internal parser state. Initialize with `yaml_parser_init`. */
struct YamlParser {
  SioSeekableReader* reader;
  YamlValue* active;
  unsigned max_depth;
  YamlMark mark;
};

#endif /* YAML_H */
