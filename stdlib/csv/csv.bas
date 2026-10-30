IMPORT "@std/collections/list.bas"
IMPORT "@std/text/text.bas"

EXPORT RECORD Csv_Error(code, byte_offset)
EXPORT RECORD Csv_Table(cells, row_offsets)

EXPORT FUNCTION row_count(table)
    RETURN list.length(table.row_offsets) - 1
END FUNCTION

EXPORT FUNCTION field_count(table, row_index)
    IF row_index < 0 OR row_index >= row_count(table)THEN
        RETURN 0
    END IF
    RETURN table.row_offsets[row_index + 1] - table.row_offsets[row_index]
END FUNCTION

EXPORT FUNCTION get(table, row_index, field_index)
    IF row_index < 0 OR row_index >= row_count(table) OR field_index < 0 OR field_index >= field_count(table, row_index)THEN
        RETURN Error(Csv_Error("index_out_of_range", 0))
    END IF
    RETURN Ok(table.cells[table.row_offsets[row_index] + field_index + 1])
END FUNCTION

EXPORT FUNCTION parse(source)
    IF text.byte_length(source) > 67108864 THEN
        RETURN Error(Csv_Error("resource_exhausted", 0))
    END IF
    VAR cells =[""]
    VAR row_offsets =[0]
    VAR field = ""
    VAR index = 0
    VAR field_bytes = 0
    VAR row_bytes = 0
    VAR row_cells = 0
    VAR quoted = FALSE
    VAR after_quote = FALSE
    VAR field_started = FALSE
    VAR record_started = FALSE
    WHILE index < text.length(source)
        character = text.slice(source, index, 1)
        IF quoted THEN
            IF character = "\"" THEN
                IF index + 1 < text.length(source) AND text.slice(source, index + 1, 1) = "\"" THEN
                    field = field + "\""
                    field_bytes = field_bytes + 1
                    row_bytes = row_bytes + 1
                    index = index + 2
                ELSE
                    quoted = FALSE
                    after_quote = TRUE
                    index = index + 1
                END IF
            ELSE
                field = field + character
                field_bytes = field_bytes + text.byte_length(character)
                row_bytes = row_bytes + text.byte_length(character)
                index = index + 1
            END IF
            IF field_bytes > 1048576 OR row_bytes > 8388608 THEN
                RETURN Error(Csv_Error("resource_exhausted", text.byte_length(text.slice(source, 0, index))))
            END IF
        ELSE IF after_quote THEN
            IF character = "," THEN
                cells = cells +[field]
                row_cells = row_cells + 1
                field = ""
                field_bytes = 0
                after_quote = FALSE
                field_started = FALSE
                record_started = TRUE
                index = index + 1
            ELSE IF character = "\n" OR character = "\r" THEN
                cells = cells +[field]
                row_cells = row_cells + 1
                row_offsets = row_offsets +[row_offsets[list.length(row_offsets) - 1] + row_cells]
                IF list.length(row_offsets) > 100001 THEN
                    RETURN Error(Csv_Error("resource_exhausted", text.byte_length(text.slice(source, 0, index))))
                END IF
                row_cells = 0
                field = ""
                field_bytes = 0
                row_bytes = 0
                after_quote = FALSE
                field_started = FALSE
                record_started = FALSE
                IF character = "\r" AND index + 1 < text.length(source) AND text.slice(source, index + 1, 1) = "\n" THEN
                    index = index + 2
                ELSE
                    index = index + 1
                END IF
            ELSE
                RETURN Error(Csv_Error("malformed_quote", text.byte_length(text.slice(source, 0, index))))
            END IF
        ELSE IF character = "," THEN
            cells = cells +[field]
            row_cells = row_cells + 1
            field = ""
            field_bytes = 0
            field_started = FALSE
            record_started = TRUE
            index = index + 1
        ELSE IF character = "\n" OR character = "\r" THEN
            cells = cells +[field]
            row_cells = row_cells + 1
            row_offsets = row_offsets +[row_offsets[list.length(row_offsets) - 1] + row_cells]
            IF list.length(row_offsets) > 100001 THEN
                RETURN Error(Csv_Error("resource_exhausted", text.byte_length(text.slice(source, 0, index))))
            END IF
            row_cells = 0
            field = ""
            field_bytes = 0
            row_bytes = 0
            field_started = FALSE
            record_started = FALSE
            IF character = "\r" AND index + 1 < text.length(source) AND text.slice(source, index + 1, 1) = "\n" THEN
                index = index + 2
            ELSE
                index = index + 1
            END IF
        ELSE IF character = "\"" THEN
            IF field_started OR field <> "" THEN
                RETURN Error(Csv_Error("malformed_quote", text.byte_length(text.slice(source, 0, index))))
            END IF
            quoted = TRUE
            field_started = TRUE
            record_started = TRUE
            index = index + 1
        ELSE
            field = field + character
            field_started = TRUE
            record_started = TRUE
            field_bytes = field_bytes + text.byte_length(character)
            row_bytes = row_bytes + text.byte_length(character)
            IF field_bytes > 1048576 OR row_bytes > 8388608 THEN
                RETURN Error(Csv_Error("resource_exhausted", text.byte_length(text.slice(source, 0, index))))
            END IF
            index = index + 1
        END IF
    END WHILE
    IF quoted THEN
        RETURN Error(Csv_Error("malformed_quote", text.byte_length(source)))
    END IF
    IF after_quote OR record_started OR row_cells > 0 THEN
        cells = cells +[field]
        row_cells = row_cells + 1
        row_offsets = row_offsets +[row_offsets[list.length(row_offsets) - 1] + row_cells]
        IF list.length(row_offsets) > 100001 THEN
            RETURN Error(Csv_Error("resource_exhausted", text.byte_length(source)))
        END IF
    END IF
    RETURN Ok(Csv_Table(cells, row_offsets))
END FUNCTION

EXPORT FUNCTION stringify(table)
    IF row_count(table) > 100000 THEN
        RETURN Error(Csv_Error("resource_exhausted", 0))
    END IF
    VAR output = ""
    VAR row_index = 0
    WHILE row_index < row_count(table)
        IF row_index > 0 THEN
            output = output + "\r\n"
        END IF
        VAR field_index = 0
        VAR row_bytes = 0
        WHILE field_index < field_count(table, row_index)
            value = table.cells[table.row_offsets[row_index] + field_index + 1]
            row_bytes = row_bytes + text.byte_length(value)
            IF text.byte_length(value) > 1048576 OR row_bytes > 8388608 THEN
                RETURN Error(Csv_Error("resource_exhausted", text.byte_length(output)))
            END IF
            IF field_index > 0 THEN
                output = output + ","
            END IF
            VAR quote = text.find(value, ",") >= 0 OR text.find(value, "\"") >= 0 OR text.find(value, "\r") >= 0 OR text.find(value, "\n") >= 0
            IF quote THEN
                output = output + "\"" + text.replace(value, "\"", "\"\"") + "\""
            ELSE
                output = output + value
            END IF
            IF text.byte_length(output) > 67108864 THEN
                RETURN Error(Csv_Error("resource_exhausted", 0))
            END IF
            field_index = field_index + 1
        END WHILE
        row_index = row_index + 1
    END WHILE
    RETURN Ok(output)
END FUNCTION
