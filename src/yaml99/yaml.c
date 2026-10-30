#include <limits.h>
#include <string.h>
#include <yaml99/yaml.h>
#include "yaml_assert.h"
#include "yaml_try.h"
#include "yaml_require.h"

/* A physical line, inspected without retaining its bytes. */
typedef struct YamlLine {
  YamlMark start;
  YamlMark content;
  YamlMark end;
  YamlMark next;
  uint8_t first;
  bool blank;
  bool comment;
  bool tab_indent;
  bool eof;
  bool has_break;
} YamlLine;

/* Source spelling boundaries, excluding quotes or a block header. */
typedef struct YamlSpan {
  YamlMark start;
  YamlMark end;
} YamlSpan;

static int yaml_prepare(YamlValue* it);
static int yaml_after_indicator(YamlValue* it, bool map);
static int yaml_next(YamlValue* it);
static int yaml_scalar(YamlValue* it, UtfDecoder* decoder, YamlSpan* span);

static bool
yaml_space(
  uint8_t c
) {
  return c == ' ' || c == '\t';
}

static bool
yaml_break(
  uint8_t c
) {
  return c == '\n' || c == '\r';
}

static int
yaml_utf_result(
  int result
) {
  YAML_REQUIRE_NE(result, UTF_ERROR_WRITE, YAML_ERROR_IO);
  YAML_REQUIRE_EQ(result, UTF_OK, YAML_ERROR_INVALID);
  return YAML_OK;
}

static int
yaml_peek(
  YamlParser* parser,
  uint8_t* c,
  bool* eof
) {
  YAML_REQUIRE_EQ(
    sio_reader_peek((SioReader*) parser->reader, c, eof),
    SIO_OK,
    YAML_ERROR_IO
  );
  return YAML_OK;
}

static int
yaml_read(
  YamlParser* parser,
  uint8_t* out
) {
  uint8_t c;
  int result = sio_reader_read((SioReader*) parser->reader, &c);
  YAML_REQUIRE_NE(result, SIO_ERROR_EOF, YAML_ERROR_EOF);
  YAML_REQUIRE_EQ(result, SIO_OK, YAML_ERROR_IO);
  YAML_REQUIRE_NE(parser->mark.position, UINT64_MAX, YAML_ERROR_INVALID);
  YAML_REQUIRE_NE(parser->mark.column, UINT_MAX, YAML_ERROR_INVALID);
  parser->mark.position++;
  parser->mark.column++;
  if (out != NULL) {
    *out = c;
  }
  return YAML_OK;
}

static int
yaml_seek(
  YamlParser* parser,
  YamlMark mark
) {
  YAML_REQUIRE_EQ(
    sio_seekable_reader_seek(parser->reader, mark.position),
    SIO_OK,
    YAML_ERROR_IO
  );
  parser->mark = mark;
  return YAML_OK;
}

static int
yaml_newline(
  YamlParser* parser
) {
  uint8_t c;
  bool eof;
  YAML_TRY(yaml_read(parser, &c));
  YAML_ASSERT(yaml_break(c));
  if (c == '\r') {
    YAML_TRY(yaml_peek(parser, &c, &eof));
    if (!eof && c == '\n') {
      YAML_TRY(yaml_read(parser, NULL));
    }
  }
  YAML_REQUIRE_NE(parser->mark.line, UINT64_MAX, YAML_ERROR_INVALID);
  parser->mark.line++;
  parser->mark.column = 0;
  return YAML_OK;
}

/* Validate complete physical lines, including ignored comments. */
static int
yaml_line(
  YamlParser* parser,
  YamlLine* out
) {
  *out = (YamlLine) {
    .start = parser->mark,
    .content = parser->mark,
    .blank = true,
  };
  UtfDecoder utf;
  utf_decoder_init(&utf, NULL);
  bool prefix = true;
  bool tabs = false;
  uint8_t previous = 0;
  uint8_t before_previous = 0;
  for (;;) {
    uint8_t c = 0;
    bool eof;
    YAML_TRY(yaml_peek(parser, &c, &eof));
    if (eof || yaml_break(c)) {
      YAML_TRY(yaml_utf_result(utf_decoder_finish(&utf)));
      out->end = parser->mark;
      out->eof = eof && out->start.position == out->end.position;
      out->has_break = !eof;
      if (!eof) {
        YAML_TRY(yaml_newline(parser));
      }
      out->next = parser->mark;
      return YAML_OK;
    }
    YAML_REQUIRE(c >= 0x20 || c == '\t', YAML_ERROR_INVALID);
    YAML_REQUIRE_NE(c, 0x7f, YAML_ERROR_INVALID);
    YAML_REQUIRE(
      previous != 0xc2 || c < 0x80 || c > 0x9f || c == 0x85,
      YAML_ERROR_INVALID
    );
    YAML_REQUIRE(
      before_previous != 0xef || previous != 0xbf || c < 0xbe,
      YAML_ERROR_INVALID
    );
    YAML_TRY(yaml_utf_result(utf_decoder_write_utf8(&utf, c)));
    if (prefix && c == ' ') {
      out->content = parser->mark;
      out->content.position++;
      out->content.column++;
    } else if (prefix) {
      prefix = false;
      out->first = c;
    }
    if (out->blank && c == '\t') {
      tabs = true;
    } else if (out->blank && !yaml_space(c)) {
      out->blank = false;
      out->comment = c == '#';
      out->tab_indent = tabs;
    }
    before_previous = previous;
    previous = c;
    YAML_TRY(yaml_read(parser, NULL));
  }
}

