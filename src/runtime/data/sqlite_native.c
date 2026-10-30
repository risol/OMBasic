#include "ombasic/foundation/memory.h"
#include "ombasic/foundation/utf8.h"

#include "sqlite3.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static INIT_ONCE database_mutex_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION database_mutex;
static BOOL CALLBACK initialize_database_mutex(PINIT_ONCE once, PVOID param,
                                               PVOID *context)
{
    (void)once; (void)param; (void)context;
    InitializeCriticalSection(&database_mutex);
    return TRUE;
}
static void lock_databases(void)
{
    (void)InitOnceExecuteOnce(&database_mutex_once,
                              initialize_database_mutex, NULL, NULL);
    EnterCriticalSection(&database_mutex);
}
static void unlock_databases(void) { LeaveCriticalSection(&database_mutex); }
#else
#include <pthread.h>
static pthread_mutex_t database_mutex = PTHREAD_MUTEX_INITIALIZER;
static void lock_databases(void) { (void)pthread_mutex_lock(&database_mutex); }
static void unlock_databases(void) { (void)pthread_mutex_unlock(&database_mutex); }
#endif

enum {
    DATABASE_MAX_CONNECTIONS = 64,
    DATABASE_MAX_PATH_BYTES = 4096,
    DATABASE_MAX_SQL_BYTES = 1048576,
    DATABASE_MAX_RESULT_BYTES = 64 * 1024 * 1024,
    DATABASE_MAX_ROWS = 65536,
};

typedef struct Database_Connection {
    int64_t id;
    sqlite3 *handle;
} Database_Connection;

typedef struct Json_Buffer {
    char *bytes;
    size_t length;
    size_t capacity;
    bool failed;
} Json_Buffer;

static Database_Connection connections[DATABASE_MAX_CONNECTIONS];
static int64_t next_connection_id = 1;

static sqlite3 *find_connection(int64_t id)
{
    if (id <= 0) return NULL;
    for (size_t index = 0; index < DATABASE_MAX_CONNECTIONS; ++index) {
        if (connections[index].id == id) return connections[index].handle;
    }
    return NULL;
}

static const char *database_error(int result)
{
    switch (result & 0xff) {
    case SQLITE_OK: return "ok";
    case SQLITE_BUSY: case SQLITE_LOCKED: return "busy";
    case SQLITE_CONSTRAINT: return "constraint";
    case SQLITE_MISMATCH: case SQLITE_MISUSE: return "invalid_argument";
    case SQLITE_NOMEM: case SQLITE_TOOBIG: case SQLITE_FULL:
        return "resource_exhausted";
    case SQLITE_CANTOPEN: case SQLITE_NOTFOUND: return "not_found";
    case SQLITE_READONLY: case SQLITE_IOERR: case SQLITE_CORRUPT:
    case SQLITE_NOTADB: case SQLITE_PROTOCOL: return "io_error";
    case SQLITE_INTERRUPT: return "cancelled";
    default: return "invalid_sql";
    }
}

static char *format_message(const char *format, const char *code,
                            int64_t first, int64_t second)
{
    char buffer[160];
    const int count = snprintf(buffer, sizeof(buffer), format, code,
                               (long long)first, (long long)second);
    if (count < 0 || (size_t)count >= sizeof(buffer)) return NULL;
    char *value = om_alloc((size_t)count + 1);
    if (value != NULL) (void)memcpy(value, buffer, (size_t)count + 1);
    return value;
}

static char *format_error(int result)
{
    return format_message("error|%s|%lld", database_error(result), result, 0);
}

static char *format_open_id(int64_t id)
{
    char buffer[48];
    const int length = snprintf(buffer, sizeof(buffer), "ok|%lld",
                                (long long)id);
    if (length < 0 || (size_t)length >= sizeof(buffer)) return NULL;
    char *result = om_alloc((size_t)length + 1);
    if (result != NULL) (void)memcpy(result, buffer, (size_t)length + 1);
    return result;
}

static char *format_execute_result(int changes, int64_t row_id)
{
    char buffer[80];
    const int length = snprintf(buffer, sizeof(buffer), "ok|%d|%lld",
                                changes, (long long)row_id);
    if (length < 0 || (size_t)length >= sizeof(buffer)) return NULL;
    char *result = om_alloc((size_t)length + 1);
    if (result != NULL) (void)memcpy(result, buffer, (size_t)length + 1);
    return result;
}

