# open-switch — OpenSpec project root

This directory follows the OpenSpec convention. Each `changes/<id>/`
directory documents one logical change to the codebase:

- `proposal.md` — what + why (single page)
- `tasks.md` — subtask breakdown for implementation
- `designs/` — design decisions, ADRs, deep dives
- `specs/` — the externally-visible contracts (APIs, file formats, configs)

Phase 1 (V1 of the module) lives under `changes/core-module-v1/`.

After Phase 1 lands and is implemented, future changes (transports, codecs,
features) get their own `changes/<id>/` directory and reference the
specs they extend or modify.