/*
 * Seek to the next actual entry, not treating comment prefixes as indentation.
 */
static int
yaml_entry_line(
  YamlParser* parser,
  YamlLine* out
) {
  for (;;) {
    YAML_TRY(yaml_line(parser, out));
    if (out->eof) {
      return yaml_seek(parser, out->end);
    }
    if (!out->blank && !out->comment) {
      YAML_REQUIRE(!out->tab_indent, YAML_ERROR_INVALID);
      return yaml_seek(parser, out->content);
    }
  }
}

static int
yaml_spaces(
  YamlParser* parser
) {
  for (;;) {
    uint8_t c = 0;
    bool eof;
    YAML_TRY(yaml_peek(parser, &c, &eof));
    if (eof || !yaml_space(c)) {
      return YAML_OK;
    }
    YAML_TRY(yaml_read(parser, NULL));
  }
}

/* Consume separation after a completed scalar or a collection indicator. */
static int
yaml_tail(
  YamlParser* parser
) {
  YAML_TRY(yaml_spaces(parser));
  uint8_t c = 0;
  bool eof;
  YAML_TRY(yaml_peek(parser, &c, &eof));
  if (eof || parser->mark.column == 0) {
    return YAML_OK;
  }
  if (c == '#') {
    YamlLine line;
    return yaml_line(parser, &line);
  }
  YAML_REQUIRE(yaml_break(c), YAML_ERROR_INVALID);
  return yaml_newline(parser);
}

static int
yaml_indicator(
  YamlParser* parser,
  uint8_t indicator,
  bool* out
) {
  YamlMark mark = parser->mark;
  uint8_t c = 0;
  bool eof;
  *out = false;
  YAML_TRY(yaml_peek(parser, &c, &eof));
  if (!eof && c == indicator) {
    YAML_TRY(yaml_read(parser, NULL));
    YAML_TRY(yaml_peek(parser, &c, &eof));
    *out = eof || yaml_space(c) || yaml_break(c);
  }
  return yaml_seek(parser, mark);
}

static int
yaml_marker(
  YamlParser* parser,
  uint8_t c,
  bool* out
) {
  YamlMark start = parser->mark;
  *out = false;
  if (start.column != 0) {
    return YAML_OK;
  }
  for (unsigned i = 0; i < 3; i++) {
    uint8_t found = 0;
    bool eof;
    YAML_TRY(yaml_peek(parser, &found, &eof));
    if (eof || found != c) {
      return yaml_seek(parser, start);
    }
    YAML_TRY(yaml_read(parser, NULL));
  }
  uint8_t found = 0;
  bool eof;
  YAML_TRY(yaml_peek(parser, &found, &eof));
  *out = eof || yaml_space(found) || yaml_break(found);
  return yaml_seek(parser, start);
}

/* Inspect a possible implicit key without storing it. */
static int
yaml_key_end(
  YamlParser* parser,
  uint64_t* end,
  bool* found
) {
  YamlMark start = parser->mark;
  uint8_t quote = 0;
  uint8_t c = 0;
  bool eof;
  bool escaped = false;
  *found = false;
  YAML_TRY(yaml_peek(parser, &c, &eof));
  if (!eof && (c == '\'' || c == '"')) {
    quote = c;
    YAML_TRY(yaml_read(parser, NULL));
  }
  bool separated = true;
  for (;;) {
    YAML_TRY(yaml_peek(parser, &c, &eof));
    if (eof || yaml_break(c)) {
      break;
    }
    if (quote != 0) {
      YAML_TRY(yaml_read(parser, NULL));
      if (escaped) {
        escaped = false;
      } else if (quote == '"' && c == '\\') {
        escaped = true;
      } else if (c == quote) {
        YAML_TRY(yaml_peek(parser, &c, &eof));
        if (quote == '\'' && !eof && c == '\'') {
          YAML_TRY(yaml_read(parser, NULL));
        } else {
          quote = 0;
        }
      }
      continue;
    }
    if (c == '#' && separated) {
      break;
    }
    if (c == ':') {
      YamlMark colon = parser->mark;
      bool indicator;
      YAML_TRY(yaml_indicator(parser, ':', &indicator));
      if (indicator) {
        *end = colon.position;
        *found = true;
        break;
      }
    }
    separated = yaml_space(c);
    YAML_TRY(yaml_read(parser, NULL));
  }
  if (*found) {
    YAML_TRY(yaml_seek(parser, start));
    unsigned characters = 0;
    while (parser->mark.position < *end) {
      YAML_TRY(yaml_read(parser, &c));
      if ((c & 0xc0) != 0x80) {
        characters++;
        YAML_REQUIRE_LTE(characters, 1024, YAML_ERROR_INVALID);
      }
    }
  }
  return yaml_seek(parser, start);
}

