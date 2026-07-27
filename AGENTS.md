# Agent Coding Guidance

This document describes the coding conventions and working practices expected
when contributing to this repository. Follow these guidelines to keep the
codebase consistent, the history readable, and the reviews fast.

## Commit discipline

- Each commit should represent a single logical change. If a change touches
  multiple concerns, split it into multiple commits within the same branch.
- Write conventional commit messages: `feat:`, `fix:`, `refactor:`, `docs:`,
  `chore:`, `build:`, `ci:`. Use lowercase and no trailing period.
- Group related universal improvements (cleanups, refactors, bugfixes) into
  their own commit before adding new features on top. This keeps the feature
  commit focused on the new behaviour.

## Feature flags

- Any new behaviour that changes runtime defaults must be gated behind a CMake
  `option()` with a default of `OFF`.
- The default build path must produce the same behaviour as the current release.
- Use a `#ifdef GUARD_NAME` block in C sources rather than replacing the
  existing code path. The original logic stays under `#else`.
- Name the flag clearly: describe what it enables, e.g.
  `WITH_ALSA_SELFCONFIGURATION` rather than `NEW_ALSA`.

## Language

- All comments, commit messages, and documentation must be in English.
- No other language than English — no code-switching, no bilingual commentary.
- Existing comments in English should stay in English; rewording for clarity is
  fine but keep the same language.

## Code style

- Respect the existing style of the file you are editing. The codebase is C99
  with mixed `//` and `/* */` comments, `#ifdef` guards indented inside
  enclosing scope, and variables declared at the top of the enclosing block.
- Do not column-align variable declarations or assignments with extra
  whitespace. Declarations should read naturally, e.g.:
  ```c
  static snd_pcm_t *handle;
  static OpusMSDecoder* decoder;
  ```
- CMake `option()` and `set()` calls should not be padded to align values.
- Do not introduce `#pragma once`. This project follows the established GPL
  license header convention and does not use `#pragma once` in its headers.

## Comments

- Use plain, single-line section markers when you need to separate logical
  blocks. A short `/* Initialisation */` comment is fine.
- Do not use ascii-art separators, banner comments, or decorative box-drawing.
- Comments should explain the rationale for non-obvious logic. The comment
  "increase buffer to 200 ms to absorb network irregularities" is useful.
  A bare "increased to 200 ms" is less so.

## Licensing

- Never modify, truncate, or remove the GPL license block at the top of a file.
  These blocks are part of the project's legal identity and must stay intact.
- For files that use a third-party license (e.g. Broadcom's BSD-3 in `mmal.c`),
  do not alter or replace that license text either.

## Scoping

- Do not include changes unrelated to the feature you are working on. A commit
  adding a new video backend should not also reformat audio code, rename
  variables in unrelated files, or reindent build scripts.
- Keep CI changes, build system tweaks, documentation updates, and feature work
  in separate commits — ideally separate branches — so they can be reviewed and
  cherry-picked independently.

## Submodules

- When you bump a submodule, commit the updated pointer together with any source
  changes needed to match the new submodule version. A submodule bump with no
  accompanying code changes, or code changes that expect a new submodule version
  without the pointer update, will break the build.
- Run `git submodule update --init --recursive` after checking out a branch
  that changes submodule pointers.

## Build verification

- After making changes, verify the build compiles with the feature flag both
  ON and OFF, using the standard CMake invocation:
  ```sh
  mkdir build && cd build
  cmake .. && make -j$(nproc)
  ```
- For feature-flagged changes, also test the flag explicitly:
  ```sh
  cmake .. -DYOUR_FLAG=ON && make -j$(nproc)
  ```
