# OMBasic 0.1 language freeze policy

## Normative source

[`OMBasic-Language-Standard.md`](../../OMBasic-Language-Standard.md) is the
only normative definition of OMBasic 0.1 source syntax and semantics. Each
normative clause has a stable `OM-SS.RR` Rule ID. Rule IDs are referenced by
the conformance manifest and are not renumbered after the 0.1 release
candidate begins.

ADRs record architecture and compatibility decisions. They do not create or
override language rules. Implementation status, source comments, examples,
and tests are informative: they must follow the Standard and cannot establish
new language behavior on their own.

## Change classes

### Class A: non-semantic

Internal refactors, performance work, code organization, and diagnostic wording
improvements do not change language semantics or Rule IDs. Diagnostic wording
may improve while its stable code and primary span behavior remain unchanged.

### Class B: clarification

A clarification makes the Standard more precise without changing conforming
program behavior. Update the affected Standard text and add or refine
conformance cases. Keep the Rule ID when the rule's meaning is unchanged.

### Class C: semantic change

Syntax, static semantics, runtime-visible behavior, serialization, Actor
protocol behavior, Native ownership, or diagnostic-code meaning changes are
semantic changes. After `0.1.0-rc1`, do not make these changes unless they fix a
direct conflict in the frozen Standard. A necessary exception updates the
Standard and conformance cases, records the reason, and restarts the release
candidate period.

## Diagnostic contract

Diagnostic codes are public behavior. A code keeps its meaning across 0.1
releases. Wording may change. Primary spans remain one-based and use UTF-8 byte
columns with an exclusive end. Conformance cases should assert code and span,
and only assert stable text where it distinguishes the relevant token.

## Review checklist

Every change that can affect OMBasic source behavior answers the checklist in
the pull request template. A semantic change updates the Standard and adds or
updates a case that names the affected Rule ID. Release checks require the
Standard audit and complete conformance Rule coverage.
