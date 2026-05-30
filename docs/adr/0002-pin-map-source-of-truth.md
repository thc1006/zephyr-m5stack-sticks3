# ADR 0002 — Treat M5Stack official PinMap as source of truth until measured otherwise

## Status

Accepted.

## Context

Board bring-up depends on exact pins. Community examples may be wrong or stale.

## Decision

Use the official M5Stack StickS3 PinMap as the source of truth for initial DTS and validation docs. Any correction must cite a datasheet, schematic, hardware measurement, or upstream maintainer feedback.

## Consequences

- Reduces accidental pin drift.
- Makes review easier.
- Hardware measurement can override docs, but must be recorded.
