# Spec NNN: <feature name>

- **Status**: draft | accepted | implemented | superseded by NNN
- **Date**: YYYY-MM-DD
- **Author**: <who>

## Summary

One paragraph: what this feature is and why, in plain language.

## Problem

What is missing or wrong today, and who is affected. Concrete examples of queries/workloads that do
not work yet.

## Design

The chosen approach. Cover, as applicable: SQL surface, the manager methods touched, the T-SQL
generated, interactions with DuckLake semantics (snapshots, inlining, retries), and version-pin
implications.

## Enforcement & security

Fail-closed behavior, trust assumptions, what happens across a version mismatch.

## Testing

How it is proven: sqllogictest cases, C++ tests, integration scenarios against a real SQL Server.

## Alternatives considered

Options rejected and why (briefly).

## Follow-ups
