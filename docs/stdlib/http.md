# HTTP

The `@std/http/http.bas` facade provides bounded HTTP/1.1 request
serialization and response parsing over `Buffer` values. Import
`@std/http/client.bas` for the Network-backed Client Actor.

Each `Http_Client` actor handles one request at a time and reuses an idle
HTTP/1.1 connection for a later request to the same scheme, host, and port
when the response does not request closure. Spawn an actor and send:

```basic
SEND client, http_client.Http_Get(
    "GET", "https://example.com/items?limit=10", "", encoding.Buffer([]),
    Actor_Id(SELF))
```

`headers_text` contains zero or more `Name: value` lines separated by LF.
The request body is always a `Buffer`. Methods are validated as HTTP token
values. The client resolves the URI host through the Network DNS service,
connects with TCP, and upgrades HTTPS connections through the TLS service with
normal certificate and host verification.

Completion is delivered as `Http_Completed(success, status, reason, headers,
body, error_code, error_offset)`. On success, `status`, `reason`, `headers`,
and `body` describe the response and `error_code` is empty. On failure,
`error_code` is a stable HTTP, DNS, or Network error and `error_offset` is the
response byte offset when the HTTP parser reports one. Pass `Actor_Id(SELF)` as
the reply identity because the client retains the destination while network
messages are in flight.

Send `Http_Close()` when the client Actor is no longer needed. While idle, it
closes any retained connection and stops the Actor. A close message sent during
an active request is ignored; send it after receiving `Http_Completed`.

The actor allows one in-flight request. A second request received before the
first completes is returned with `resource_exhausted`. Requests send
`Connection: keep-alive`; a response with `Connection: close`, an EOF-delimited
body, or a different origin closes the transport. The actor does not retry a
stale idle connection and follows no redirects. Use `HEAD` to request response
metadata without a body. Request and response bodies are limited to 64 MiB;
response headers are limited to 64 KiB and 256 fields.

Bracketed IPv6 URI hosts are preserved in the HTTP `Host` header and
unbracketed for DNS lookup and TLS host verification. Network resources,
including TLS sessions and HTTP Client transports, share the 1,024-resource
Runtime limit.

The wire parser supports Content-Length, chunked responses, and bodies delimited
by connection close. `parse_head` and `parse_head_eof` parse HEAD response
headers while requiring that no body bytes follow. HTTP/2, HTTP/3, proxies,
cookie storage, automatic authentication, multipart requests, and server APIs
are outside this version.
