# Timer contract


The current timer surface is the `Timer` Runtime Service Actor, not a blocking
sleep function:

```basic
SEND Timer, Timer_After(1000, Tick(), recipient)
```

The delay is a non-negative signed Int64 count of milliseconds. The Runtime
uses a monotonic clock for deadlines. At expiry it posts the supplied Message
to the recipient mailbox; no synchronous result or completion Message is
produced. The system Actor clones the Message into an owned Runtime context so
the caller's heap lifetime does not constrain the scheduled payload. A negative
delay is rejected by the Runtime send path; deadline overflow is reported as a
Runtime fault. There is currently no cancellation, wall-clock API, or
high-resolution duration type. The source facade also defines a signed
`Duration(milliseconds)` value with pure construction, conversion, addition,
subtraction, and comparison helpers. Duration arithmetic uses checked Int64
operations and therefore raises the standard arithmetic Runtime Fault on
overflow. Negative Duration values are valid spans, but Timer_After still
requires a non-negative delay.

The API guarantees millisecond units, not delivery at an exact millisecond.
Clock granularity and scheduler load can delay delivery. A scheduled timer has
no cancellation handle; once accepted, it remains queued until delivery or
Runtime shutdown. Wall-clock timestamps and elapsed-time measurements are
separate concepts and are not exposed by this Timer API.

No blocking `sleep` or hidden continuation is exposed. The
`system-timer-delivers-message` Conformance case validates mailbox delivery and
the Actor's normal run-to-completion behavior. The
`system-timer-respects-mailbox-order` case checks that a message already queued
for an Actor is handled before a later timer completion.

`Duration` is a signed millisecond span. `Instant` is a monotonic millisecond
count whose origin is unspecified; comparisons and subtraction are meaningful
only for samples from the same Runtime clock. It is not a UTC timestamp.

`Instant` is an `Instant(milliseconds)` record in `@std/time/instant.bas`.
Its pure helpers compare two values, expose their millisecond counts, and
compute a signed millisecond difference. The `instant` module is imported
separately from `time.bas`.

`Date_Time` is a UTC proleptic-Gregorian calendar record in
`@std/time/datetime.bas`, with millisecond precision. The supported year range
is 1 through 9999. `datetime_from_unix_milliseconds` and
`datetime_to_unix_milliseconds` convert to and from signed Unix epoch
milliseconds. Invalid fields and out-of-range timestamps return
`Error("out_of_range")`. Local time zones, locale formatting, and leap seconds
are outside this version.

The `Clock` system Actor keeps the two clock domains explicit:

```basic
SEND Clock, Clock_Monotonic_Now(reply_actor)
SEND Clock, Clock_Utc_Now(reply_actor)
```

`Clock_Monotonic_Now_Completed` and `Clock_Utc_Now_Completed` carry
`Result<Int, String>` in milliseconds: monotonic milliseconds for the first
request, Unix epoch milliseconds for the second. Clock reads are asynchronous
Messages; conversion functions remain pure. Clock failures use stable codes
`io_error` or `out_of_range`; queue admission failures follow the Runtime send
fault path and do not have a completion.

Timer cancellation is not part of v1. An accepted timer remains scheduled
until delivery or Runtime shutdown; shutdown cancels pending timers and
suppresses delivery. Negative delays and deadline overflow are synchronous
Runtime send faults.