static int
yaml_prepare(
  YamlValue* it
) {
  YamlParser* parser = it->parser;
  it->start = parser->mark;
  it->type = YAML_TYPE_STRING;
  if (it->empty) {
    return YAML_OK;
  }
  uint8_t c = 0;
  bool eof;
  YAML_TRY(yaml_peek(parser, &c, &eof));
  if (eof) {
    it->empty = true;
    return YAML_OK;
  }
  YAML_REQUIRE_NE(c, 0, YAML_ERROR_INVALID);
  YAML_REQUIRE_EQ(strchr(",[]{}&*!%@`", c), NULL, YAML_ERROR_INVALID);
  bool indicator;
  YAML_TRY(yaml_indicator(parser, '?', &indicator));
  if (indicator) {
    it->type = YAML_TYPE_MAP;
    return YAML_OK;
  }
  YAML_TRY(yaml_indicator(parser, '-', &indicator));
  if (indicator) {
    it->type = YAML_TYPE_ARRAY;
  } else if (c != '|' && c != '>') {
    bool found;
    YAML_TRY(yaml_key_end(parser, &it->key_end, &found));
    if (found) {
      it->type = YAML_TYPE_MAP;
    }
  }
  return YAML_OK;
}

static int
yaml_emit_range(
  YamlParser* parser,
  YamlMark start,
  YamlMark end,
  UtfDecoder* decoder
) {
  YAML_TRY(yaml_seek(parser, start));
  while (parser->mark.position < end.position) {
    uint8_t c;
    YAML_TRY(yaml_read(parser, &c));
    YAML_TRY(yaml_utf_result(utf_decoder_write_utf8(decoder, c)));
  }
  return YAML_OK;
}

static int
yaml_emit_breaks(
  UtfDecoder* decoder,
  uint64_t count
) {
  for (uint64_t i = 0; i < count; i++) {
    YAML_TRY(yaml_utf_result(utf_decoder_write_utf8(decoder, '\n')));
  }
  return YAML_OK;
}

/* Count empty continuation lines and strip flow-scalar indentation. */
static int
yaml_flow_break(
  YamlValue* it,
  UtfDecoder* decoder,
  bool escaped
) {
  YamlParser* parser = it->parser;
  YAML_REQUIRE(!it->is_key || it->explicit_key, YAML_ERROR_INVALID);
  YAML_TRY(yaml_newline(parser));
  uint64_t empty = 0;
  for (;;) {
    YamlLine line;
    YAML_TRY(yaml_line(parser, &line));
    YAML_REQUIRE(!line.eof, YAML_ERROR_EOF);
    if (line.blank) {
      YAML_REQUIRE(line.has_break && empty != UINT64_MAX, YAML_ERROR_EOF);
      empty++;
      continue;
    }
    YAML_REQUIRE(
      it->parent == NULL || line.content.column > it->container_indent,
      YAML_ERROR_INVALID
    );
    YAML_TRY(yaml_seek(parser, line.content));
    YAML_TRY(yaml_spaces(parser));
    bool marker;
    YAML_TRY(yaml_marker(parser, '-', &marker));
    YAML_REQUIRE(!marker, YAML_ERROR_INVALID);
    YAML_TRY(yaml_marker(parser, '.', &marker));
    YAML_REQUIRE(!marker, YAML_ERROR_INVALID);
    if (empty == 0 && !escaped) {
      return yaml_utf_result(utf_decoder_write_utf8(decoder, ' '));
    }
    return yaml_emit_breaks(decoder, empty);
  }
}