static void json_reserve(Json_Buffer *buffer, size_t additional)
{
    if (buffer->failed || additional > DATABASE_MAX_RESULT_BYTES
        || buffer->length > DATABASE_MAX_RESULT_BYTES - additional) {
        buffer->failed = true;
        return;
    }
    const size_t required = buffer->length + additional + 1;
    if (required <= buffer->capacity) return;
    size_t capacity = buffer->capacity == 0 ? 256 : buffer->capacity;
    while (capacity < required) {
        if (capacity > DATABASE_MAX_RESULT_BYTES / 2) {
            capacity = DATABASE_MAX_RESULT_BYTES + 1;
            break;
        }
        capacity *= 2;
    }
    char *replacement = om_realloc_array(buffer->bytes, capacity, 1);
    if (replacement == NULL) {
        buffer->failed = true;
        return;
    }
    buffer->bytes = replacement;
    buffer->capacity = capacity;
}

static void json_append(Json_Buffer *buffer, const char *value, size_t length)
{
    json_reserve(buffer, length);
    if (buffer->failed) return;
    if (length != 0) (void)memcpy(buffer->bytes + buffer->length, value, length);
    buffer->length += length;
    buffer->bytes[buffer->length] = '\0';
}

static void json_literal(Json_Buffer *buffer, const char *value)
{
    json_append(buffer, value, strlen(value));
}

static void json_integer(Json_Buffer *buffer, int64_t value)
{
    char digits[32];
    const int length = snprintf(digits, sizeof(digits), "%lld",
                                (long long)value);
    if (length < 0 || (size_t)length >= sizeof(digits)) {
        buffer->failed = true;
        return;
    }
    json_append(buffer, digits, (size_t)length);
}

static bool valid_utf8(const unsigned char *bytes, size_t length)
{
    size_t offset = 0;
    while (offset < length) {
        uint32_t scalar = 0;
        if (om_utf8_decode_next((Om_Const_Byte_Slice){bytes, length},
                                &offset, &scalar) != OM_STATUS_OK) {
            return false;
        }
    }
    return true;
}

static void json_string(Json_Buffer *buffer, const unsigned char *value,
                        size_t length)
{
    static const char hex[] = "0123456789abcdef";
    json_literal(buffer, "\"");
    for (size_t index = 0; index < length; ++index) {
        const unsigned char byte = value[index];
        switch (byte) {
        case '"': json_literal(buffer, "\\\""); break;
        case '\\': json_literal(buffer, "\\\\"); break;
        case '\b': json_literal(buffer, "\\b"); break;
        case '\f': json_literal(buffer, "\\f"); break;
        case '\n': json_literal(buffer, "\\n"); break;
        case '\r': json_literal(buffer, "\\r"); break;
        case '\t': json_literal(buffer, "\\t"); break;
        default:
            if (byte < 0x20u) {
                char escaped[6] = {'\\', 'u', '0', '0', hex[byte >> 4],
                                   hex[byte & 0x0fu]};
                json_append(buffer, escaped, sizeof(escaped));
            } else {
                json_append(buffer, (const char *)&byte, 1);
            }
        }
    }
    json_literal(buffer, "\"");
}

static void json_blob(Json_Buffer *buffer, const unsigned char *value,
                      size_t length)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    json_literal(buffer, "{\"$blob\":\"");
    for (size_t index = 0; index < length; index += 3) {
        const size_t remaining = length - index;
        const uint32_t block = ((uint32_t)value[index] << 16)
            | (remaining > 1 ? (uint32_t)value[index + 1] << 8 : 0)
            | (remaining > 2 ? value[index + 2] : 0);
        const char encoded[4] = {
            alphabet[(block >> 18) & 63u], alphabet[(block >> 12) & 63u],
            remaining > 1 ? alphabet[(block >> 6) & 63u] : '=',
            remaining > 2 ? alphabet[block & 63u] : '=',
        };
        json_append(buffer, encoded, sizeof(encoded));
    }
    json_literal(buffer, "\"}");
}

