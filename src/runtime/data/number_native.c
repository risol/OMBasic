#if !defined(_WIN32)
#define _GNU_SOURCE 1
#endif

#include <errno.h>
#include <locale.h>
#include <math.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "ombasic/foundation/memory.h"

enum {
    NUMBER_OK = 0,
    NUMBER_EMPTY = 1,
    NUMBER_INVALID_DIGIT = 2,
    NUMBER_INVALID_RADIX = 3,
    NUMBER_OUT_OF_RANGE = 4,
    NUMBER_NON_FINITE = 5,
    NUMBER_INVALID_FORMAT = 6
};

typedef struct Number_Float_Result {
    double value;
    int64_t offset;
    int64_t status;
} Number_Float_Result;

static bool float_grammar(const char *text, size_t *error_offset)
{
    size_t index = 0;
    if (text[index] == '+' || text[index] == '-') ++index;
    const size_t integer_start = index;
    while (text[index] >= '0' && text[index] <= '9') ++index;
    const bool has_integer = index != integer_start;
    bool has_fraction = false;
    if (text[index] == '.') {
        ++index;
        const size_t fraction_start = index;
        while (text[index] >= '0' && text[index] <= '9') ++index;
        has_fraction = index != fraction_start;
    }
    if (!has_integer && !has_fraction) {
        *error_offset = index;
        return false;
    }
    if (text[index] == 'e' || text[index] == 'E') {
        ++index;
        if (text[index] == '+' || text[index] == '-') ++index;
        const size_t exponent_start = index;
        while (text[index] >= '0' && text[index] <= '9') ++index;
        if (index == exponent_start) {
            *error_offset = index;
            return false;
        }
    }
    if (text[index] != '\0') {
        *error_offset = index;
        return false;
    }
    return true;
}

static bool ascii_equal_ignore_case(const char *left, const char *right)
{
    size_t index = 0;
    while (left[index] != '\0' && right[index] != '\0') {
        unsigned char a = (unsigned char)left[index];
        unsigned char b = (unsigned char)right[index];
        if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = (unsigned char)(b + ('a' - 'A'));
        if (a != b) return false;
        ++index;
    }
    return left[index] == right[index];
}

static bool is_non_finite_token(const char *text)
{
    if (*text == '+' || *text == '-') ++text;
    return ascii_equal_ignore_case(text, "nan")
        || ascii_equal_ignore_case(text, "inf")
        || ascii_equal_ignore_case(text, "infinity");
}

static Number_Float_Result parse_float_value(const char *text)
{
    Number_Float_Result result = {0.0, 0, NUMBER_OK};
    if (text == NULL || text[0] == '\0') {
        result.status = NUMBER_EMPTY;
        return result;
    }
    if (is_non_finite_token(text)) {
        result.status = NUMBER_NON_FINITE;
        result.offset = (int64_t)strlen(text);
        return result;
    }
    size_t error_offset = 0;
    if (!float_grammar(text, &error_offset)) {
        result.status = NUMBER_INVALID_FORMAT;
        result.offset = (int64_t)error_offset;
        return result;
    }
#if defined(_WIN32)
    _locale_t c_locale = _create_locale(LC_NUMERIC, "C");
    if (c_locale == NULL) {
        result.status = NUMBER_OUT_OF_RANGE;
        return result;
    }
    char *end = NULL;
    errno = 0;
    const double parsed = _strtod_l(text, &end, c_locale);
    _free_locale(c_locale);
#else
    locale_t c_locale = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
    if (c_locale == (locale_t)0) {
        result.status = NUMBER_OUT_OF_RANGE;
        return result;
    }
    char *end = NULL;
    errno = 0;
    const double parsed = strtod_l(text, &end, c_locale);
    freelocale(c_locale);
#endif
    if (end == text || *end != '\0' || !isfinite(parsed)) {
        result.status = NUMBER_NON_FINITE;
        result.offset = (int64_t)strlen(text);
        return result;
    }
    result.value = parsed;
    return result;
}

int64_t om_runtime_number_parse_float_status(const char *text)
{
    return parse_float_value(text).status;
}

int64_t om_runtime_number_parse_float_offset(const char *text)
{
    return parse_float_value(text).offset;
}

double om_runtime_number_parse_float_value(const char *text)
{
    return parse_float_value(text).value;
}

bool om_runtime_number_float_is_finite(double value)
{
    return isfinite(value) != 0;
}

