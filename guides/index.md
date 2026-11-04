# AI development guidance

These guides contain concise constraints for AI agents working on GizOS. Read only the files relevant to the change.

| When changing | Read |
| --- | --- |
| Directories, ownership, dependencies, or artifact entries | [Repository structure](structure.md) |
| Libraries, Apps, Runtime, PAL, or asynchronous behavior | [Development constraints](development.md) |
| Source code, public interfaces, names, or Markdown | [Coding conventions](coding.md) |
| Build rules, tests, CI, or validation | [Build and validation](validation.md) |
| Issues, self-review, or pull requests | [Review guidance](reviewing.md) |

Production code, Public Headers, executable configuration, and tests define behavior. Check the relevant sources before editing; these guides do not duplicate API declarations, constants, resource lists, or support matrices.

Keep App prototypes, Product IA, architecture diagrams, product specifications, tutorials, and generated API mirrors out of `guides/`. Update these files only when a development constraint changes. Put module-specific usage beside its source and API contracts in Public Headers.