char *om_runtime_database_open(const char *path)
{
    if (path == NULL || path[0] == '\0') return format_error(SQLITE_MISUSE);
    if (strlen(path) > DATABASE_MAX_PATH_BYTES) {
        return format_error(SQLITE_TOOBIG);
    }
    sqlite3 *database = NULL;
    int status = sqlite3_open_v2(path, &database,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        NULL);
    if (status != SQLITE_OK) {
        if (database != NULL) sqlite3_close_v2(database);
        return format_error(status);
    }
    (void)sqlite3_busy_timeout(database, 5000);
    lock_databases();
    size_t free_slot = DATABASE_MAX_CONNECTIONS;
    for (size_t index = 0; index < DATABASE_MAX_CONNECTIONS; ++index) {
        if (connections[index].handle == NULL) {
            free_slot = index;
            break;
        }
    }
    if (free_slot == DATABASE_MAX_CONNECTIONS || next_connection_id <= 0
        || next_connection_id == INT64_MAX) {
        unlock_databases();
        sqlite3_close_v2(database);
        return format_error(SQLITE_FULL);
    }
    const int64_t id = next_connection_id++;
    connections[free_slot] = (Database_Connection){id, database};
    unlock_databases();
    return format_open_id(id);
}

char *om_runtime_database_execute(int64_t id, const char *sql)
{
    if (sql == NULL || strlen(sql) > DATABASE_MAX_SQL_BYTES) {
        return format_error(SQLITE_TOOBIG);
    }
    lock_databases();
    sqlite3 *database = find_connection(id);
    if (database == NULL) {
        unlock_databases();
        return format_error(SQLITE_MISUSE);
    }
    sqlite3_stmt *statement = NULL;
    const char *tail = NULL;
    int status = sqlite3_prepare_v2(database, sql, -1, &statement, &tail);
    if (status == SQLITE_OK && statement == NULL) status = SQLITE_ERROR;
    while (status == SQLITE_OK && tail != NULL && (*tail == ' ' || *tail == '\t'
           || *tail == '\r' || *tail == '\n')) ++tail;
    if (status == SQLITE_OK && tail != NULL && *tail != '\0') {
        status = SQLITE_MISUSE;
    }
    if (status == SQLITE_OK && sqlite3_column_count(statement) != 0) {
        status = SQLITE_MISUSE;
    }
    if (status == SQLITE_OK) {
        status = sqlite3_step(statement);
        if (status == SQLITE_DONE) status = SQLITE_OK;
    }
    if (statement != NULL) {
        const int final_status = sqlite3_finalize(statement);
        if (status == SQLITE_OK && final_status != SQLITE_OK) status = final_status;
    }
    const int changes = sqlite3_changes(database);
    const int64_t row_id = sqlite3_last_insert_rowid(database);
    unlock_databases();
    if (status != SQLITE_OK) return format_error(status);
    return format_execute_result(changes, row_id);
}

