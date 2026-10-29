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
high-resolution duration type.

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