static int
yaml_escape(
  YamlValue* it,
  UtfDecoder* decoder
) {
  YamlParser* parser = it->parser;
  uint8_t c = 0;
  bool eof;
  YAML_TRY(yaml_peek(parser, &c, &eof));
  YAML_REQUIRE(!eof, YAML_ERROR_EOF);
  if (yaml_break(c)) {
    return yaml_flow_break(it, decoder, true);
  }
  YAML_TRY(yaml_read(parser, &c));
  const char* escapes = "0abtnvfre \"/\\N_LP";
  const uint32_t points[] = {
    0, 7, 8, 9, 10, 11, 12, 13, 27, 32, 34, 47, 92,
    0x85, 0xa0, 0x2028, 0x2029,
  };
  const char* match = NULL;
  if (c != 0) {
    match = strchr(escapes, c);
  }
  if (match != NULL) {
    return yaml_utf_result(utf_decoder_write_utf32(
      decoder,
      points[match - escapes]
    ));
  }
  unsigned digits = 0;
  if (c == 'x') {
    digits = 2;
  } else if (c == 'u') {
    digits = 4;
  } else if (c == 'U') {
    digits = 8;
  }
  YAML_REQUIRE_NE(digits, 0, YAML_ERROR_INVALID);
  uint32_t point = 0;
  for (unsigned i = 0; i < digits; i++) {
    YAML_TRY(yaml_read(parser, &c));
    unsigned value;
    if (c >= '0' && c <= '9') {
      value = c - '0';
    } else if (c >= 'a' && c <= 'f') {
      value = c - 'a' + 10;
    } else {
      YAML_REQUIRE_GTE(c, 'A', YAML_ERROR_INVALID);
      YAML_REQUIRE_LTE(c, 'F', YAML_ERROR_INVALID);
      value = c - 'A' + 10;
    }
    point = (point << 4) | value;
  }
  return yaml_utf_result(utf_decoder_write_utf32(decoder, point));
}

static int
yaml_quoted(
  YamlValue* it,
  UtfDecoder* decoder,
  YamlSpan* span
) {
  YamlParser* parser = it->parser;
  uint8_t quote;
  YAML_TRY(yaml_read(parser, &quote));
  span->start = parser->mark;
  for (;;) {
    uint8_t c = 0;
    bool eof;
    YAML_TRY(yaml_peek(parser, &c, &eof));
    YAML_REQUIRE(!eof, YAML_ERROR_EOF);
    if (c == quote) {
      span->end = parser->mark;
      YAML_TRY(yaml_read(parser, NULL));
      YAML_TRY(yaml_peek(parser, &c, &eof));
      if (quote != '\'' || eof || c != '\'') {
        YAML_REQUIRE(eof || c != '#', YAML_ERROR_INVALID);
        return YAML_OK;
      }
      YAML_TRY(yaml_read(parser, NULL));
      YAML_TRY(yaml_utf_result(utf_decoder_write_utf8(decoder, '\'')));
    } else if (quote == '"' && c == '\\') {
      YAML_TRY(yaml_read(parser, NULL));
      YAML_TRY(yaml_escape(it, decoder));
    } else if (yaml_break(c)) {
      YAML_TRY(yaml_flow_break(it, decoder, false));
    } else if (yaml_space(c)) {
      YamlMark start = parser->mark;
      YAML_TRY(yaml_spaces(parser));
      YamlMark end = parser->mark;
      YAML_TRY(yaml_peek(parser, &c, &eof));
      if (eof || !yaml_break(c)) {
        YAML_TRY(yaml_emit_range(parser, start, end, decoder));
      }
    } else {
      YAML_TRY(yaml_read(parser, &c));
      YAML_TRY(yaml_utf_result(utf_decoder_write_utf8(decoder, c)));
    }
  }
}

static int
yaml_plain(
  YamlValue* it,
  UtfDecoder* decoder,
  YamlSpan* span
) {
  YamlParser* parser = it->parser;
  span->start = parser->mark;
  span->end = parser->mark;
  bool first = true;
  uint64_t empty_lines = 0;
  for (;;) {
    YamlMark start = parser->mark;
    YamlMark end = start;
    bool separated = true;
    bool comment = false;
    for (;;) {
      uint8_t c = 0;
      bool eof;
      YAML_TRY(yaml_peek(parser, &c, &eof));
      if (
        eof
        || yaml_break(c)
        || (
          it->is_key
          && !it->explicit_key
          && parser->mark.position == it->key_end
        )
      ) {
        break;
      }
      if (c == '#' && separated) {
        comment = true;
        break;
      }
      if (c == ':') {
        bool indicator;
        YAML_TRY(yaml_indicator(parser, ':', &indicator));
        YAML_REQUIRE(!indicator, YAML_ERROR_INVALID);
      }
      YAML_TRY(yaml_read(parser, &c));
      separated = yaml_space(c);
      if (!separated) {
        end = parser->mark;
      }
    }
    YamlMark stop = parser->mark;
    if (!first) {
      if (empty_lines == 0) {
        YAML_TRY(yaml_utf_result(utf_decoder_write_utf8(decoder, ' ')));
      } else {
        YAML_TRY(yaml_emit_breaks(decoder, empty_lines));
      }
    }
    YAML_TRY(yaml_emit_range(parser, start, end, decoder));
    span->end = end;
    YAML_TRY(yaml_seek(parser, stop));
    if ((it->is_key && !it->explicit_key) || comment) {
      return YAML_OK;
    }
    uint8_t c = 0;
    bool eof;
    YAML_TRY(yaml_peek(parser, &c, &eof));
    if (eof) {
      return YAML_OK;
    }
    YAML_TRY(yaml_newline(parser));
    YamlMark after = parser->mark;
    empty_lines = 0;
    for (;;) {
      YamlLine line;
      YAML_TRY(yaml_line(parser, &line));
      if (line.blank && !line.eof && line.has_break) {
        YAML_REQUIRE_NE(empty_lines, UINT64_MAX, YAML_ERROR_INVALID);
        empty_lines++;
        continue;
      }
      if (
        line.eof || line.blank || line.comment
        || (it->parent != NULL && line.content.column <= it->container_indent)
      ) {
        return yaml_seek(parser, after);
      }
      YAML_REQUIRE(!line.tab_indent, YAML_ERROR_INVALID);
      YAML_TRY(yaml_seek(parser, line.content));
      bool marker;
      YAML_TRY(yaml_marker(parser, '-', &marker));
      if (marker) {
        return yaml_seek(parser, after);
      }
      YAML_TRY(yaml_marker(parser, '.', &marker));
      if (marker) {
        return yaml_seek(parser, after);
      }
      break;
    }
    first = false;
  }
}