char *om_runtime_database_query(int64_t id, const char *sql)
{
    if (sql == NULL || strlen(sql) > DATABASE_MAX_SQL_BYTES) {
        return format_error(SQLITE_TOOBIG);
    }
    lock_databases();
    sqlite3 *database = find_connection(id);
    if (database == NULL) {
        unlock_databases();
        return format_error(SQLITE_MISUSE);
    }
    sqlite3_stmt *statement = NULL;
    const char *tail = NULL;
    int status = sqlite3_prepare_v2(database, sql, -1, &statement, &tail);
    if (status == SQLITE_OK && statement == NULL) status = SQLITE_ERROR;
    while (status == SQLITE_OK && tail != NULL && (*tail == ' ' || *tail == '\t'
           || *tail == '\r' || *tail == '\n')) ++tail;
    if (status == SQLITE_OK && tail != NULL && *tail != '\0') {
        status = SQLITE_MISUSE;
    }
    Json_Buffer output = {0};
    if (status == SQLITE_OK) {
        json_literal(&output, "{\"columns\":[");
        const int column_count = sqlite3_column_count(statement);
        for (int column = 0; column < column_count; ++column) {
            if (column != 0) json_literal(&output, ",");
            const unsigned char *name = (const unsigned char *)
                sqlite3_column_name(statement, column);
            if (name == NULL || !valid_utf8(name, strlen((const char *)name))) {
                status = SQLITE_MISMATCH;
                break;
            }
            json_string(&output, name, strlen((const char *)name));
        }
        json_literal(&output, "],\"rows\":[");
        size_t row_count = 0;
        while (status == SQLITE_OK) {
            const int step_status = sqlite3_step(statement);
            if (step_status == SQLITE_DONE) {
                status = SQLITE_OK;
                break;
            }
            if (step_status != SQLITE_ROW) {
                status = step_status;
                break;
            }
            status = SQLITE_OK;
            if (row_count == DATABASE_MAX_ROWS) {
                status = SQLITE_TOOBIG;
                break;
            }
            if (row_count++ != 0) json_literal(&output, ",");
            json_literal(&output, "[");
            for (int column = 0; column < column_count; ++column) {
                if (column != 0) json_literal(&output, ",");
                switch (sqlite3_column_type(statement, column)) {
                case SQLITE_NULL:
                    json_literal(&output, "null");
                    break;
                case SQLITE_INTEGER:
                    json_integer(&output, sqlite3_column_int64(statement, column));
                    break;
                case SQLITE_FLOAT: {
                    const double value = sqlite3_column_double(statement, column);
                    if (!isfinite(value)) {
                        json_literal(&output, "null");
                    } else {
                        char *number = sqlite3_mprintf("%!.17g", value);
                        if (number == NULL) {
                            status = SQLITE_NOMEM;
                            break;
                        }
                        json_append(&output, number, strlen(number));
                        sqlite3_free(number);
                    }
                    break;
                }
                case SQLITE_TEXT: {
                    const unsigned char *value = sqlite3_column_text(statement, column);
                    const int byte_count = sqlite3_column_bytes(statement, column);
                    if (byte_count < 0 || (value == NULL && byte_count != 0)
                        || !valid_utf8(value, (size_t)byte_count)) {
                        status = SQLITE_MISMATCH;
                        break;
                    }
                    json_string(&output, value, (size_t)byte_count);
                    break;
                }
                case SQLITE_BLOB: {
                    const unsigned char *value = sqlite3_column_blob(statement, column);
                    const int byte_count = sqlite3_column_bytes(statement, column);
                    if (byte_count < 0 || (value == NULL && byte_count != 0)) {
                        status = SQLITE_MISMATCH;
                        break;
                    }
                    json_blob(&output, value, (size_t)byte_count);
                    break;
                }
                }
                if (output.failed) status = SQLITE_TOOBIG;
                if (status != SQLITE_OK) break;
            }
            json_literal(&output, "]");
        }
        json_literal(&output, "]}");
        if (output.failed && status == SQLITE_OK) status = SQLITE_TOOBIG;
    }
    if (statement != NULL) {
        const int final_status = sqlite3_finalize(statement);
        if (status == SQLITE_OK && final_status != SQLITE_OK) status = final_status;
    }
    unlock_databases();
    if (status != SQLITE_OK) {
        om_free(output.bytes);
        return format_error(status);
    }
    const size_t json_length = output.length;
    const size_t prefix_length = 3;
    char *result = om_alloc(prefix_length + json_length + 1);
    if (result == NULL) {
        om_free(output.bytes);
        return NULL;
    }
    (void)memcpy(result, "ok|", prefix_length);
    if (json_length != 0) {
        (void)memcpy(result + prefix_length, output.bytes, json_length);
    }
    result[prefix_length + json_length] = '\0';
    om_free(output.bytes);
    return result;
}

char *om_runtime_database_close(int64_t id)
{
    lock_databases();
    for (size_t index = 0; index < DATABASE_MAX_CONNECTIONS; ++index) {
        if (connections[index].id == id && connections[index].handle != NULL) {
            sqlite3 *database = connections[index].handle;
            connections[index] = (Database_Connection){0};
            const int status = sqlite3_close_v2(database);
            unlock_databases();
            if (status != SQLITE_OK) return format_error(status);
            char *result = om_alloc(3);
            if (result != NULL) (void)memcpy(result, "ok", 3);
            return result;
        }
    }
    unlock_databases();
    return format_error(SQLITE_MISUSE);
}

void om_runtime_database_release(void *value)
{
    om_free(value);
}
