# Standard Library Resource Limits

Public operations that allocate or retain data must enforce a finite bound
using checked size arithmetic. Exceeding an input or output bound is an
ordinary operation failure reported as the stable code `resource_exhausted`;
it must not rely on assertions or integer overflow.

| Capability | Current limit / policy |
| --- | ---: |
| Buffer | 64 MiB |
| SHA-256 / CRC-32 | 64 MiB per operation |
| Text file read, write, or append | 64 MiB per operation |
| TCP send or receive | 64 MiB per request |
| Connected UDP payload | 64 MiB per request |
| Process stream request | 64 MiB per request |
| Directory listing | 10,000 entries and 64 MiB of combined names |
| URI parser and percent codec | 1 MiB maximum input |
| CSV parser input / serializer output | 64 MiB |
| CSV field / record / row count | 1 MiB / 8 MiB / 100,000 |
| Compression input / output | 64 MiB per operation |
| Regex pattern / input | 4 KiB / 1 MiB bytes; 8,192 NFA instructions; 16 million simulation steps |
| SQLite path / statement | 4 KiB / 1 MiB |
| SQLite query results | 2,048 columns, 65,536 rows, and 64 MiB JSON output |
| SQLite connections | 64 process-wide; lock wait up to 5 seconds |
| HTTP headers | 64 KiB and at most 256 fields |
| HTTP body | 64 MiB by default |
| Active network resources, including HTTP Client transports | 1,024 |

The URI, CSV, HTTP parser, HTTP Client, and Network limits are enforced by the
current implementation. DNS results are limited to 256 addresses per lookup;
TLS sessions share the 1,024 active Network resource limit. Each HTTP Client
Actor handles one request and then closes its transport. Concurrent requests
are bounded by the Network resource limit.

HTTP Client Actors allow one in-flight request, reuse same-origin keep-alive
connections, and close idle transports through `Http_Close`. Concurrent
requests are bounded by the Network resource limit.

Actor services document whether a request is rejected before acceptance or
completed with an error after acceptance. Runtime shutdown cancels queued work
and suppresses completions for accepted work unless a service contract states
otherwise. Callers must not wait for completions while the Runtime is shutting
down.
