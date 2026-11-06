# ADR-0001: UI and core split

Status: accepted
Date: 2026-11-05
Issue: #166

## Context

STACK will eventually run its core as a remote daemon (Phase 6) while the UI stays
on the user's machine. That should be a change of transport, not a rewrite. The
boundary has to exist before editor state does, because state written against
direct access to the UI is what makes the later split expensive.

The frame budget constrains the design too: the main thread must never wait on
disk, the network, or a lock held by another thread.

## Decision

One process, two layers, communicating only through typed messages.

**UI layer**
- Owns the window, the GPU device, and input.
- Runs on the main thread. Window and event calls must stay there for macOS.

**Core layer**
- Owns all editor state: buffers, files, language services, agents.
- Runs on other threads and never depends on SDL or on UI code.

**Messages**
- Defined in `stackide/core/messages.hpp` as `UiToCore` and `CoreToUi`.
- Hold values only: no pointers, references, callbacks, or handles, since none of
  these survive a network hop. Each one is copyable and equality-comparable, which
  `src/core/messages.cpp` checks at compile time.

## Enforcement

`stackide_forbid_links` in `cmake/Boundaries.cmake` fails configuration if
`stackide_core`, or the `stackide_base` and `stackide_profile` libraries it may use,
reaches SDL or a UI target through any chain of links. Core is also compiled without
SDL's include path.

The compile-time checks can't see through a pointer hidden in a struct member, so
the values-only rule also relies on review.

## Consequences

- The event queues in #9 carry these types, so the in-process transport can be
  replaced without changing either layer.
- Every core-to-UI update has a copy cost, and every keystroke crosses threads.
  That crossing counts against the under-8 ms input-to-paint budget. It is expected
  to be small next to the budget, but this is unmeasured until #142.
- The UI cannot read editor state directly, so rendering works from data the core
  sends.

## Open question

Copying a whole buffer's visible text per frame may not fit the budget. A shared
immutable snapshot would, but sharing memory conflicts with values-only messages.
Decide this in Phase 1 with measurements, when text rendering first needs the data.
A likely shape is a message that names a snapshot version, with shared memory as an
in-process optimization behind the transport.

## Alternatives considered

- **Separate processes from the start.** Proves the boundary, but adds
  serialization, process lifecycle, and crash handling years before a remote daemon
  needs them.
- **Shared state behind locks.** Simplest today, but the main thread would wait on
  locks, breaking the frame budget rule, and the split would become a rewrite.
