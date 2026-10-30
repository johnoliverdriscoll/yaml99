# yaml99

An allocation-free StrictYAML syntax parser for C99 with caller-owned parsing
contexts. Maps and arrays are traversed through iterators. Scalars are decoded
as UTF-8 strings without implicit number, boolean, or null conversion.

## Build

```sh
git clone --recurse-submodules https://github.com/johnoliverdriscoll/yaml99.git
cd yaml99
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

See [BUILDING.md](BUILDING.md) for installation and build options.

## Parsing behavior

- Input must be UTF-8 and seekable. Parsing uses lookahead and rereads input
  without allocating buffers for lines or values.
- Maps have string keys. Arrays and maps use block syntax. Entries are delivered
  in source order, including duplicates. The owner is responsible for checking
  decoded keys for uniqueness.
- Plain, single-quoted, double-quoted, literal, and folded strings are supported.
  Empty values are empty strings. Applications interpret strings as needed.
- Indentation, comments, line folding, escapes, block indentation indicators,
  and trailing-newline indicators are validated during traversal.
- Blank and comment-only lines between entries accept spaces and tabs.
  Tabs cannot provide structural indentation.
- One document is read, optionally enclosed by `---` and `...`. Flow collections,
  anchors, aliases, tags, and directives are rejected.
- Consume or skip the root to validate the complete input. Initialization alone
  does not validate the document. A caller-selected depth limit bounds nesting.

## Traversal

Entering a container suspends its parent iterator. A map iterator visits a key,
then its value, then the next key. Reading a string advances the iterator.
`yaml_value_advance` validates and skips a value, including its descendants.
Leaving a container validates and skips any remaining children before advancing
its parent. Check `yaml_value_at_end` before consuming another value.

String readers accept an optional `SioWriter`. Passing `NULL` validates without
producing output. Allocating string getters are available through `yaml99_heap`.
Public headers document ownership and error contracts.
