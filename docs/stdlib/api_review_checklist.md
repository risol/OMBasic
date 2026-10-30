# Standard Library API Review Checklist

Use this checklist before stabilizing or changing a public standard-library
symbol.

## API classification

- [ ] Is the operation pure `FUNCTION`, synchronous side-effecting `ACTION`, or
  Actor-based asynchronous service work?
- [ ] Does a pure function avoid file, network, time, and process side effects?
- [ ] Does asynchronous work avoid futures, callbacks, hidden waiting, and
  continuation-based APIs?

## Actor service contract

- [ ] Are Request and Completion Message fields and reply protocol documented?
- [ ] Are stable error categories distinct from Runtime Faults?
- [ ] Are service shutdown, requester shutdown, and rejected requests defined?
- [ ] Are ordering, concurrency, queue capacity, and per-request limits defined?
- [ ] Are resource ownership and release rules explicit?

## Native boundary

- [ ] Is ordinary OMBasic sufficient before adding Native code?
- [ ] Are ABI types, effects, blocking behavior, and platform behavior defined?
- [ ] Is CString/CPointer ownership Borrowed or Owned and mechanically released?
- [ ] Are invalid input and OS failures mapped to stable errors?

## Errors, offsets, and limits

- [ ] Are public error codes stable lower_snake_case ASCII identifiers?
- [ ] Are ordinary input and operating-system failures returned as Results or
  completion payloads instead of Runtime Faults?
- [ ] Does every reported parse offset specify whether it counts UTF-8 bytes or
  Unicode scalar values?
- [ ] Are size calculations checked before allocation and bounded by a
  documented resource limit?
- [ ] Does exceeding a limit return `resource_exhausted` without assertions or
  integer wraparound?
- [ ] Are Runtime Faults reserved for violated invariants and unrecoverable
  Runtime conditions?

## Compatibility and evidence

- [ ] Is the symbol exported through the intended Facade only?
- [ ] Are boundary, failure, and success semantics documented?
- [ ] Is there a conformance case for the public source API?
- [ ] Are Native and Runtime tests added when the implementation crosses those layers?
- [ ] Are SDK manifest, installation, and relocation checks still valid?
- [ ] Does every asynchronous system service document queue rejection,
  requester shutdown, Runtime shutdown, and completion delivery behavior?
- [ ] Are binary values represented by `Buffer`, never by `String`?