static int
yaml_block(
  YamlValue* it,
  UtfDecoder* decoder,
  YamlSpan* span
) {
  YamlParser* parser = it->parser;
  uint8_t style;
  YAML_TRY(yaml_read(parser, &style));
  unsigned indicator = 0;
  int chomp = 0;
  for (;;) {
    uint8_t c = 0;
    bool eof;
    YAML_TRY(yaml_peek(parser, &c, &eof));
    if (eof || yaml_space(c) || yaml_break(c)) {
      break;
    }
    if (c >= '1' && c <= '9' && indicator == 0) {
      indicator = c - '0';
    } else {
      YAML_REQUIRE(c == '+' || c == '-', YAML_ERROR_INVALID);
      YAML_REQUIRE_EQ(chomp, 0, YAML_ERROR_INVALID);
      if (c == '+') {
        chomp = 1;
      } else {
        chomp = -1;
      }
    }
    YAML_TRY(yaml_read(parser, NULL));
  }
  YAML_TRY(yaml_tail(parser));
  span->start = parser->mark;
  span->end = parser->mark;
  unsigned base = 0;
  if (it->parent != NULL) {
    base = it->container_indent;
  }
  YAML_REQUIRE_LTE(base, UINT_MAX - indicator, YAML_ERROR_INVALID);
  unsigned indent = base + indicator;
  bool known = indicator != 0;
  unsigned leading_spaces = 0;
  bool content = false;
  bool previous_more = false;
  uint64_t breaks = 0;
  for (;;) {
    YamlLine line;
    YAML_TRY(yaml_line(parser, &line));
    if (line.eof) {
      break;
    }
    if (line.content.column == 0) {
      YAML_TRY(yaml_seek(parser, line.content));
      bool marker;
      YAML_TRY(yaml_marker(parser, '-', &marker));
      if (!marker) {
        YAML_TRY(yaml_marker(parser, '.', &marker));
      }
      if (marker) {
        YAML_TRY(yaml_seek(parser, line.start));
        break;
      }
      YAML_TRY(yaml_seek(parser, line.next));
    }
    // Only spaces count as an empty block line. Tabs can be string content.
    bool spaces_only = line.content.position == line.end.position;
    if (!known && spaces_only) {
      if (line.content.column > leading_spaces) {
        leading_spaces = line.content.column;
      }
    } else if (!known) {
      indent = line.content.column;
      if ((it->parent != NULL && indent <= base)
        || (line.comment && indent < leading_spaces)) {
        YAML_TRY(yaml_seek(parser, line.start));
        break;
      }
      YAML_REQUIRE_GTE(indent, leading_spaces, YAML_ERROR_INVALID);
      known = true;
    }
    if (!spaces_only && known && line.content.column < indent) {
      YAML_TRY(yaml_seek(parser, line.start));
      break;
    }
    if (!known || (spaces_only && line.content.column <= indent)) {
      if (line.has_break) {
        YAML_REQUIRE_NE(breaks, UINT64_MAX, YAML_ERROR_INVALID);
        breaks++;
      }
      span->end = line.next;
      continue;
    }
    YamlMark start = line.start;
    start.position += indent;
    start.column = indent;
    uint8_t c = 0;
    bool eof;
    YAML_TRY(yaml_seek(parser, start));
    YAML_TRY(yaml_peek(parser, &c, &eof));
    bool more = !eof && yaml_space(c);
    if (style == '>' && content && !previous_more && !more) {
      if (breaks == 1) {
        YAML_TRY(yaml_utf_result(utf_decoder_write_utf8(decoder, ' ')));
      } else if (breaks > 1) {
        YAML_TRY(yaml_emit_breaks(decoder, breaks - 1));
      }
    } else {
      YAML_TRY(yaml_emit_breaks(decoder, breaks));
    }
    YAML_TRY(yaml_emit_range(parser, start, line.end, decoder));
    YAML_TRY(yaml_seek(parser, line.next));
    span->end = line.next;
    if (line.has_break) {
      breaks = 1;
    } else {
      breaks = 0;
    }
    previous_more = more;
    content = true;
  }
  if (chomp > 0) {
    YAML_TRY(yaml_emit_breaks(decoder, breaks));
  } else if (chomp == 0 && content && breaks > 0) {
    YAML_TRY(yaml_emit_breaks(decoder, 1));
  }
  return YAML_OK;
}