char *om_runtime_number_format_float(double value)
{
    if (!isfinite(value)) return NULL;
    char buffer[64];
    int length = -1;
#if defined(_WIN32)
    _locale_t c_locale = _create_locale(LC_NUMERIC, "C");
    if (c_locale == NULL) return NULL;
    for (int precision = 1; precision <= 17; ++precision) {
        length = _snprintf_s_l(buffer, sizeof(buffer), _TRUNCATE, "%.*g",
                               c_locale, precision, value);
        if (length < 0 || (size_t)length >= sizeof(buffer)) continue;
        char *end = NULL;
        const double parsed = _strtod_l(buffer, &end, c_locale);
        uint64_t original_bits = 0;
        uint64_t parsed_bits = 0;
        (void)memcpy(&original_bits, &value, sizeof(original_bits));
        (void)memcpy(&parsed_bits, &parsed, sizeof(parsed_bits));
        if (end != buffer && *end == '\0' && original_bits == parsed_bits) break;
    }
    _free_locale(c_locale);
#else
    locale_t c_locale = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
    if (c_locale == (locale_t)0) return NULL;
    locale_t previous = uselocale(c_locale);
    for (int precision = 1; precision <= 17; ++precision) {
        length = snprintf(buffer, sizeof(buffer), "%.*g", precision, value);
        if (length < 0 || (size_t)length >= sizeof(buffer)) continue;
        char *end = NULL;
        const double parsed = strtod_l(buffer, &end, c_locale);
        uint64_t original_bits = 0;
        uint64_t parsed_bits = 0;
        (void)memcpy(&original_bits, &value, sizeof(original_bits));
        (void)memcpy(&parsed_bits, &parsed, sizeof(parsed_bits));
        if (end != buffer && *end == '\0' && original_bits == parsed_bits) break;
    }
    (void)uselocale(previous);
    freelocale(c_locale);
#endif
    if (length < 0 || (size_t)length >= sizeof(buffer)) return NULL;
    char *output = om_alloc((size_t)length + 1U);
    if (output == NULL) return NULL;
    (void)memcpy(output, buffer, (size_t)length + 1U);
    return output;
}

typedef struct Number_Int_Result {
    int64_t value;
    int64_t offset;
    int64_t status;
} Number_Int_Result;

static int digit_value(unsigned char byte)
{
    if (byte >= '0' && byte <= '9') return (int)(byte - '0');
    if (byte >= 'a' && byte <= 'z') return (int)(byte - 'a') + 10;
    if (byte >= 'A' && byte <= 'Z') return (int)(byte - 'A') + 10;
    return -1;
}

static Number_Int_Result parse_integer(const char *text, int64_t radix)
{
    Number_Int_Result result = {0, 0, NUMBER_OK};
    if (radix < 2 || radix > 36) {
        result.status = NUMBER_INVALID_RADIX;
        return result;
    }
    if (text == NULL || text[0] == '\0') {
        result.status = NUMBER_EMPTY;
        return result;
    }
    size_t index = 0;
    bool negative = false;
    if (text[index] == '+' || text[index] == '-') {
        negative = text[index] == '-';
        ++index;
    }
    if (text[index] == '\0') {
        result.status = NUMBER_INVALID_DIGIT;
        result.offset = (int64_t)index;
        return result;
    }
    const int64_t base = radix;
    const int64_t limit = negative ? INT64_MIN : -INT64_MAX;
    const int64_t multiply_limit = limit / base;
    int64_t value = 0;
    for (; text[index] != '\0'; ++index) {
        const int digit = digit_value((unsigned char)text[index]);
        if (digit < 0 || digit >= radix) {
            result.status = NUMBER_INVALID_DIGIT;
            result.offset = (int64_t)index;
            return result;
        }
        if (value < multiply_limit) {
            result.status = NUMBER_OUT_OF_RANGE;
            result.offset = (int64_t)index;
            return result;
        }
        value *= base;
        if (value < limit + digit) {
            result.status = NUMBER_OUT_OF_RANGE;
            result.offset = (int64_t)index;
            return result;
        }
        value -= digit;
    }
    result.value = negative ? value : -value;
    return result;
}

int64_t om_runtime_number_parse_int_status(const char *text, int64_t radix)
{
    return parse_integer(text, radix).status;
}

int64_t om_runtime_number_parse_int_offset(const char *text, int64_t radix)
{
    return parse_integer(text, radix).offset;
}

int64_t om_runtime_number_parse_int_value(const char *text, int64_t radix)
{
    return parse_integer(text, radix).value;
}

char *om_runtime_number_format_int(int64_t value, int64_t radix)
{
    if (radix < 2 || radix > 36) return NULL;
    static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    char reversed[66];
    size_t length = 0;
    const bool negative = value < 0;
    uint64_t magnitude = negative
        ? (uint64_t)(-(value + 1)) + UINT64_C(1) : (uint64_t)value;
    do {
        reversed[length++] = digits[magnitude % (uint64_t)radix];
        magnitude /= (uint64_t)radix;
    } while (magnitude != 0);
    const size_t sign_length = negative ? 1U : 0U;
    char *output = om_alloc(length + sign_length + 1U);
    if (output == NULL) return NULL;
    size_t position = 0;
    if (negative) output[position++] = '-';
    while (length != 0) output[position++] = reversed[--length];
    output[position] = '\0';
    return output;
}

void om_runtime_number_release(void *value)
{
    om_free(value);
}
