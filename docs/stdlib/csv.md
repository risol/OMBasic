# CSV

`@std/csv/csv.bas` provides a pure UTF-8 CSV parser and serializer using an RFC 4180 baseline. Parsing accepts CRLF and LF record separators, doubled quotes, and newlines inside quoted fields. It returns `Result<Csv_Table, Csv_Error>`; the table stores fields in row order with cumulative row offsets. Use `row_count`, `field_count`, and `get` to read it.

`Csv_Error.code` is a stable code. `byte_offset` is a zero-based UTF-8 byte offset. The parser rejects quotes in unquoted fields, characters after a closing quote, and unterminated quoted fields.

The v1 bounds are 64 MiB per input/output, 1 MiB per field, 8 MiB per record, and 100,000 records. `stringify` emits CRLF between records and quotes fields containing commas, quotes, CR, or LF. Quote characters inside a quoted field are doubled.