static int
yaml_scalar(
  YamlValue* it,
  UtfDecoder* decoder,
  YamlSpan* span
) {
  YamlParser* parser = it->parser;
  YAML_TRY(yaml_seek(parser, it->start));
  *span = (YamlSpan) { .start = it->start, .end = it->start };
  if (!it->empty) {
    uint8_t c = 0;
    bool eof;
    YAML_TRY(yaml_peek(parser, &c, &eof));
    if (c == '\'' || c == '"') {
      YAML_TRY(yaml_quoted(it, decoder, span));
    } else if (c == '|' || c == '>') {
      YAML_REQUIRE(!it->is_key || it->explicit_key, YAML_ERROR_INVALID);
      YAML_TRY(yaml_block(it, decoder, span));
    } else {
      YAML_TRY(yaml_plain(it, decoder, span));
    }
  }
  return yaml_utf_result(utf_decoder_finish(decoder));
}

static int
yaml_map_key(
  YamlValue* it
) {
  bool explicit_key;
  YAML_TRY(yaml_indicator(it->parser, '?', &explicit_key));
  it->explicit_key = explicit_key;
  if (explicit_key) {
    YAML_TRY(yaml_after_indicator(it, false));
    YAML_REQUIRE_EQ(it->type, YAML_TYPE_STRING, YAML_ERROR_INVALID);
    it->is_key = true;
    return YAML_OK;
  }
  it->is_key = true;
  it->empty = false;
  YAML_TRY(yaml_prepare(it));
  YAML_REQUIRE_EQ(it->type, YAML_TYPE_MAP, YAML_ERROR_INVALID);
  it->type = YAML_TYPE_STRING;
  it->empty = it->start.position == it->key_end;
  return YAML_OK;
}

static int
yaml_after_indicator(
  YamlValue* it,
  bool map
) {
  YamlParser* parser = it->parser;
  YAML_TRY(yaml_read(parser, NULL));
  YAML_TRY(yaml_spaces(parser));
  uint8_t c = 0;
  bool eof;
  YAML_TRY(yaml_peek(parser, &c, &eof));
  it->is_key = false;
  it->empty = false;
  if (eof) {
    it->empty = true;
    return yaml_prepare(it);
  }
  bool new_line = yaml_break(c) || c == '#';
  if (new_line) {
    YAML_TRY(yaml_tail(parser));
    YamlLine line;
    YAML_TRY(yaml_entry_line(parser, &line));
    bool dash = false;
    if (!line.eof && map && line.content.column == it->container_indent) {
      YAML_TRY(yaml_indicator(parser, '-', &dash));
    }
    if (line.eof || (line.content.column <= it->container_indent && !dash)) {
      it->empty = true;
    }
  }
  YAML_TRY(yaml_prepare(it));
  YAML_REQUIRE(
    !map || new_line || it->type == YAML_TYPE_STRING,
    YAML_ERROR_INVALID
  );
  return YAML_OK;
}

static int
yaml_document_end(
  YamlParser* parser
) {
  YamlLine line;
  YAML_TRY(yaml_entry_line(parser, &line));
  if (line.eof) {
    return YAML_OK;
  }
  bool end;
  YAML_TRY(yaml_marker(parser, '.', &end));
  YAML_REQUIRE(end, YAML_ERROR_INVALID);
  for (unsigned i = 0; i < 3; i++) {
    YAML_TRY(yaml_read(parser, NULL));
  }
  YAML_TRY(yaml_tail(parser));
  YAML_TRY(yaml_entry_line(parser, &line));
  YAML_REQUIRE(line.eof, YAML_ERROR_INVALID);
  return YAML_OK;
}

