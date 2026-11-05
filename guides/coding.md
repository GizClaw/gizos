# Coding conventions

Match the surrounding code and keep changes focused. Preserve upstream style in vendored code; adapt it through the owning wrapper or patch.

## Names and files

- Use stable names that identify the owner and purpose. Avoid generic `common`, `helper`, or `manager` buckets and temporary `new`, `old`, or migration suffixes.
- Use lowercase repository paths and snake case for C files, Markdown files, and C symbols. Preserve established upstream and toolchain names.
- Public C headers use `h2_<module>.h`; public symbols carry their owner prefix, typedefs end in `_t`, and macros and enum constants use uppercase owner prefixes.
- Keep private headers with their implementation and out of public include paths. Use `tests/` for tests.
- Distinguish App-facing `component_id` from board-facing `periph_id`. Name boards, chips, targets, images, and Apps accurately. Include units in time and length fields.

## C and C++

- Keep portable C compatible with C11 and clean under `-Wall -Wextra -Werror`. Isolate SDK extensions and types in their platform owner.
- Public Headers expose the minimum contract, with include guards and C++ linkage guards where needed. Include the matching header first in its implementation.
- Document parameter direction, ownership, blocking, timeout units, callback context, errors, and lifecycle in the Public Header. Use opaque handles for private state.
- Use `size_t` for sizes and explicit integer widths for wire formats. Encode fields explicitly; never serialize native struct padding or assume alignment, endianness, or enum width.
- Validate pointers, lengths, ranges, arithmetic overflow, and state before use. Preserve meaningful signed error codes and check every fallible operation.
- Keep capability API objects separate from mutable instance state. Inject dependencies explicitly rather than hiding them in singletons.
- Comments explain contracts and non-obvious constraints, not the next line of code. Keep hardware workarounds scoped to their actual SDK or device conditions.

## Go

- Use `gofmt`, idiomatic package names, and MixedCaps exports. Export only what consumers need.
- Prefer concrete constructors and small consumer-owned interfaces. Pass required dependencies explicitly.
- Propagate `context.Context` through cancellable work. Return errors for ordinary failures, preserve causes with `%w`, and keep secrets out of messages.
- Give each goroutine, channel, timer, subprocess, and handle an explicit shutdown owner and exit path.
- Keep generated output deterministic. Validate external paths and payloads, and replace important files atomically.

## Documentation

- Write maintained documentation in English. Keep commands, paths, and identifiers exact.
- Keep each prose paragraph on one physical source line; do not wrap at a fixed print width. Preserve structural Markdown line breaks.
- Use relative file links, concise headings, and language-tagged code fences. Link to source definitions instead of copying them.
- Keep usage in the owning module's README and contracts in Public Headers. `guides/` holds development constraints only.
- Do not add App prototypes, Product IA, architecture diagrams, change logs, session progress, or verification archives to `guides/`.
