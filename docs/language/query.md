# Query expressions

Query expressions eagerly transform one in-memory `List<T>` into a new
`List<R>`:

```basic
active_users = QUERY user IN users
    WHERE user.enabled
    WHERE user.score >= minimum_score
    ORDER BY user.score DESCENDING, user.name ASCENDING
    TAKE 10
    SELECT User_View(user.name, user.score)
END QUERY
```

The fixed clause order is `WHERE*`, optional `ORDER BY`, optional `TAKE`, then
required `SELECT`. Each order key must state `ASCENDING` or `DESCENDING`.
`WHERE` expressions must be `Bool`; `TAKE` must be `Int`; order keys support
`Int` and `String`; `SELECT` determines the result element type.

The source expression is evaluated once. Query is synchronous and does not
create an iterator or schedule actor work. Filters are fused into one scan, and
an unordered `TAKE` stops the scan as soon as enough rows survive. Ordered
queries use a stable multi-key sort; `ORDER BY ... TAKE K` uses a bounded Top-K
selection before the final stable ordering. Ordered queries evaluate `SELECT`
only after the final row order is known, so projection faults do not occur for
rows discarded by Top-K. Order-key faults occur while scanning rows that pass
all filters, before any projection runs.

The physical implementation uses private transient List builders for scan
results, ordered candidates, keys, and the final projection. These builders are
sealed before ordinary List operations can read the value. This does not add a
source type or change Query's `List<T> -> List<R>` contract.

Query expressions may read lexical values and the current actor's state. They
cannot mutate state, send messages, spawn actors, perform I/O, call actions, or
suspend. A long Query runs synchronously inside its current function or actor
handler; applications that need mailbox responsiveness should process bounded
chunks across messages.

The compiler represents a validated Query with a temporary QIR plan between
typed HIR and MIR. QIR refers to HIR expressions by ID and is never converted
back to syntax. `om_query_plan_dump` provides a deterministic plan dump for
compiler diagnostics and tests.
