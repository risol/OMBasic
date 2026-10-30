# ADR-0035: Time Types, Clock, and Timer Contract

- Status: Accepted
- Date: 2026-10-30

## Context

The standard library currently has signed millisecond `Duration` values and a
monotonic `Timer` Actor. It needs a wall-clock representation and clock reads
without mixing calendar time into timer deadlines or adding hidden waits.

## Decision

1. `Duration` is a signed number of milliseconds. Negative spans are valid;
   checked integer overflow is a Runtime Fault.
2. `Instant` is an `Instant(milliseconds)` record containing a monotonic
   millisecond count. Its origin is unspecified. It can be compared and
   subtracted only when both values came from the same Runtime clock. It is not
   a calendar timestamp and must not be serialized as a cross-process time
   value.
3. `Date_Time` is a UTC proleptic-Gregorian calendar value with millisecond
   precision and years 1 through 9999. Conversion uses signed Unix epoch
   milliseconds (`1970-01-01T00:00:00.000Z` is zero). Invalid fields and
   timestamps outside that range return `Error("out_of_range")`. Time zones,
   locale formatting, and leap seconds are outside this version.
4. The `Clock` system Actor has separate `Clock_Monotonic_Now` and
   `Clock_UTC_Now` requests, each with an explicit reply Actor. Their
   completions carry `Result<Int, String>`: monotonic milliseconds for the
   former and Unix epoch milliseconds for the latter. This keeps unit and
   clock-domain selection visible in the Message name.
5. Clock reads are asynchronous Actor operations. Pure conversions between
   milliseconds, `Instant`, `Duration`, and `Date_Time` do not read the system
   clock.
6. `Timer_After` continues to use a monotonic deadline and a non-negative
   signed millisecond delay. Negative delay and deadline overflow are
   synchronous Runtime send faults. Delivery is best effort after the
   deadline and remains subject to scheduler load.
7. Timer cancellation is deferred from v1. A cancellation handle would need
   explicit identity, completion-versus-cancel race rules, and stale-handle
   behavior. Accepted timers remain scheduled until delivery or Runtime
   shutdown. Runtime shutdown cancels pending timers and suppresses delivery.

## Consequences

Calendar conversion is deterministic and independent of local time-zone
configuration. Applications must use UTC or implement a separate time-zone
database layer. Timer scheduling is not affected by wall-clock adjustments.

The pure `Instant` and `Date_Time` helpers remain separate source modules
under `@std/time/`; their record types are not re-exported from `time.bas`.

## Validation requirements

- Unix epoch, negative sub-day timestamp, leap day, month/year boundaries,
  and supported timestamp endpoints.
- Rejection of invalid calendar fields and out-of-range timestamps.
- Monotonic samples do not decrease within one Runtime instance.
- UTC samples are Unix epoch milliseconds.
- Timer negative-delay, deadline-overflow, ordering, and shutdown behavior.