static int
yaml_next(
  YamlValue* it
) {
  YamlParser* parser = it->parser;
  if (it->is_key && it->explicit_key) {
    if (!it->empty && parser->mark.column != 0) {
      YAML_TRY(yaml_tail(parser));
    }
    YamlLine line;
    YAML_TRY(yaml_entry_line(parser, &line));
    bool colon = false;
    if (!line.eof && line.content.column == it->container_indent) {
      YAML_TRY(yaml_indicator(parser, ':', &colon));
    }
    it->explicit_key = false;
    if (colon) {
      return yaml_after_indicator(it, true);
    }
    it->is_key = false;
    it->empty = true;
    return yaml_prepare(it);
  }
  if (it->is_key) {
    YAML_TRY(yaml_spaces(parser));
    YAML_REQUIRE_EQ(parser->mark.position, it->key_end, YAML_ERROR_INVALID);
    return yaml_after_indicator(it, true);
  }
  if (!it->empty && parser->mark.column != 0) {
    YAML_TRY(yaml_tail(parser));
  }
  if (it->parent == NULL) {
    YAML_TRY(yaml_document_end(parser));
    it->at_end = true;
    return YAML_OK;
  }
  uint8_t c = 0;
  bool eof;
  YAML_TRY(yaml_peek(parser, &c, &eof));
  if (eof) {
    it->at_end = true;
    return YAML_OK;
  }
  YamlLine line;
  // Empty values can leave the reader at the next line's first content byte.
  if (parser->mark.column != 0) {
    YamlMark start = parser->mark;
    start.position -= start.column;
    start.column = 0;
    YAML_TRY(yaml_seek(parser, start));
  }
  YAML_TRY(yaml_entry_line(parser, &line));
  bool marker = false;
  if (!line.eof) {
    YAML_TRY(yaml_marker(parser, '.', &marker));
  }
  if (line.eof || marker || line.content.column < it->container_indent) {
    it->at_end = true;
    return YAML_OK;
  }
  YAML_REQUIRE_LTE(
    line.content.column,
    it->container_indent,
    YAML_ERROR_INVALID
  );
  it->empty = false;
  if (it->container_type == YAML_TYPE_MAP) {
    return yaml_map_key(it);
  }
  bool dash;
  YAML_TRY(yaml_indicator(parser, '-', &dash));
  if (!dash) {
    YAML_REQUIRE(it->indentless, YAML_ERROR_INVALID);
    it->at_end = true;
    return YAML_OK;
  }
  return yaml_after_indicator(it, false);
}

int
yaml_parser_init(
  SioSeekableReader* reader,
  unsigned max_depth,
  YamlParser* out_parser,
  YamlValue* out_it
) {
  YAML_ASSERT_NE(out_parser, NULL);
  YAML_ASSERT_NE(out_it, NULL);
  *out_parser = (YamlParser) {
    .reader = reader,
    .active = out_it,
    .max_depth = max_depth,
  };
  YAML_REQUIRE_EQ(
    sio_seekable_reader_tell(reader, &out_parser->mark.position),
    SIO_OK,
    YAML_ERROR_IO
  );
  *out_it = (YamlValue) { .parser = out_parser };
  // UTF-8 BOM is a stream prefix, not scalar content.
  YamlMark start = out_parser->mark;
  uint8_t c = 0;
  bool eof;
  YAML_TRY(yaml_peek(out_parser, &c, &eof));
  if (!eof && c == 0xef) {
    uint8_t b, d;
    YAML_TRY(yaml_read(out_parser, NULL));
    YAML_TRY(yaml_read(out_parser, &b));
    YAML_TRY(yaml_read(out_parser, &d));
    if (b == 0xbb && d == 0xbf) {
      out_parser->mark.column = 0;
    } else {
      YAML_TRY(yaml_seek(out_parser, start));
    }
  }
  YamlLine line;
  YAML_TRY(yaml_entry_line(out_parser, &line));
  bool marker;
  YAML_TRY(yaml_marker(out_parser, '-', &marker));
  if (marker) {
    for (unsigned i = 0; i < 3; i++) {
      YAML_TRY(yaml_read(out_parser, NULL));
    }
    YAML_TRY(yaml_spaces(out_parser));
    YAML_TRY(yaml_peek(out_parser, &c, &eof));
    if (!eof && (yaml_break(c) || c == '#')) {
      YAML_TRY(yaml_tail(out_parser));
      YAML_TRY(yaml_entry_line(out_parser, &line));
    }
  }
  YAML_TRY(yaml_marker(out_parser, '.', &marker));
  out_it->empty = marker || line.eof;
  return yaml_prepare(out_it);
}

YamlType
yaml_value_get_type(
  const YamlValue* it
) {
  YAML_ASSERT_NE(it, NULL);
  YAML_ASSERT(!it->at_end);
  return it->type;
}

bool
yaml_value_at_end(
  const YamlValue* it
) {
  YAML_ASSERT_NE(it, NULL);
  return it->at_end;
}

bool
yaml_value_is_string(
  const YamlValue* it
) {
  YAML_ASSERT_NE(it, NULL);
  return !it->at_end && it->type == YAML_TYPE_STRING;
}

