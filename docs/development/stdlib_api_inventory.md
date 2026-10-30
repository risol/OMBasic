# Standard Library API Inventory

This inventory reflects the source tree and Prelude builtins reviewed on
2026-10-31. Exported source symbols are listed explicitly. Private BASIC and
Native helpers remain scoped to the source module shown in the final column;
they are not part of the SDK interface fingerprint.

| Area / facade | Public API | Private implementation |
|---|---|---|
| Prelude | `Unit`, `Result`, `Node_Ref`, `Actor_Id`, `Actor_Down_Reason`, `Ok`, `Error`, `Actor_Down`, `print` | Runtime and compiler builtins; lifecycle members `normal`, `failed`, `node_unreachable`. |
| Math | `add`, `abs`, `min`, `max`, `clamp`, `sign`, `is_even`, `is_odd`; `sqrt`, `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`, `exp`, `log`, `log10`, `floor`, `ceil`, `round`, `trunc`, `is_finite`, `is_nan` | `float.bas` Native wrappers; `float.omi` declares `math_float`, `math_is_finite`, `math_is_nan`. |
| Text | `concat`, `equals`, `is_empty`, `length`, `byte_length`, `starts_with`, `ends_with`, `find`, `find_from`, `find_last`, `slice`, `trim`, `trim_start`, `trim_end`, `split`, `join`, `replace` | `text_native.bas` and `.omi`: scalar and byte length, search/slice/trim and owned C-string release helpers. |
| Path | `is_absolute`, `join`, `basename`, `dirname`, `extension`, `stem`, `change_extension` | No Native implementation; operations are lexical and do not access the filesystem. |
| JSON | Records `Parse_Error`, `Value`, `Entry`; `null_value`, `boolean`, `number`, `string_value`, `array`, `object`, `parse`, `kind`, `array_length`, `as_string`, `as_boolean`, `as_number_text`, `as_int64`, `as_float64`, `is_null`, `object_has`, `object_length`, `object_keys`, `object_entries`, `array_get`, `object_get`, `stringify` | `parse_error`, `decode_key_array`; `json_native.bas` wraps parser/query/serialization primitives. `json_native.omi` declares Native entry points and owned-string release. |
| Collections | List: `length`, `is_empty`, `contains`, `index_of`, `concat`, `reverse`, `take`, `drop`, `slice`, `map`, `filter`. Map: `map_length`, `map_is_empty`, `contains_key`, `keys`, `get_or` | `map.bas` imports Text for string-key equality. Current Map facade helpers specialize to String keys. |
| Console | Prelude Actor `Console`; `Console_Write`, `Console_Write_Completed`, `Console_Read_Line`, `Console_Read_Line_Completed`; helpers `write_payload`, `write_line_payload`; `print` | Console requests use the Runtime blocking pool; line reads are cancellable when the program's Actors stop; `print` writes one String plus LF synchronously through the Runtime primitive. |
| File System | Prelude Actor `File_System`; text and binary `File_Read`, `File_Write`, `File_Append`; `File_Exists`, `File_Delete`, `Directory_Create`, `Directory_List`, `File_Stat` and matching completion Messages | Runtime worker-pool path conversion, byte Buffer adapters, filesystem calls, sorting, limits, and stable error mapping. |
| Time | Prelude Actors `Timer`, `Clock`; `Timer_After`; `Clock_Monotonic_Now`, `Clock_Utc_Now` and completions. Source records `Duration`, `Instant`, `Date_Time`; documented conversion and arithmetic helpers. | `datetime.bas`: `is_leap_year`, `days_in_year`, `days_in_month`, `floor_divide`; `instant.bas`: `int_value`. Clock and Timer use Runtime services. |
| Random | `Random_Sample`, `seed`, `next_sample`; system Actor `Secure_Random` with `Secure_Random_Bytes` and `Secure_Random_Bytes_Completed` | `normalize_seed`; deterministic Park–Miller state transition and OS CSPRNG worker request handling. |
| Encoding | `Buffer`, `Encoding_Error`, `buffer_from_bytes`, `buffer_length`, `hex_encode`, `hex_decode`, `base64_encode`, `base64_decode`, `utf8_encode`, `utf8_decode` | `valid_buffer`, byte codecs, and length-delimited UTF-8 Native bridge. |
| Number | `Number_Error`; `parse_int`, `parse_int_radix`, `format_int`, `format_int_radix`, `parse_bool`, `format_bool`, `parse_float`, `format_float` | Native locale-independent integer and floating parser/formatter. |
| URI | `Uri`, `Query_Pair`, `Uri_Error`; `parse`, `format`, percent/query codecs, `resolve_reference`, `remove_dot_segments`, `is_absolute`, `default_port` | Bounded RFC 3986 ASCII/percent parser and UTF-8 percent encoding. |
| Hash | `Hash_Error`; `sha256`, `sha256_hex`, `crc32` | SHA-256 and CRC-32 Native implementations over Buffer bytes. |
| Platform | `os_name`, `architecture`, `target_triple`, `path_separator`, `line_separator` | Compile-target facts emitted by the linked Runtime. |
| Environment | `Environment_Arguments`, `Environment_Get`, `Environment_Current_Directory`, `Environment_Executable_Path` and completion Messages | Runtime snapshot requests; read-only process environment and paths. |
| UUID | `Uuid_Error`; `parse`, `format`, `v4_from_random_bytes` | Formatting and version/variant construction; caller supplies random bytes. |
| CSV | `Csv_Error`, `Csv_Table`; `row_count`, `field_count`, `get`, `parse`, `stringify` | Bounded RFC 4180-style parser and serializer. |
| Compression | `Compression_Error`; `gzip_compress`, `gzip_decompress`, `deflate_compress`, `deflate_decompress` | Pinned miniz DEFLATE core with gzip/zlib wrappers and output limits. |
| Regex | `Regex_Match`, `Regex_Error`; `find` | Bounded UTF-8 parser and Thompson NFA; Unicode-scalar offsets and no backtracking. |
| SQLite | `Database_Error`; `Database` Actor with `Database_Open`, `Database_Execute`, `Database_Query`, `Database_Close` and completion Messages | Pinned SQLite amalgamation, serialized connection table, bounded one-statement execution, JSON query encoder, and base64 BLOB values. |
| Network | System Actor `Network`; typed `Ip_Address` and `Socket_Address`; TCP, connected UDP, `Dns_Resolve` and completions | Runtime resource IDs, bounded resolver and sockets, TLS transport. |
| HTTP | `Http_Response`, `Http_Problem`; `serialize`, `serialize_keepalive`, `parse`, `parse_eof`, `parse_head`, `parse_head_eof`; `Http_Client`, `Http_Get`, `Http_Completed`, `Http_Close` | Bounded HTTP/1.1 wire parser/serializer and Network-backed Client Actor with same-origin keep-alive reuse. |
| Logging | `Log_Field`, `Log_Event`, `format_text` from `@std/logging/logging.bas` | Pure deterministic formatter; Logger Actor and delivery confirmations remain pending. |
| Secure Random | `Secure_Random_Bytes(count, reply)` and `Secure_Random_Bytes_Completed(Result<List<Int>, String>)` | Runtime uses `getrandom` on Linux and `BCryptGenRandom` on Windows; 64 MiB request cap and no PRNG fallback. |
| Process | No public module/API | ADR-0040 Process Actor design only; Runtime remains pending. |

The JSON Native functions are module-private implementation hooks; consumers
should import `@std/json/json.bas`. `Instant` and `Date_Time` currently use
their own paths (`@std/time/instant.bas` and `@std/time/datetime.bas`) because
the time facade re-exports only `Duration` while source-level exported Record
stability for multiple time records is being resolved.

System Actor Message payloads, stable error codes, resource ceilings, and
shutdown behavior are documented in the module contracts and ADRs. Update this
inventory with every public Prelude or facade change.
