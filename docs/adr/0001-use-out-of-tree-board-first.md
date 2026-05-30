# ADR 0001 — Use an out-of-tree Zephyr board first

## Status

Accepted.

## Context

We want fast public iteration without waiting for upstream review. However, the final project must remain upstream-oriented.

## Decision

Develop `m5stack_sticks3` as an out-of-tree board inside this repository first. Once build and hardware evidence exist, convert the changes into upstream Zephyr PRs.

## Consequences

Positive:

- Fast iteration.
- Can publish reproducible work early.
- Keeps risky experimental drivers outside upstream until validated.

Negative:

- Board layout may need adjustment to match current Zephyr upstream conventions.
- Users must set `BOARD_ROOT` or use this application repository correctly.