bool
yaml_value_is_array(
  const YamlValue* it
) {
  YAML_ASSERT_NE(it, NULL);
  return !it->at_end && it->type == YAML_TYPE_ARRAY;
}

bool
yaml_value_is_map(
  const YamlValue* it
) {
  YAML_ASSERT_NE(it, NULL);
  return !it->at_end && it->type == YAML_TYPE_MAP;
}

int
yaml_value_read_unicode(
  YamlValue* it,
  UtfDecoder* decoder
) {
  YAML_ASSERT_NE(it, NULL);
  YAML_ASSERT_EQ(it->parser->active, it);
  YAML_ASSERT(!it->at_end);
  YAML_REQUIRE_EQ(it->type, YAML_TYPE_STRING, YAML_ERROR_INVALID);
  YamlSpan span;
  YAML_TRY(yaml_scalar(it, decoder, &span));
  return yaml_next(it);
}

int
yaml_value_read_string(
  YamlValue* it,
  SioWriter* writer
) {
  UtfDecoder decoder;
  utf_decoder_init(&decoder, writer);
  return yaml_value_read_unicode(it, &decoder);
}

int
yaml_value_read_raw_string(
  YamlValue* it,
  SioWriter* writer
) {
  YAML_ASSERT_NE(it, NULL);
  YAML_ASSERT_EQ(it->parser->active, it);
  YAML_ASSERT(!it->at_end);
  YAML_REQUIRE_EQ(it->type, YAML_TYPE_STRING, YAML_ERROR_INVALID);
  UtfDecoder decoder;
  utf_decoder_init(&decoder, NULL);
  YamlSpan span;
  YAML_TRY(yaml_scalar(it, &decoder, &span));
  YamlMark after = it->parser->mark;
  YAML_TRY(yaml_seek(it->parser, span.start));
  while (it->parser->mark.position < span.end.position) {
    uint8_t c;
    YAML_TRY(yaml_read(it->parser, &c));
    if (writer != NULL) {
      YAML_REQUIRE_EQ(sio_writer_write(writer, c), SIO_OK, YAML_ERROR_IO);
    }
  }
  YAML_TRY(yaml_seek(it->parser, after));
  return yaml_next(it);
}

int
yaml_value_enter_container(
  YamlValue* it,
  YamlValue* out_recursed
) {
  YAML_ASSERT_NE(it, NULL);
  YAML_ASSERT_NE(out_recursed, NULL);
  YAML_ASSERT_NE(it, out_recursed);
  YAML_ASSERT_EQ(it->parser->active, it);
  YAML_ASSERT(!it->at_end);
  YAML_REQUIRE_NE(it->type, YAML_TYPE_STRING, YAML_ERROR_INVALID);
  YAML_REQUIRE_LT(it->depth, it->parser->max_depth, YAML_ERROR_DEPTH_LIMIT);
  *out_recursed = (YamlValue) {
    .parser = it->parser,
    .parent = it,
    .container_type = it->type,
    .container_indent = it->start.column,
    .depth = it->depth + 1,
    .indentless = it->parent != NULL
      && it->container_type == YAML_TYPE_MAP
      && it->type == YAML_TYPE_ARRAY
      && it->start.column == it->container_indent,
  };
  YAML_TRY(yaml_seek(it->parser, it->start));
  if (it->type == YAML_TYPE_MAP) {
    YAML_TRY(yaml_map_key(out_recursed));
  } else {
    YAML_TRY(yaml_after_indicator(out_recursed, false));
  }
  it->entered = true;
  it->parser->active = out_recursed;
  return YAML_OK;
}

int
yaml_value_leave_container(
  YamlValue* it,
  YamlValue* recursed
) {
  YAML_ASSERT_NE(it, NULL);
  YAML_ASSERT_NE(recursed, NULL);
  YAML_ASSERT_EQ(recursed->parent, it);
  YAML_ASSERT_EQ(it->parser->active, recursed);
  YAML_ASSERT(it->entered);
  while (!recursed->at_end) {
    YAML_TRY(yaml_value_advance(recursed));
  }
  it->parser->active = it;
  it->entered = false;
  // A completed child leaves the next entry unread, just like an empty scalar.
  it->empty = true;
  return yaml_next(it);
}

int
yaml_value_advance(
  YamlValue* it
) {
  YAML_ASSERT_NE(it, NULL);
  YAML_ASSERT_EQ(it->parser->active, it);
  if (it->at_end) {
    return YAML_OK;
  }
  if (it->type == YAML_TYPE_STRING) {
    return yaml_value_read_string(it, NULL);
  }
  YamlValue child;
  YAML_TRY(yaml_value_enter_container(it, &child));
  return yaml_value_leave_container(it, &child);
}
