/*
 * Typed values of schema-informed EXI streams (EXI 1.0, 7.1 and 7.2;
 * D333): Binary, Boolean, Decimal, Float, Integer (sign and magnitude,
 * Unsigned Integer, n-bit offset), Date-Time and Enumeration, from text
 * and back. Golden: src/exi_stream.c (the representations under these).
 *
 * Which texts are values, and the text a value decodes to, are the
 * oracle's (EXIficient 1.0.4), whose streams these must reproduce bit for
 * bit: a value is valid when its representation can hold it, read by the
 * oracle's lexical rules (the text trimmed, signs and digits as Java reads
 * them), with no facet checked but the bounds of an n-bit integer and the
 * members of an enumeration; a decoded value is written as the oracle
 * writes it ("123.0" for a decimal 123, "555E-2" for a float 5.55,
 * fraction digits without trailing zeros). Integers have any number of
 * digits (decimal digit strings, divided and multiplied by 128 for the
 * 7-bit groups of an Unsigned Integer, 7.1.6).
 */
#include <stdalign.h>
#include <string.h>

#include "exi_internal.h"
#include "memory_internal.h"

#define SXML_EXI_DATATYPE_TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

enum {
    /* 7.1.4: the exponent of INF, -INF and NaN, and the exponent's
       range. */
    SXML_EXI_FLOAT_SPECIAL_EXPONENT = -16384,
    SXML_EXI_FLOAT_EXPONENT_LIMIT = 16383,
    /* 7.1.8, Table 7-3. */
    SXML_EXI_YEAR_OFFSET = 2000,
    SXML_EXI_MONTH_DAY_BITS = 9,
    SXML_EXI_TIME_BITS = 17,
    SXML_EXI_TIME_ZONE_BITS = 11,
    SXML_EXI_TIME_ZONE_OFFSET = 896,
    /* The most digits a fraction of seconds may have to fit the oracle's
       32-bit int, reversed. */
    SXML_EXI_FRACTION_LIMIT = 2147483647,
};

/* ------------------------------------------------------------ Sets */

/* Table 7-2, each set ascending. */
static const uint32_t sxml_exi_base64_set[] = {0x9, 0xA, 0xD, 0x20, '+', '/', '0', '1', '2', '3', '4', '5', '6', '7',
                                               '8', '9', '=', 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K',
                                               'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y',
                                               'Z', 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm',
                                               'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z'};
static const uint32_t sxml_exi_hex_set[] = {0x9, 0xA, 0xD, 0x20, '0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
                                            'A', 'B', 'C', 'D', 'E', 'F', 'a', 'b', 'c', 'd', 'e', 'f'};
static const uint32_t sxml_exi_boolean_set[] = {0x9, 0xA, 0xD, 0x20, '0', '1', 'a', 'e', 'f', 'l', 'r', 's', 't', 'u'};
static const uint32_t sxml_exi_date_time_set[] = {0x9, 0xA, 0xD, 0x20, '+', '-', '.', '0', '1', '2', '3',
                                                  '4', '5', '6', '7', '8', '9', ':', 'T', 'Z'};
static const uint32_t sxml_exi_decimal_set[] = {0x9, 0xA, 0xD, 0x20, '+', '-', '.', '0',
                                                '1', '2', '3', '4', '5', '6', '7', '8', '9'};
static const uint32_t sxml_exi_double_set[] = {0x9, 0xA, 0xD, 0x20, '+', '-', '.', '0', '1', '2', '3', '4',
                                               '5', '6', '7', '8', '9', 'E', 'F', 'I', 'N', 'a', 'e'};
static const uint32_t sxml_exi_integer_set[] = {0x9, 0xA, 0xD, 0x20, '+', '-', '0', '1',
                                                '2', '3', '4', '5', '6', '7', '8', '9'};

#define SXML_EXI_SET(array) ((sxml_exi_character_set){.characters = (array), .character_count = sizeof(array) / sizeof((array)[0])})

sxml_exi_character_set sxml_exi_lexical_character_set(sxml_exi_lexical_set lexical_set) {
    switch (lexical_set) {
    case SXML_EXI_LEXICAL_SET_BASE64_BINARY:
        return SXML_EXI_SET(sxml_exi_base64_set);
    case SXML_EXI_LEXICAL_SET_HEX_BINARY:
        return SXML_EXI_SET(sxml_exi_hex_set);
    case SXML_EXI_LEXICAL_SET_BOOLEAN:
        return SXML_EXI_SET(sxml_exi_boolean_set);
    case SXML_EXI_LEXICAL_SET_DATE_TIME:
        return SXML_EXI_SET(sxml_exi_date_time_set);
    case SXML_EXI_LEXICAL_SET_DECIMAL:
        return SXML_EXI_SET(sxml_exi_decimal_set);
    case SXML_EXI_LEXICAL_SET_DOUBLE:
        return SXML_EXI_SET(sxml_exi_double_set);
    case SXML_EXI_LEXICAL_SET_INTEGER:
        return SXML_EXI_SET(sxml_exi_integer_set);
    case SXML_EXI_LEXICAL_SET_NONE:
        break;
    }
    return (sxml_exi_character_set){.characters = NULL, .character_count = 0};
}

sxml_exi_lexical_set sxml_exi_datatype_lexical_set(const sxml_exi_schema_grammars *grammars, uint32_t datatype) {
    /* An enumeration's and a list's are their base's (7.1.10.1, Table
       7-2); the walk is bounded by the number of datatypes. */
    uint32_t current = datatype;
    for (size_t step_index = 0; step_index <= grammars->datatype_count && current != SXML_EXI_NONE; step_index++) {
        const sxml_exi_datatype *entry = &grammars->datatypes[current];
        switch (entry->datatype_kind) {
        case SXML_EXI_DATATYPE_STRING:
        case SXML_EXI_DATATYPE_RESTRICTED_STRING:
            return SXML_EXI_LEXICAL_SET_NONE;
        case SXML_EXI_DATATYPE_BASE64_BINARY:
            return SXML_EXI_LEXICAL_SET_BASE64_BINARY;
        case SXML_EXI_DATATYPE_HEX_BINARY:
            return SXML_EXI_LEXICAL_SET_HEX_BINARY;
        case SXML_EXI_DATATYPE_BOOLEAN:
        case SXML_EXI_DATATYPE_PATTERN_BOOLEAN:
            return SXML_EXI_LEXICAL_SET_BOOLEAN;
        case SXML_EXI_DATATYPE_DECIMAL:
            return SXML_EXI_LEXICAL_SET_DECIMAL;
        case SXML_EXI_DATATYPE_FLOAT:
            return SXML_EXI_LEXICAL_SET_DOUBLE;
        case SXML_EXI_DATATYPE_INTEGER:
        case SXML_EXI_DATATYPE_UNSIGNED_INTEGER:
        case SXML_EXI_DATATYPE_BOUNDED_INTEGER:
            return SXML_EXI_LEXICAL_SET_INTEGER;
        case SXML_EXI_DATATYPE_DATE_TIME:
            return SXML_EXI_LEXICAL_SET_DATE_TIME;
        case SXML_EXI_DATATYPE_LIST:
        case SXML_EXI_DATATYPE_ENUMERATION:
            current = entry->base_datatype;
            break;
        }
    }
    return SXML_EXI_LEXICAL_SET_NONE;
}

uint32_t sxml_exi_character_index(sxml_exi_character_set set, uint32_t codepoint) {
    size_t low = 0;
    size_t high = set.character_count;
    while (low < high) {
        const size_t middle = low + (high - low) / 2;
        if (set.characters[middle] < codepoint) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low < set.character_count && set.characters[low] == codepoint ? (uint32_t)low
                                                                          : (uint32_t)set.character_count;
}

/* -------------------------------------------------------- Whitespace */

static bool sxml_exi_is_space(uint8_t byte) {
    return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r';
}

/* Java's String.trim: every character up to U+0020 at either end. */
static sxml_byte_view sxml_exi_java_trim(sxml_byte_view text) {
    while (text.byte_count > 0 && text.bytes[0] <= 0x20) {
        text.bytes++;
        text.byte_count--;
    }
    while (text.byte_count > 0 && text.bytes[text.byte_count - 1] <= 0x20) {
        text.byte_count--;
    }
    return text;
}

sxml_status sxml_exi_apply_whitespace(sxml_exi_whitespace_request request, sxml_exi_buffer *output) {
    output->byte_count = 0;
    sxml_status status = SXML_STATUS_OK;
    if (request.whitespace == SXML_EXI_WHITESPACE_PRESERVE) {
        return sxml_exi_buffer_append(output, request.text);
    }
    bool is_pending_space = false;
    for (size_t byte_index = 0; byte_index < request.text.byte_count && status == SXML_STATUS_OK; byte_index++) {
        uint8_t byte = request.text.bytes[byte_index];
        if (sxml_exi_is_space(byte)) {
            byte = ' ';
        }
        if (request.whitespace == SXML_EXI_WHITESPACE_COLLAPSE && byte == ' ') {
            is_pending_space = output->byte_count > 0;
            continue;
        }
        if (is_pending_space) {
            status = sxml_exi_buffer_append(output, SXML_EXI_DATATYPE_TEXT(" "));
            is_pending_space = false;
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_buffer_append(output, (sxml_byte_view){.bytes = &byte, .byte_count = 1});
        }
    }
    return status;
}

bool sxml_exi_next_list_item(sxml_byte_view text, size_t *offset, sxml_byte_view *item) {
    /* java.util.StringTokenizer's delimiters: space, tab, LF, CR, FF. */
    size_t position = *offset;
    while (position < text.byte_count && (sxml_exi_is_space(text.bytes[position]) || text.bytes[position] == 0x0C)) {
        position++;
    }
    if (position >= text.byte_count) {
        *offset = position;
        return false;
    }
    const size_t start = position;
    while (position < text.byte_count && !sxml_exi_is_space(text.bytes[position]) && text.bytes[position] != 0x0C) {
        position++;
    }
    *item = (sxml_byte_view){.bytes = text.bytes + start, .byte_count = position - start};
    *offset = position;
    return true;
}

/* ----------------------------------------------------- Decimal digits */

/*
 * Integers of any size are decimal digit strings in a buffer: the
 * magnitude's digits, most significant first, without leading zeros ("0"
 * for zero).
 */

static bool sxml_exi_is_digit(uint8_t byte) {
    return byte >= '0' && byte <= '9';
}

static sxml_byte_view sxml_exi_buffer_view(const sxml_exi_buffer *buffer) {
    return (sxml_byte_view){.bytes = buffer->bytes, .byte_count = buffer->byte_count};
}

/* digits without leading zeros, appended to output. */
static sxml_status sxml_exi_append_magnitude(sxml_exi_buffer *output, sxml_byte_view digits) {
    while (digits.byte_count > 1 && digits.bytes[0] == '0') {
        digits.bytes++;
        digits.byte_count--;
    }
    if (digits.byte_count == 0) {
        return sxml_exi_buffer_append(output, SXML_EXI_DATATYPE_TEXT("0"));
    }
    return sxml_exi_buffer_append(output, digits);
}

static bool sxml_exi_is_zero(sxml_byte_view digits) {
    for (size_t digit_index = 0; digit_index < digits.byte_count; digit_index++) {
        if (digits.bytes[digit_index] != '0') {
            return false;
        }
    }
    return true;
}

/* Compares magnitudes without leading zeros. */
static int sxml_exi_compare_magnitudes(sxml_byte_view first, sxml_byte_view second) {
    if (first.byte_count != second.byte_count) {
        return first.byte_count < second.byte_count ? -1 : 1;
    }
    const int order = first.byte_count == 0 ? 0 : memcmp(first.bytes, second.bytes, first.byte_count);
    return order < 0 ? -1 : order > 0 ? 1 : 0;
}

/* A signed integer: its sign and the offset and length of its magnitude
   in a buffer. */
typedef struct sxml_exi_signed {
    bool is_negative;
    size_t offset;
    size_t digit_count;
} sxml_exi_signed;

static sxml_byte_view sxml_exi_magnitude(const sxml_exi_buffer *buffer, sxml_exi_signed number) {
    return (sxml_byte_view){.bytes = buffer->bytes + number.offset, .byte_count = number.digit_count};
}

/* Reads a Java integer literal (BigInteger, Long.parseLong): an optional
   sign then at least one ASCII digit; appends its magnitude to work. */
typedef struct sxml_exi_integer_parse {
    sxml_byte_view text;
    sxml_exi_buffer *work;
} sxml_exi_integer_parse;

/* What a parse of a signed integer gives: the number, when valid. */
typedef struct sxml_exi_signed_result {
    sxml_exi_signed number;
    bool is_valid;
} sxml_exi_signed_result;

static sxml_status sxml_exi_parse_signed(const sxml_exi_integer_parse *parse, sxml_exi_signed_result *result) {
    sxml_byte_view text = parse->text;
    result->is_valid = false;
    sxml_exi_signed *number = &result->number;
    *number = (sxml_exi_signed){0};
    if (text.byte_count > 0 && (text.bytes[0] == '-' || text.bytes[0] == '+')) {
        number->is_negative = text.bytes[0] == '-';
        text.bytes++;
        text.byte_count--;
    }
    if (text.byte_count == 0) {
        return SXML_STATUS_OK;
    }
    for (size_t digit_index = 0; digit_index < text.byte_count; digit_index++) {
        if (!sxml_exi_is_digit(text.bytes[digit_index])) {
            return SXML_STATUS_OK;
        }
    }
    number->offset = parse->work->byte_count;
    sxml_status status = sxml_exi_append_magnitude(parse->work, text);
    number->digit_count = parse->work->byte_count - number->offset;
    if (sxml_exi_is_zero(sxml_exi_magnitude(parse->work, *number))) {
        number->is_negative = false;
    }
    result->is_valid = status == SXML_STATUS_OK;
    return status;
}

/* IntegerValue.parse: the text trimmed, one leading '+' dropped, then a
   Java integer literal. */
static sxml_status sxml_exi_parse_integer_value(const sxml_exi_integer_parse *parse, sxml_exi_signed_result *result) {
    sxml_byte_view text = sxml_exi_java_trim(parse->text);
    if (text.byte_count > 0 && text.bytes[0] == '+') {
        text.bytes++;
        text.byte_count--;
    }
    return sxml_exi_parse_signed(&(sxml_exi_integer_parse){.text = text, .work = parse->work}, result);
}

/* Divides the digits in place by divisor (at most 2^24) and returns the
   remainder; the digits keep their length (leading zeros). */
typedef struct sxml_exi_division {
    uint8_t *digits;
    size_t digit_count;
    uint32_t divisor;
} sxml_exi_division;

static uint32_t sxml_exi_divide(const sxml_exi_division *division) {
    uint32_t remainder = 0;
    for (size_t digit_index = 0; digit_index < division->digit_count; digit_index++) {
        const uint32_t current = remainder * 10U + (uint32_t)(division->digits[digit_index] - '0');
        division->digits[digit_index] = (uint8_t)('0' + current / division->divisor);
        remainder = current % division->divisor;
    }
    return remainder;
}

/* Writes a magnitude as an Unsigned Integer (7.1.6): 7-bit groups, least
   significant first, by division by 128 of a copy in work. */
typedef struct sxml_exi_magnitude_write {
    sxml_exi_bit_writer *writer;
    sxml_byte_view digits;
    sxml_exi_buffer *work;
} sxml_exi_magnitude_write;

static sxml_status sxml_exi_write_magnitude(const sxml_exi_magnitude_write *write) {
    const size_t start = write->work->byte_count;
    sxml_status status = sxml_exi_buffer_append(write->work, write->digits);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uint8_t *digits = write->work->bytes + start;
    const size_t digit_count = write->digits.byte_count;
    size_t first_nonzero = 0;
    for (;;) {
        while (first_nonzero < digit_count && digits[first_nonzero] == '0') {
            first_nonzero++;
        }
        const uint32_t group = sxml_exi_divide(&(sxml_exi_division){
            .digits = digits + first_nonzero, .digit_count = digit_count - first_nonzero, .divisor = 128});
        while (first_nonzero < digit_count && digits[first_nonzero] == '0') {
            first_nonzero++;
        }
        const bool has_more = first_nonzero < digit_count;
        status = sxml_exi_write_bits(write->writer, (sxml_exi_bits){.value = group | (has_more ? 0x80U : 0U), .bit_count = 8});
        if (status != SXML_STATUS_OK || !has_more) {
            break;
        }
    }
    write->work->byte_count = start;
    return status;
}

/* Reads an Unsigned Integer of any size (7.1.6) and appends its decimal
   digits to output: the groups first, then digits = digits * 128 + group
   from the most significant group. */
typedef struct sxml_exi_magnitude_read {
    sxml_exi_bit_reader *reader;
    sxml_exi_buffer *output;
    sxml_exi_buffer *work;
} sxml_exi_magnitude_read;

static sxml_status sxml_exi_read_magnitude(const sxml_exi_magnitude_read *read) {
    const size_t group_start = read->work->byte_count;
    sxml_status status = SXML_STATUS_OK;
    for (;;) {
        uint32_t octet = 0;
        status = sxml_exi_read_bits(read->reader, (sxml_exi_bits_request){.bit_count = 8}, &octet);
        const uint8_t group = (uint8_t)(octet & 0x7FU);
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_buffer_append(read->work, (sxml_byte_view){.bytes = &group, .byte_count = 1});
        }
        if (status != SXML_STATUS_OK || (octet & 0x80U) == 0) {
            break;
        }
        /* Every 7 bits give at most three digits: bound the work. */
        if (read->work->byte_count - group_start > 1U << 20) {
            return SXML_STATUS_LIMIT_EXCEEDED;
        }
    }
    if (status != SXML_STATUS_OK) {
        read->work->byte_count = group_start;
        return status;
    }
    const size_t group_count = read->work->byte_count - group_start;
    const size_t digit_start = read->output->byte_count;
    status = sxml_exi_buffer_append(read->output, SXML_EXI_DATATYPE_TEXT("0"));
    for (size_t group_index = group_count; group_index > 0 && status == SXML_STATUS_OK; group_index--) {
        const uint32_t group = read->work->bytes[group_start + group_index - 1];
        /* digits = digits * 128 + group, from the least significant digit. */
        uint32_t carry = group;
        for (size_t digit_index = read->output->byte_count; digit_index > digit_start; digit_index--) {
            const uint32_t current = (uint32_t)(read->output->bytes[digit_index - 1] - '0') * 128U + carry;
            read->output->bytes[digit_index - 1] = (uint8_t)('0' + current % 10U);
            carry = current / 10U;
        }
        while (carry > 0 && status == SXML_STATUS_OK) {
            const uint8_t digit = (uint8_t)('0' + carry % 10U);
            carry /= 10U;
            status = sxml_exi_buffer_append(read->output, (sxml_byte_view){.bytes = &digit, .byte_count = 1});
            if (status == SXML_STATUS_OK) {
                memmove(read->output->bytes + digit_start + 1, read->output->bytes + digit_start,
                        read->output->byte_count - digit_start - 1);
                read->output->bytes[digit_start] = digit;
            }
        }
    }
    read->work->byte_count = group_start;
    if (status == SXML_STATUS_OK) {
        /* Leading zeros of the start: keep one digit at least. */
        size_t leading = 0;
        const size_t digit_count = read->output->byte_count - digit_start;
        while (leading + 1 < digit_count && read->output->bytes[digit_start + leading] == '0') {
            leading++;
        }
        if (leading > 0) {
            memmove(read->output->bytes + digit_start, read->output->bytes + digit_start + leading, digit_count - leading);
            read->output->byte_count -= leading;
        }
    }
    return status;
}

/* Adds or subtracts one from a magnitude in place at buffer[offset..]
   (subtraction of a nonzero one); the magnitude stays without leading
   zeros. */
typedef struct sxml_exi_step_request {
    sxml_exi_buffer *buffer;
    size_t offset;
    bool is_increment;
} sxml_exi_step_request;

static sxml_status sxml_exi_step_magnitude(const sxml_exi_step_request *request) {
    sxml_exi_buffer *buffer = request->buffer;
    size_t digit_index = buffer->byte_count;
    if (request->is_increment) {
        while (digit_index > request->offset) {
            if (buffer->bytes[digit_index - 1] != '9') {
                buffer->bytes[digit_index - 1]++;
                return SXML_STATUS_OK;
            }
            buffer->bytes[digit_index - 1] = '0';
            digit_index--;
        }
        sxml_status status = sxml_exi_buffer_append(buffer, SXML_EXI_DATATYPE_TEXT("0"));
        if (status == SXML_STATUS_OK) {
            memmove(buffer->bytes + request->offset + 1, buffer->bytes + request->offset,
                    buffer->byte_count - request->offset - 1);
            buffer->bytes[request->offset] = '1';
        }
        return status;
    }
    while (digit_index > request->offset) {
        if (buffer->bytes[digit_index - 1] != '0') {
            buffer->bytes[digit_index - 1]--;
            break;
        }
        buffer->bytes[digit_index - 1] = '9';
        digit_index--;
    }
    if (buffer->byte_count - request->offset > 1 && buffer->bytes[request->offset] == '0') {
        memmove(buffer->bytes + request->offset, buffer->bytes + request->offset + 1,
                buffer->byte_count - request->offset - 1);
        buffer->byte_count--;
    }
    return SXML_STATUS_OK;
}

/* Subtracts magnitude second from first (first >= second) into output. */
typedef struct sxml_exi_subtraction {
    sxml_byte_view first;
    sxml_byte_view second;
} sxml_exi_subtraction;

static sxml_status sxml_exi_subtract_magnitudes(sxml_exi_subtraction subtraction, sxml_exi_buffer *output) {
    const size_t start = output->byte_count;
    sxml_status status = sxml_exi_buffer_append(output, subtraction.first);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uint32_t borrow = 0;
    for (size_t position = 0; position < subtraction.first.byte_count; position++) {
        const size_t digit_index = start + subtraction.first.byte_count - 1 - position;
        int32_t digit = (int32_t)(output->bytes[digit_index] - '0') - (int32_t)borrow;
        if (position < subtraction.second.byte_count) {
            digit -= (int32_t)(subtraction.second.bytes[subtraction.second.byte_count - 1 - position] - '0');
        }
        borrow = digit < 0 ? 1U : 0U;
        if (digit < 0) {
            digit += 10;
        }
        output->bytes[digit_index] = (uint8_t)('0' + digit);
    }
    size_t leading = 0;
    const size_t digit_count = output->byte_count - start;
    while (leading + 1 < digit_count && output->bytes[start + leading] == '0') {
        leading++;
    }
    if (leading > 0) {
        memmove(output->bytes + start, output->bytes + start + leading, digit_count - leading);
        output->byte_count -= leading;
    }
    return SXML_STATUS_OK;
}

/* Adds magnitudes into output. */
static sxml_status sxml_exi_add_magnitudes(sxml_exi_subtraction addition, sxml_exi_buffer *output) {
    const size_t longest =
        addition.first.byte_count > addition.second.byte_count ? addition.first.byte_count : addition.second.byte_count;
    const size_t start = output->byte_count;
    sxml_status status = SXML_STATUS_OK;
    for (size_t position = 0; position <= longest && status == SXML_STATUS_OK; position++) {
        status = sxml_exi_buffer_append(output, SXML_EXI_DATATYPE_TEXT("0"));
    }
    if (status != SXML_STATUS_OK) {
        return status;
    }
    uint32_t carry = 0;
    for (size_t position = 0; position <= longest; position++) {
        uint32_t digit = carry;
        if (position < addition.first.byte_count) {
            digit += (uint32_t)(addition.first.bytes[addition.first.byte_count - 1 - position] - '0');
        }
        if (position < addition.second.byte_count) {
            digit += (uint32_t)(addition.second.bytes[addition.second.byte_count - 1 - position] - '0');
        }
        carry = digit / 10U;
        output->bytes[start + longest - position] = (uint8_t)('0' + digit % 10U);
    }
    size_t leading = 0;
    const size_t digit_count = output->byte_count - start;
    while (leading + 1 < digit_count && output->bytes[start + leading] == '0') {
        leading++;
    }
    if (leading > 0) {
        memmove(output->bytes + start, output->bytes + start + leading, digit_count - leading);
        output->byte_count -= leading;
    }
    return SXML_STATUS_OK;
}

/* The signed difference first - second, appended to work. */
typedef struct sxml_exi_signed_pair {
    sxml_exi_signed first;
    sxml_exi_signed second;
    sxml_exi_buffer *work;
} sxml_exi_signed_pair;

static sxml_status sxml_exi_signed_difference(const sxml_exi_signed_pair *pair, sxml_exi_signed *difference) {
    sxml_exi_buffer *work = pair->work;
    /* Copies first, as the buffer may move while appending. */
    const sxml_exi_signed first = pair->first;
    const sxml_exi_signed second = pair->second;
    const size_t offset = work->byte_count;
    sxml_status status = SXML_STATUS_OK;
    bool is_negative = false;
    if (first.is_negative != second.is_negative) {
        /* a - (-b) = a + b; -a - b = -(a + b). */
        sxml_exi_buffer copy = {.allocator = work->allocator};
        status = sxml_exi_buffer_append(&copy, sxml_exi_magnitude(work, first));
        const size_t second_start = copy.byte_count;
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_buffer_append(&copy, sxml_exi_magnitude(work, second));
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_add_magnitudes(
                (sxml_exi_subtraction){.first = {.bytes = copy.bytes, .byte_count = second_start},
                                       .second = {.bytes = copy.bytes + second_start, .byte_count = copy.byte_count - second_start}},
                work);
        }
        sxml_exi_buffer_release(&copy);
        is_negative = first.is_negative;
    } else {
        const int order = sxml_exi_compare_magnitudes(sxml_exi_magnitude(work, first), sxml_exi_magnitude(work, second));
        sxml_exi_buffer copy = {.allocator = work->allocator};
        const sxml_exi_signed larger = order >= 0 ? first : second;
        const sxml_exi_signed smaller = order >= 0 ? second : first;
        status = sxml_exi_buffer_append(&copy, sxml_exi_magnitude(work, larger));
        const size_t smaller_start = copy.byte_count;
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_buffer_append(&copy, sxml_exi_magnitude(work, smaller));
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_subtract_magnitudes(
                (sxml_exi_subtraction){.first = {.bytes = copy.bytes, .byte_count = smaller_start},
                                       .second = {.bytes = copy.bytes + smaller_start, .byte_count = copy.byte_count - smaller_start}},
                work);
        }
        sxml_exi_buffer_release(&copy);
        /* (a - b) is negative when |a| < |b| for positives, when |a| > |b|
           for negatives. */
        is_negative = first.is_negative ? order > 0 : order < 0;
    }
    *difference = (sxml_exi_signed){.is_negative = is_negative, .offset = offset, .digit_count = work->byte_count - offset};
    if (sxml_exi_is_zero(sxml_exi_magnitude(work, *difference))) {
        difference->is_negative = false;
    }
    return status;
}

/* A small magnitude's value; UINT64_MAX past 18 digits. */
static uint64_t sxml_exi_small_value(sxml_byte_view digits) {
    if (digits.byte_count > 18) {
        return UINT64_MAX;
    }
    uint64_t value = 0;
    for (size_t digit_index = 0; digit_index < digits.byte_count; digit_index++) {
        value = value * 10U + (uint64_t)(digits.bytes[digit_index] - '0');
    }
    return value;
}

/* Appends a 64-bit signed value's decimal text. */
static sxml_status sxml_exi_append_int64(sxml_exi_buffer *output, int64_t value) {
    uint8_t digits[24];
    size_t digit_count = 0;
    uint64_t magnitude = value < 0 ? (uint64_t)(-(value + 1)) + 1U : (uint64_t)value;
    do {
        digits[sizeof digits - 1 - digit_count++] = (uint8_t)('0' + magnitude % 10U);
        magnitude /= 10U;
    } while (magnitude > 0);
    if (value < 0) {
        digits[sizeof digits - 1 - digit_count++] = '-';
    }
    return sxml_exi_buffer_append(output, (sxml_byte_view){.bytes = digits + sizeof digits - digit_count, .byte_count = digit_count});
}

/* --------------------------------------------------------- Integer range */

sxml_status sxml_exi_integer_range_of(const sxml_exi_integer_bounds *bounds, sxml_exi_integer_range *range) {
    /* The oracle's defaults when a bound is absent: -(10^40 - 1) and
       10^40 - 1. */
    static const char forty_nines[] = "9999999999999999999999999999999999999999";
    sxml_exi_buffer work = {.allocator = range->lower_bound->allocator};
    sxml_exi_signed minimum = {.is_negative = true};
    sxml_exi_signed maximum = {.is_negative = false};
    sxml_status status = sxml_exi_buffer_append(&work, (sxml_byte_view){.bytes = (const uint8_t *)forty_nines, .byte_count = 40});
    minimum.offset = 0;
    minimum.digit_count = 40;
    maximum.offset = 0;
    maximum.digit_count = 40;
    const sxml_byte_view inclusive_minimum = bounds->minimum_inclusive;
    const sxml_byte_view exclusive_minimum = bounds->minimum_exclusive;
    const sxml_byte_view inclusive_maximum = bounds->maximum_inclusive;
    const sxml_byte_view exclusive_maximum = bounds->maximum_exclusive;
    const sxml_byte_view texts[4] = {inclusive_minimum, exclusive_minimum, inclusive_maximum, exclusive_maximum};
    for (size_t bound_index = 0; bound_index < 4 && status == SXML_STATUS_OK; bound_index++) {
        if (texts[bound_index].bytes == NULL) {
            continue;
        }
        sxml_exi_signed_result parsed_bound = {0};
        status = sxml_exi_parse_signed(&(sxml_exi_integer_parse){.text = texts[bound_index], .work = &work}, &parsed_bound);
        if (status != SXML_STATUS_OK || !parsed_bound.is_valid) {
            continue;
        }
        sxml_exi_signed bound = parsed_bound.number;
        if (bound_index == 1 || bound_index == 3) {
            /* minExclusive + 1, maxExclusive - 1. */
            const bool moves_up = bound_index == 1;
            const bool grows = moves_up == !bound.is_negative;
            if (grows || !sxml_exi_is_zero(sxml_exi_magnitude(&work, bound))) {
                status = sxml_exi_step_magnitude(&(sxml_exi_step_request){.buffer = &work, .offset = bound.offset, .is_increment = grows});
                bound.digit_count = work.byte_count - bound.offset;
            } else {
                /* 0 - 1 = -1 and 0 + 1 = 1 when the magnitude is zero. */
                work.bytes[bound.offset] = '1';
                bound.is_negative = !moves_up;
            }
            if (sxml_exi_is_zero(sxml_exi_magnitude(&work, bound))) {
                bound.is_negative = false;
            }
        }
        sxml_exi_signed difference = {0};
        const bool is_minimum = bound_index < 2;
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_signed_difference(
                &(sxml_exi_signed_pair){.first = bound, .second = is_minimum ? minimum : maximum, .work = &work}, &difference);
        }
        if (status == SXML_STATUS_OK) {
            /* The minimum is the larger, the maximum the smaller. */
            const bool is_positive = !difference.is_negative && !sxml_exi_is_zero(sxml_exi_magnitude(&work, difference));
            if (is_minimum && is_positive) {
                minimum = bound;
            } else if (!is_minimum && difference.is_negative) {
                maximum = bound;
            }
        }
    }
    sxml_exi_signed span = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_signed_difference(&(sxml_exi_signed_pair){.first = maximum, .second = minimum, .work = &work},
                                            &span);
    }
    if (status == SXML_STATUS_OK) {
        const uint64_t value = span.is_negative ? UINT64_MAX : sxml_exi_small_value(sxml_exi_magnitude(&work, span));
        if (value < 4096U) {
            range->datatype_kind = SXML_EXI_DATATYPE_BOUNDED_INTEGER;
            range->value_count = (uint32_t)value + 1U;
            range->lower_bound->byte_count = 0;
            if (minimum.is_negative) {
                status = sxml_exi_buffer_append(range->lower_bound, SXML_EXI_DATATYPE_TEXT("-"));
            }
            if (status == SXML_STATUS_OK) {
                status = sxml_exi_buffer_append(range->lower_bound, sxml_exi_magnitude(&work, minimum));
            }
        } else {
            range->datatype_kind = minimum.is_negative ? SXML_EXI_DATATYPE_INTEGER : SXML_EXI_DATATYPE_UNSIGNED_INTEGER;
        }
    }
    sxml_exi_buffer_release(&work);
    return status;
}

/* ------------------------------------------------------- Parsed values */

/* A value in its representation's terms (7.1). Magnitudes and bytes are
   in the work buffer of the call. */
typedef struct sxml_exi_parsed_value {
    sxml_exi_datatype_kind datatype_kind;
    sxml_exi_date_time_kind date_time_kind;
    /* Boolean; the pattern Boolean's code 0 to 3 (false, 0, true, 1). */
    bool boolean_value;
    uint32_t boolean_code;
    /* Integers: the value; Decimal: the sign and integral part, and the
       fraction's digits reversed as a magnitude (7.1.3). */
    sxml_exi_signed integer;
    sxml_exi_signed fraction;
    /* Float: mantissa and exponent (7.1.4). */
    int64_t mantissa;
    int64_t exponent;
    /* Date-Time (7.1.8, Table 7-3). */
    int64_t year;
    uint32_t month_day;
    uint32_t time;
    uint32_t fraction_seconds;
    bool has_time_zone;
    int32_t time_zone;
    /* Binary: bytes in the work buffer. */
    size_t byte_offset;
    size_t byte_count;
    /* Bounded integer: the offset; enumeration: the ordinal. */
    uint32_t ordinal;
} sxml_exi_parsed_value;

static bool sxml_exi_view_is(sxml_byte_view text, sxml_byte_view expected) {
    return text.byte_count == expected.byte_count &&
           (text.byte_count == 0 || memcmp(text.bytes, expected.bytes, text.byte_count) == 0);
}

/* What a value is parsed, written or read with. */
typedef struct sxml_exi_value_context {
    const sxml_exi_schema_grammars *grammars;
    sxml_exi_buffer *work;
} sxml_exi_value_context;

/* Java's Integer.parseInt of a short text: an optional sign (not alone)
   then ASCII digits; false when it throws. */
static bool sxml_exi_java_parse_int(sxml_byte_view text, int64_t *value) {
    if (text.byte_count == 0) {
        return false;
    }
    size_t position = 0;
    bool is_negative = false;
    if (text.bytes[0] == '-' || text.bytes[0] == '+') {
        if (text.byte_count == 1) {
            return false;
        }
        is_negative = text.bytes[0] == '-';
        position = 1;
    }
    int64_t result = 0;
    for (; position < text.byte_count; position++) {
        if (!sxml_exi_is_digit(text.bytes[position]) || result > INT64_C(214748364700)) {
            return false;
        }
        result = result * 10 + (int64_t)(text.bytes[position] - '0');
    }
    if (result > INT64_C(2147483648) || (!is_negative && result > INT64_C(2147483647))) {
        return false;
    }
    *value = is_negative ? -result : result;
    return true;
}

/* A cursor over a date-time's text (the oracle's StringBuilder). */
typedef struct sxml_exi_cursor {
    sxml_byte_view text;
    size_t position;
} sxml_exi_cursor;

static size_t sxml_exi_cursor_left(const sxml_exi_cursor *cursor) {
    return cursor->text.byte_count - cursor->position;
}

/* Reads a field of width characters with Integer.parseInt. */
typedef struct sxml_exi_field {
    size_t width;
    int64_t *value;
} sxml_exi_field;

static bool sxml_exi_read_field(sxml_exi_cursor *cursor, sxml_exi_field field) {
    if (sxml_exi_cursor_left(cursor) < field.width) {
        return false;
    }
    const sxml_byte_view text = {.bytes = cursor->text.bytes + cursor->position, .byte_count = field.width};
    cursor->position += field.width;
    return sxml_exi_java_parse_int(text, field.value);
}

static bool sxml_exi_read_character(sxml_exi_cursor *cursor, uint8_t character) {
    if (sxml_exi_cursor_left(cursor) == 0 || cursor->text.bytes[cursor->position] != character) {
        return false;
    }
    cursor->position++;
    return true;
}

static bool sxml_exi_read_month_day(sxml_exi_cursor *cursor, uint32_t *month_day) {
    int64_t month = 0;
    int64_t day = 0;
    if (!sxml_exi_read_field(cursor, (sxml_exi_field){.width = 2, .value = &month}) ||
        !sxml_exi_read_character(cursor, '-') || !sxml_exi_read_field(cursor, (sxml_exi_field){.width = 2, .value = &day})) {
        return false;
    }
    *month_day = (uint32_t)(month * 32 + day);
    return true;
}

static bool sxml_exi_read_time(sxml_exi_cursor *cursor, sxml_exi_parsed_value *parsed) {
    int64_t hour = 0;
    int64_t minute = 0;
    int64_t second = 0;
    if (!sxml_exi_read_field(cursor, (sxml_exi_field){.width = 2, .value = &hour}) ||
        !sxml_exi_read_character(cursor, ':') ||
        !sxml_exi_read_field(cursor, (sxml_exi_field){.width = 2, .value = &minute}) ||
        !sxml_exi_read_character(cursor, ':') ||
        !sxml_exi_read_field(cursor, (sxml_exi_field){.width = 2, .value = &second})) {
        return false;
    }
    parsed->time = (uint32_t)((hour * 64 + minute) * 64 + second);
    if (sxml_exi_cursor_left(cursor) > 0 && cursor->text.bytes[cursor->position] == '.') {
        cursor->position++;
        size_t digit_count = 0;
        while (digit_count < sxml_exi_cursor_left(cursor) && sxml_exi_is_digit(cursor->text.bytes[cursor->position + digit_count])) {
            digit_count++;
        }
        /* Integer.parseInt of the digits reversed. */
        if (digit_count == 0) {
            return false;
        }
        uint64_t reversed = 0;
        for (size_t digit_index = digit_count; digit_index > 0; digit_index--) {
            reversed = reversed * 10U + (uint64_t)(cursor->text.bytes[cursor->position + digit_index - 1] - '0');
            if (reversed > SXML_EXI_FRACTION_LIMIT) {
                return false;
            }
        }
        parsed->fraction_seconds = (uint32_t)reversed;
        cursor->position += digit_count;
    }
    return true;
}

/* DateTimeValue.parse (7.1.8): the fields of the kind, then nothing, Z,
   or a signed hh?mm offset. */
static bool sxml_exi_parse_date_time(sxml_byte_view text, sxml_exi_parsed_value *parsed) {
    sxml_exi_cursor cursor = {.text = sxml_exi_java_trim(text)};
    int64_t year = 0;
    int64_t month = 0;
    int64_t day = 0;
    bool is_valid = cursor.text.byte_count > 0;
    const sxml_exi_date_time_kind kind = parsed->date_time_kind;
    const bool has_year = kind == SXML_EXI_DATE_TIME_G_YEAR || kind == SXML_EXI_DATE_TIME_G_YEAR_MONTH ||
                          kind == SXML_EXI_DATE_TIME_DATE || kind == SXML_EXI_DATE_TIME_DATE_TIME;
    if (is_valid && has_year) {
        const size_t width = cursor.text.bytes[0] == '-' ? 5 : 4;
        is_valid = sxml_exi_read_field(&cursor, (sxml_exi_field){.width = width, .value = &year});
    }
    switch (kind) {
    case SXML_EXI_DATE_TIME_G_YEAR:
        break;
    case SXML_EXI_DATE_TIME_G_YEAR_MONTH:
        is_valid = is_valid && sxml_exi_read_character(&cursor, '-') &&
                   sxml_exi_read_field(&cursor, (sxml_exi_field){.width = 2, .value = &month});
        parsed->month_day = (uint32_t)(month * 32);
        break;
    case SXML_EXI_DATE_TIME_DATE:
        is_valid = is_valid && sxml_exi_read_character(&cursor, '-') && sxml_exi_read_month_day(&cursor, &parsed->month_day);
        break;
    case SXML_EXI_DATE_TIME_DATE_TIME:
        is_valid = is_valid && sxml_exi_read_character(&cursor, '-') && sxml_exi_read_month_day(&cursor, &parsed->month_day) &&
                   sxml_exi_read_character(&cursor, 'T') && sxml_exi_read_time(&cursor, parsed);
        break;
    case SXML_EXI_DATE_TIME_TIME:
        is_valid = is_valid && sxml_exi_read_time(&cursor, parsed);
        break;
    case SXML_EXI_DATE_TIME_G_MONTH:
        is_valid = is_valid && sxml_exi_read_character(&cursor, '-') && sxml_exi_read_character(&cursor, '-') &&
                   sxml_exi_read_field(&cursor, (sxml_exi_field){.width = 2, .value = &month});
        parsed->month_day = (uint32_t)(month * 32);
        if (is_valid && sxml_exi_cursor_left(&cursor) > 1 && cursor.text.bytes[cursor.position] == '-' &&
            cursor.text.bytes[cursor.position + 1] == '-') {
            cursor.position += 2;
        }
        break;
    case SXML_EXI_DATE_TIME_G_MONTH_DAY:
        is_valid = is_valid && sxml_exi_read_character(&cursor, '-') && sxml_exi_read_character(&cursor, '-') &&
                   sxml_exi_read_month_day(&cursor, &parsed->month_day);
        break;
    case SXML_EXI_DATE_TIME_G_DAY:
        is_valid = is_valid && sxml_exi_read_character(&cursor, '-') && sxml_exi_read_character(&cursor, '-') &&
                   sxml_exi_read_character(&cursor, '-') &&
                   sxml_exi_read_field(&cursor, (sxml_exi_field){.width = 2, .value = &day});
        parsed->month_day = (uint32_t)day;
        break;
    }
    if (!is_valid) {
        return false;
    }
    parsed->year = year;
    const size_t left = sxml_exi_cursor_left(&cursor);
    if (left == 0) {
        parsed->has_time_zone = false;
    } else if (left == 1 && cursor.text.bytes[cursor.position] == 'Z') {
        parsed->has_time_zone = true;
        parsed->time_zone = 0;
    } else {
        const uint8_t sign = cursor.text.bytes[cursor.position];
        int64_t hours = 0;
        int64_t minutes = 0;
        if ((sign != '+' && sign != '-') || left < 6 ||
            !sxml_exi_java_parse_int((sxml_byte_view){.bytes = cursor.text.bytes + cursor.position + 1, .byte_count = 2}, &hours) ||
            !sxml_exi_java_parse_int((sxml_byte_view){.bytes = cursor.text.bytes + cursor.position + 4, .byte_count = 2}, &minutes)) {
            return false;
        }
        parsed->has_time_zone = true;
        parsed->time_zone = (int32_t)((sign == '-' ? -1 : 1) * (hours * 64 + minutes));
    }
    return true;
}

/* DateTimeValue's constructor: 24:mm:ss is the next day's 00:mm:ss. */
static void sxml_exi_normalize_date_time(sxml_exi_parsed_value *parsed) {
    const uint32_t hour = parsed->time / 4096U;
    if (hour != 24) {
        return;
    }
    const uint32_t rest = parsed->time - hour * 4096U;
    parsed->month_day++;
    parsed->time = rest;
    if (parsed->month_day / 32U == 13U) {
        parsed->year++;
        parsed->month_day = 32U + 1U;
    }
}

/* FloatValue.parse (7.1.4), in Java's long arithmetic, then its
   constructor's normalization. */
static bool sxml_exi_parse_float(sxml_byte_view text, sxml_exi_parsed_value *parsed) {
    const sxml_byte_view value = sxml_exi_java_trim(text);
    if (value.byte_count == 0) {
        return false;
    }
    int64_t mantissa = 0;
    int64_t exponent = 0;
    if (sxml_exi_view_is(value, SXML_EXI_DATATYPE_TEXT("INF"))) {
        mantissa = 1;
        exponent = SXML_EXI_FLOAT_SPECIAL_EXPONENT;
    } else if (sxml_exi_view_is(value, SXML_EXI_DATATYPE_TEXT("-INF"))) {
        mantissa = -1;
        exponent = SXML_EXI_FLOAT_SPECIAL_EXPONENT;
    } else if (sxml_exi_view_is(value, SXML_EXI_DATATYPE_TEXT("NaN"))) {
        mantissa = 0;
        exponent = SXML_EXI_FLOAT_SPECIAL_EXPONENT;
    } else {
        size_t exponent_index = value.byte_count;
        for (size_t byte_index = 0; byte_index < value.byte_count; byte_index++) {
            if (value.bytes[byte_index] == 'E') {
                exponent_index = byte_index;
                break;
            }
        }
        if (exponent_index == value.byte_count) {
            for (size_t byte_index = 0; byte_index < value.byte_count; byte_index++) {
                if (value.bytes[byte_index] == 'e') {
                    exponent_index = byte_index;
                    break;
                }
            }
        }
        const bool is_negative = value.bytes[0] == '-';
        const size_t mantissa_end = exponent_index;
        const size_t mantissa_start = (is_negative || value.bytes[0] == '+') ? 1 : 0;
        if (mantissa_end == 0) {
            return false;
        }
        uint64_t digits = 0;
        bool has_point = false;
        int64_t decimal_digits = 0;
        for (size_t byte_index = mantissa_start; byte_index < mantissa_end; byte_index++) {
            const uint8_t byte = value.bytes[byte_index];
            if (sxml_exi_is_digit(byte)) {
                digits = digits * 10U + (uint64_t)(byte - '0');
                decimal_digits += has_point ? 1 : 0;
            } else if (byte == '.' && !has_point) {
                has_point = true;
            } else {
                return false;
            }
        }
        /* Java's wrapping long: a negative result is refused but for
           Long.MIN_VALUE with a minus sign. */
        if (digits > (uint64_t)INT64_MAX && (!is_negative || digits != (uint64_t)INT64_MAX + 1U)) {
            return false;
        }
        mantissa = digits > (uint64_t)INT64_MAX ? INT64_MIN : (is_negative ? -(int64_t)digits : (int64_t)digits);
        uint64_t exponent_digits = 0;
        bool is_negative_exponent = false;
        for (size_t byte_index = exponent_index + 1; byte_index < value.byte_count; byte_index++) {
            const uint8_t byte = value.bytes[byte_index];
            if (sxml_exi_is_digit(byte)) {
                exponent_digits = exponent_digits * 10U + (uint64_t)(byte - '0');
                if (exponent_digits > UINT64_C(1) << 40) {
                    return false;
                }
            } else if (byte == '-' && !is_negative_exponent) {
                is_negative_exponent = true;
            } else if (byte != '+') {
                return false;
            }
        }
        exponent = (is_negative_exponent ? -(int64_t)exponent_digits : (int64_t)exponent_digits) - decimal_digits;
        if (exponent < -SXML_EXI_FLOAT_EXPONENT_LIMIT || exponent > SXML_EXI_FLOAT_EXPONENT_LIMIT) {
            return false;
        }
    }
    parsed->mantissa = mantissa;
    parsed->exponent = exponent;
    return true;
}

/* FloatValue's constructor: a zero mantissa has exponent 0, others lose
   trailing zeros; a special exponent with a mantissa other than 1 and -1
   is NaN. */
static void sxml_exi_normalize_float(sxml_exi_parsed_value *parsed) {
    if (parsed->mantissa == 0) {
        if (parsed->exponent != SXML_EXI_FLOAT_SPECIAL_EXPONENT) {
            parsed->exponent = 0;
        }
    } else {
        while (parsed->mantissa % 10 == 0) {
            parsed->mantissa /= 10;
            parsed->exponent++;
        }
    }
    if (parsed->exponent == SXML_EXI_FLOAT_SPECIAL_EXPONENT && parsed->mantissa != 1 && parsed->mantissa != -1) {
        parsed->mantissa = 0;
    }
}

/* DecimalValue.parse (7.1.3): trimmed, a sign, integral digits, '.', the
   fraction's digits (each part an IntegerValue). */
static sxml_status sxml_exi_parse_decimal(const sxml_exi_value_context *context, sxml_byte_view text,
                                          sxml_exi_parsed_value *parsed) {
    sxml_byte_view value = sxml_exi_java_trim(text);
    parsed->ordinal = 0;
    if (value.byte_count == 0) {
        return SXML_STATUS_OK;
    }
    bool is_negative = false;
    if (value.bytes[0] == '-' || value.bytes[0] == '+') {
        is_negative = value.bytes[0] == '-';
        value.bytes++;
        value.byte_count--;
    }
    size_t point = value.byte_count;
    for (size_t byte_index = 0; byte_index < value.byte_count; byte_index++) {
        if (value.bytes[byte_index] == '.') {
            point = byte_index;
            break;
        }
    }
    const sxml_byte_view integral = {.bytes = value.bytes, .byte_count = point};
    const sxml_byte_view fraction = point < value.byte_count
                                        ? (sxml_byte_view){.bytes = value.bytes + point + 1, .byte_count = value.byte_count - point - 1}
                                        : (sxml_byte_view){.bytes = NULL, .byte_count = 0};
    /* Each part: digits only (IntegerValue.parse would take a sign in
       them too, which no decimal has). */
    if ((point > 0 && integral.byte_count == 0) || (point < value.byte_count && fraction.byte_count == 0) ||
        (point == value.byte_count && integral.byte_count == 0)) {
        return SXML_STATUS_OK;
    }
    for (size_t byte_index = 0; byte_index < integral.byte_count; byte_index++) {
        if (!sxml_exi_is_digit(integral.bytes[byte_index])) {
            return SXML_STATUS_OK;
        }
    }
    for (size_t byte_index = 0; byte_index < fraction.byte_count; byte_index++) {
        if (!sxml_exi_is_digit(fraction.bytes[byte_index])) {
            return SXML_STATUS_OK;
        }
    }
    sxml_exi_buffer *work = context->work;
    parsed->integer = (sxml_exi_signed){.is_negative = is_negative, .offset = work->byte_count};
    sxml_status status = sxml_exi_append_magnitude(work, integral);
    parsed->integer.digit_count = work->byte_count - parsed->integer.offset;
    /* The fraction's digits reversed, as an integer: trailing zeros go. */
    size_t kept = fraction.byte_count;
    while (kept > 0 && fraction.bytes[kept - 1] == '0') {
        kept--;
    }
    parsed->fraction = (sxml_exi_signed){.offset = work->byte_count};
    for (size_t digit_index = kept; digit_index > 0 && status == SXML_STATUS_OK; digit_index--) {
        status = sxml_exi_buffer_append(work, (sxml_byte_view){.bytes = fraction.bytes + digit_index - 1, .byte_count = 1});
    }
    if (status == SXML_STATUS_OK && kept == 0) {
        status = sxml_exi_buffer_append(work, SXML_EXI_DATATYPE_TEXT("0"));
    }
    parsed->fraction.digit_count = work->byte_count - parsed->fraction.offset;
    if (sxml_exi_is_zero(sxml_exi_magnitude(work, parsed->integer)) &&
        sxml_exi_is_zero(sxml_exi_magnitude(work, parsed->fraction))) {
        parsed->integer.is_negative = false;
    }
    parsed->ordinal = 1;
    return status;
}

/* Xerces's Base64.decode: whitespace removed, quadruples of the alphabet,
   padding only at the end with its unused bits zero. */
static int32_t sxml_exi_base64_digit(uint8_t byte) {
    if (byte >= 'A' && byte <= 'Z') {
        return byte - 'A';
    }
    if (byte >= 'a' && byte <= 'z') {
        return byte - 'a' + 26;
    }
    if (byte >= '0' && byte <= '9') {
        return byte - '0' + 52;
    }
    if (byte == '+') {
        return 62;
    }
    if (byte == '/') {
        return 63;
    }
    return -1;
}

static sxml_status sxml_exi_parse_base64(const sxml_exi_value_context *context, sxml_byte_view text,
                                         sxml_exi_parsed_value *parsed) {
    sxml_exi_buffer *work = context->work;
    const size_t characters_start = work->byte_count;
    sxml_status status = SXML_STATUS_OK;
    const sxml_byte_view value = sxml_exi_java_trim(text);
    for (size_t byte_index = 0; byte_index < value.byte_count && status == SXML_STATUS_OK; byte_index++) {
        const uint8_t byte = value.bytes[byte_index];
        if (byte != 0x20 && byte != 0xD && byte != 0xA && byte != 0x9) {
            status = sxml_exi_buffer_append(work, (sxml_byte_view){.bytes = &byte, .byte_count = 1});
        }
    }
    parsed->ordinal = 0;
    const size_t character_count = work->byte_count - characters_start;
    if (status != SXML_STATUS_OK || character_count % 4 != 0) {
        return status;
    }
    parsed->byte_offset = work->byte_count;
    for (size_t quad_index = 0; quad_index < character_count / 4 && status == SXML_STATUS_OK; quad_index++) {
        const uint8_t *characters = work->bytes + characters_start + quad_index * 4;
        const int32_t first = sxml_exi_base64_digit(characters[0]);
        const int32_t second = sxml_exi_base64_digit(characters[1]);
        const int32_t third = sxml_exi_base64_digit(characters[2]);
        const int32_t fourth = sxml_exi_base64_digit(characters[3]);
        const bool is_last = quad_index + 1 == character_count / 4;
        if (first < 0 || second < 0) {
            return SXML_STATUS_OK;
        }
        uint8_t bytes[3] = {(uint8_t)((uint32_t)first << 2U | (uint32_t)second >> 4U), 0, 0};
        size_t byte_count = 1;
        if (third >= 0 && fourth >= 0) {
            bytes[1] = (uint8_t)(((uint32_t)second & 0xFU) << 4U | (uint32_t)third >> 2U);
            bytes[2] = (uint8_t)(((uint32_t)third & 0x3U) << 6U | (uint32_t)fourth);
            byte_count = 3;
        } else if (is_last && characters[2] == '=' && characters[3] == '=') {
            if (((uint32_t)second & 0xFU) != 0) {
                return SXML_STATUS_OK;
            }
        } else if (is_last && third >= 0 && characters[3] == '=') {
            if (((uint32_t)third & 0x3U) != 0) {
                return SXML_STATUS_OK;
            }
            bytes[1] = (uint8_t)(((uint32_t)second & 0xFU) << 4U | (uint32_t)third >> 2U);
            byte_count = 2;
        } else {
            return SXML_STATUS_OK;
        }
        status = sxml_exi_buffer_append(work, (sxml_byte_view){.bytes = bytes, .byte_count = byte_count});
    }
    parsed->byte_count = work->byte_count - parsed->byte_offset;
    parsed->ordinal = 1;
    return status;
}

static int32_t sxml_exi_hex_digit(uint8_t byte) {
    if (byte >= '0' && byte <= '9') {
        return byte - '0';
    }
    if (byte >= 'a' && byte <= 'f') {
        return byte - 'a' + 10;
    }
    if (byte >= 'A' && byte <= 'F') {
        return byte - 'A' + 10;
    }
    return -1;
}

/* Xerces's HexBin.decode: pairs of hexadecimal digits. */
static sxml_status sxml_exi_parse_hex(const sxml_exi_value_context *context, sxml_byte_view text,
                                      sxml_exi_parsed_value *parsed) {
    const sxml_byte_view value = sxml_exi_java_trim(text);
    parsed->ordinal = 0;
    if (value.byte_count % 2 != 0) {
        return SXML_STATUS_OK;
    }
    sxml_exi_buffer *work = context->work;
    parsed->byte_offset = work->byte_count;
    sxml_status status = SXML_STATUS_OK;
    for (size_t byte_index = 0; byte_index < value.byte_count && status == SXML_STATUS_OK; byte_index += 2) {
        const int32_t high = sxml_exi_hex_digit(value.bytes[byte_index]);
        const int32_t low = sxml_exi_hex_digit(value.bytes[byte_index + 1]);
        if (high < 0 || low < 0) {
            return SXML_STATUS_OK;
        }
        const uint8_t byte = (uint8_t)((uint32_t)high << 4U | (uint32_t)low);
        status = sxml_exi_buffer_append(work, (sxml_byte_view){.bytes = &byte, .byte_count = 1});
    }
    parsed->byte_count = work->byte_count - parsed->byte_offset;
    parsed->ordinal = 1;
    return status;
}

/* Parses text as a value of datatype (a kind other than the strings and
   lists); parsed->ordinal of a bounded integer is its offset, of an
   enumeration its index. *is_valid false when it is no value. */
typedef struct sxml_exi_parse_result {
    sxml_exi_parsed_value *parsed;
    bool is_valid;
} sxml_exi_parse_result;

static sxml_status sxml_exi_parse_scalar(const sxml_exi_value_context *context, sxml_exi_value_text value,
                                         sxml_exi_parse_result *result);

static sxml_status sxml_exi_parse_integer_kind(const sxml_exi_value_context *context, sxml_exi_value_text value,
                                               sxml_exi_parse_result *result) {
    const sxml_exi_datatype *datatype = &context->grammars->datatypes[value.datatype];
    sxml_exi_parsed_value *parsed = result->parsed;
    sxml_exi_signed_result integer = {0};
    sxml_status status =
        sxml_exi_parse_integer_value(&(sxml_exi_integer_parse){.text = value.text, .work = context->work}, &integer);
    parsed->integer = integer.number;
    result->is_valid = integer.is_valid;
    if (status != SXML_STATUS_OK || !result->is_valid) {
        return status;
    }
    if (datatype->datatype_kind == SXML_EXI_DATATYPE_UNSIGNED_INTEGER) {
        result->is_valid = !parsed->integer.is_negative;
        return SXML_STATUS_OK;
    }
    if (datatype->datatype_kind != SXML_EXI_DATATYPE_BOUNDED_INTEGER) {
        return SXML_STATUS_OK;
    }
    /* The offset from the lower bound, within the range (7.1.5). */
    sxml_exi_signed_result lower = {0};
    const sxml_exi_text bound = datatype->lower_bound;
    sxml_exi_buffer *work = context->work;
    const sxml_byte_view lower_text = {.bytes = context->grammars->text.bytes + bound.offset, .byte_count = bound.byte_count};
    status = sxml_exi_parse_signed(&(sxml_exi_integer_parse){.text = lower_text, .work = work}, &lower);
    sxml_exi_signed offset = {0};
    if (status == SXML_STATUS_OK && lower.is_valid) {
        status = sxml_exi_signed_difference(
            &(sxml_exi_signed_pair){.first = parsed->integer, .second = lower.number, .work = work}, &offset);
    }
    if (status != SXML_STATUS_OK || !lower.is_valid) {
        result->is_valid = false;
        return status;
    }
    const uint64_t distance = offset.is_negative ? UINT64_MAX : sxml_exi_small_value(sxml_exi_magnitude(work, offset));
    result->is_valid = distance < datatype->value_count;
    parsed->ordinal = (uint32_t)(result->is_valid ? distance : 0);
    return SXML_STATUS_OK;
}

/* The text the decoder writes for a parsed value (the oracle's
   toString). */
typedef struct sxml_exi_print {
    const sxml_exi_value_context *context;
    const sxml_exi_parsed_value *parsed;
} sxml_exi_print;

static sxml_status sxml_exi_print_value(const sxml_exi_print *print, sxml_exi_buffer *output);

/* Compares the canonical text of value in the enumeration's base with its
   values; ordinal to the first equal one. */
static sxml_status sxml_exi_parse_enumeration(const sxml_exi_value_context *context, sxml_exi_value_text value,
                                              sxml_exi_parse_result *result) {
    const sxml_exi_schema_grammars *grammars = context->grammars;
    const sxml_exi_datatype *datatype = &grammars->datatypes[value.datatype];
    const sxml_exi_datatype *base = &grammars->datatypes[datatype->base_datatype];
    sxml_byte_view canonical = value.text;
    sxml_exi_buffer printed = {.allocator = context->work->allocator};
    sxml_status status = SXML_STATUS_OK;
    result->is_valid = false;
    const bool is_string = base->datatype_kind == SXML_EXI_DATATYPE_STRING ||
                           base->datatype_kind == SXML_EXI_DATATYPE_RESTRICTED_STRING ||
                           base->datatype_kind == SXML_EXI_DATATYPE_LIST ||
                           base->datatype_kind == SXML_EXI_DATATYPE_ENUMERATION;
    if (!is_string) {
        sxml_exi_parsed_value base_value = {.datatype_kind = base->datatype_kind, .date_time_kind = base->date_time_kind};
        sxml_exi_parse_result base_result = {.parsed = &base_value};
        status = sxml_exi_parse_scalar(context, (sxml_exi_value_text){.datatype = datatype->base_datatype, .text = value.text},
                                       &base_result);
        if (status == SXML_STATUS_OK && base_result.is_valid) {
            status = sxml_exi_print_value(&(sxml_exi_print){.context = context, .parsed = &base_value}, &printed);
            canonical = sxml_exi_buffer_view(&printed);
        } else {
            sxml_exi_buffer_release(&printed);
            return status;
        }
    }
    for (uint32_t value_index = 0; value_index < datatype->value_count && status == SXML_STATUS_OK; value_index++) {
        const sxml_exi_text stored = grammars->enumeration_values[datatype->first_value + value_index];
        const sxml_byte_view text = {.bytes = grammars->text.bytes + stored.offset, .byte_count = stored.byte_count};
        if (text.byte_count == canonical.byte_count &&
            (text.byte_count == 0 || memcmp(text.bytes, canonical.bytes, text.byte_count) == 0)) {
            result->is_valid = true;
            result->parsed->ordinal = value_index;
            break;
        }
    }
    sxml_exi_buffer_release(&printed);
    return status;
}

static sxml_status sxml_exi_parse_scalar(const sxml_exi_value_context *context, sxml_exi_value_text value,
                                         sxml_exi_parse_result *result) {
    const sxml_exi_datatype *datatype = &context->grammars->datatypes[value.datatype];
    sxml_exi_parsed_value *parsed = result->parsed;
    *parsed = (sxml_exi_parsed_value){.datatype_kind = datatype->datatype_kind, .date_time_kind = datatype->date_time_kind};
    result->is_valid = false;
    sxml_status status = SXML_STATUS_OK;
    switch (datatype->datatype_kind) {
    case SXML_EXI_DATATYPE_BOOLEAN: {
        const sxml_byte_view text = sxml_exi_java_trim(value.text);
        if (sxml_exi_view_is(text, SXML_EXI_DATATYPE_TEXT("true")) || sxml_exi_view_is(text, SXML_EXI_DATATYPE_TEXT("1"))) {
            parsed->boolean_value = true;
            result->is_valid = true;
        } else if (sxml_exi_view_is(text, SXML_EXI_DATATYPE_TEXT("false")) ||
                   sxml_exi_view_is(text, SXML_EXI_DATATYPE_TEXT("0"))) {
            result->is_valid = true;
        }
        return SXML_STATUS_OK;
    }
    case SXML_EXI_DATATYPE_PATTERN_BOOLEAN: {
        static const sxml_byte_view codes[4] = {SXML_BYTE_VIEW_INITIALIZER("false"), SXML_BYTE_VIEW_INITIALIZER("0"),
                                                SXML_BYTE_VIEW_INITIALIZER("true"), SXML_BYTE_VIEW_INITIALIZER("1")};
        const sxml_byte_view text = sxml_exi_java_trim(value.text);
        for (uint32_t code_index = 0; code_index < 4; code_index++) {
            if (sxml_exi_view_is(text, codes[code_index])) {
                parsed->boolean_code = code_index;
                parsed->boolean_value = code_index >= 2;
                result->is_valid = true;
            }
        }
        return SXML_STATUS_OK;
    }
    case SXML_EXI_DATATYPE_DECIMAL:
        status = sxml_exi_parse_decimal(context, value.text, parsed);
        result->is_valid = parsed->ordinal == 1;
        parsed->ordinal = 0;
        return status;
    case SXML_EXI_DATATYPE_FLOAT:
        result->is_valid = sxml_exi_parse_float(value.text, parsed);
        if (result->is_valid) {
            sxml_exi_normalize_float(parsed);
        }
        return SXML_STATUS_OK;
    case SXML_EXI_DATATYPE_INTEGER:
    case SXML_EXI_DATATYPE_UNSIGNED_INTEGER:
    case SXML_EXI_DATATYPE_BOUNDED_INTEGER:
        return sxml_exi_parse_integer_kind(context, value, result);
    case SXML_EXI_DATATYPE_DATE_TIME:
        result->is_valid = sxml_exi_parse_date_time(value.text, parsed);
        if (result->is_valid) {
            sxml_exi_normalize_date_time(parsed);
        }
        return SXML_STATUS_OK;
    case SXML_EXI_DATATYPE_BASE64_BINARY:
        status = sxml_exi_parse_base64(context, value.text, parsed);
        result->is_valid = parsed->ordinal == 1;
        return status;
    case SXML_EXI_DATATYPE_HEX_BINARY:
        status = sxml_exi_parse_hex(context, value.text, parsed);
        result->is_valid = parsed->ordinal == 1;
        return status;
    case SXML_EXI_DATATYPE_ENUMERATION:
    case SXML_EXI_DATATYPE_STRING:
    case SXML_EXI_DATATYPE_RESTRICTED_STRING:
    case SXML_EXI_DATATYPE_LIST:
        break;
    }
    return SXML_STATUS_OK;
}


/* Parses text as a value of datatype; an enumeration's text in its base
   (which is no enumeration), then among its values. */
static sxml_status sxml_exi_parse_value(const sxml_exi_value_context *context, sxml_exi_value_text value,
                                        sxml_exi_parse_result *result) {
    if (context->grammars->datatypes[value.datatype].datatype_kind == SXML_EXI_DATATYPE_ENUMERATION) {
        *result->parsed = (sxml_exi_parsed_value){.datatype_kind = SXML_EXI_DATATYPE_ENUMERATION};
        return sxml_exi_parse_enumeration(context, value, result);
    }
    return sxml_exi_parse_scalar(context, value, result);
}

/* ------------------------------------------------------------ Printing */

/* Appends value in at least two digits (the oracle's appendTwoDigits),
   or four for a year. */
typedef struct sxml_exi_padded {
    int64_t value;
    size_t width;
} sxml_exi_padded;

static sxml_status sxml_exi_append_padded(sxml_exi_buffer *output, sxml_exi_padded padded) {
    uint8_t digits[24];
    size_t digit_count = 0;
    uint64_t magnitude = padded.value < 0 ? (uint64_t)(-(padded.value + 1)) + 1U : (uint64_t)padded.value;
    do {
        digits[sizeof digits - 1 - digit_count++] = (uint8_t)('0' + magnitude % 10U);
        magnitude /= 10U;
    } while (magnitude > 0);
    while (digit_count < padded.width) {
        digits[sizeof digits - 1 - digit_count++] = '0';
    }
    sxml_status status = SXML_STATUS_OK;
    if (padded.value < 0) {
        status = sxml_exi_buffer_append(output, SXML_EXI_DATATYPE_TEXT("-"));
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_buffer_append(output,
                                        (sxml_byte_view){.bytes = digits + sizeof digits - digit_count, .byte_count = digit_count});
    }
    return status;
}

static sxml_status sxml_exi_print_two(sxml_exi_buffer *output, int64_t value) {
    return sxml_exi_append_padded(output, (sxml_exi_padded){.value = value, .width = 2});
}

/* The oracle's DateTimeValue text: the fields of the kind, fraction
   digits as written reversed (no trailing zeros), Z or +hh:mm. */
static sxml_status sxml_exi_print_date_time(const sxml_exi_parsed_value *parsed, sxml_exi_buffer *output) {
    sxml_status status = SXML_STATUS_OK;
    const uint32_t month = parsed->month_day / 32U;
    const uint32_t day = parsed->month_day - month * 32U;
    const uint32_t hour = parsed->time / 4096U;
    const uint32_t minute = (parsed->time - hour * 4096U) / 64U;
    const uint32_t second = parsed->time - hour * 4096U - minute * 64U;
    bool has_time = false;
    switch (parsed->date_time_kind) {
    case SXML_EXI_DATE_TIME_G_YEAR:
        status = sxml_exi_append_padded(output, (sxml_exi_padded){.value = parsed->year, .width = 4});
        break;
    case SXML_EXI_DATE_TIME_G_YEAR_MONTH:
        status = sxml_exi_append_padded(output, (sxml_exi_padded){.value = parsed->year, .width = 4});
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_buffer_append(output, SXML_EXI_DATATYPE_TEXT("-"));
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_print_two(output, month);
        }
        break;
    case SXML_EXI_DATE_TIME_DATE:
    case SXML_EXI_DATE_TIME_DATE_TIME:
        status = sxml_exi_append_padded(output, (sxml_exi_padded){.value = parsed->year, .width = 4});
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_buffer_append(output, SXML_EXI_DATATYPE_TEXT("-"));
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_print_two(output, month);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_buffer_append(output, SXML_EXI_DATATYPE_TEXT("-"));
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_print_two(output, day);
        }
        if (status == SXML_STATUS_OK && parsed->date_time_kind == SXML_EXI_DATE_TIME_DATE_TIME) {
            status = sxml_exi_buffer_append(output, SXML_EXI_DATATYPE_TEXT("T"));
            has_time = true;
        }
        break;
    case SXML_EXI_DATE_TIME_TIME:
        has_time = true;
        break;
    case SXML_EXI_DATE_TIME_G_MONTH:
        status = sxml_exi_buffer_append(output, SXML_EXI_DATATYPE_TEXT("--"));
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_print_two(output, month);
        }
        break;
    case SXML_EXI_DATE_TIME_G_MONTH_DAY:
        status = sxml_exi_buffer_append(output, SXML_EXI_DATATYPE_TEXT("--"));
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_print_two(output, month);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_buffer_append(output, SXML_EXI_DATATYPE_TEXT("-"));
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_print_two(output, day);
        }
        break;
    case SXML_EXI_DATE_TIME_G_DAY:
        status = sxml_exi_buffer_append(output, SXML_EXI_DATATYPE_TEXT("---"));
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_print_two(output, parsed->month_day);
        }
        break;
    }
    if (status == SXML_STATUS_OK && has_time) {
        status = sxml_exi_print_two(output, hour);
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_buffer_append(output, SXML_EXI_DATATYPE_TEXT(":"));
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_print_two(output, minute);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_buffer_append(output, SXML_EXI_DATATYPE_TEXT(":"));
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_print_two(output, second);
        }
        if (status == SXML_STATUS_OK && parsed->fraction_seconds > 0) {
            /* The digits of the reversed number, reversed back. */
            status = sxml_exi_buffer_append(output, SXML_EXI_DATATYPE_TEXT("."));
            uint32_t fraction = parsed->fraction_seconds;
            while (fraction > 0 && status == SXML_STATUS_OK) {
                const uint8_t digit = (uint8_t)('0' + fraction % 10U);
                fraction /= 10U;
                status = sxml_exi_buffer_append(output, (sxml_byte_view){.bytes = &digit, .byte_count = 1});
            }
        }
    }
    if (status == SXML_STATUS_OK && parsed->has_time_zone) {
        if (parsed->time_zone == 0) {
            status = sxml_exi_buffer_append(output, SXML_EXI_DATATYPE_TEXT("Z"));
        } else {
            const int32_t magnitude = parsed->time_zone < 0 ? -parsed->time_zone : parsed->time_zone;
            const int32_t hours = magnitude / 64;
            status = sxml_exi_buffer_append(output, parsed->time_zone < 0 ? SXML_EXI_DATATYPE_TEXT("-") : SXML_EXI_DATATYPE_TEXT("+"));
            if (status == SXML_STATUS_OK) {
                status = sxml_exi_print_two(output, hours);
            }
            if (status == SXML_STATUS_OK) {
                status = sxml_exi_buffer_append(output, SXML_EXI_DATATYPE_TEXT(":"));
            }
            if (status == SXML_STATUS_OK) {
                status = sxml_exi_print_two(output, magnitude - hours * 64);
            }
        }
    }
    return status;
}

static sxml_status sxml_exi_print_binary(const sxml_exi_print *print, sxml_exi_buffer *output) {
    static const char base64_alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    static const char hex_alphabet[] = "0123456789ABCDEF";
    const sxml_exi_parsed_value *parsed = print->parsed;
    if (parsed->byte_count == 0 || print->context->work->bytes == NULL) {
        return SXML_STATUS_OK;
    }
    const uint8_t *bytes = print->context->work->bytes + parsed->byte_offset;
    sxml_status status = SXML_STATUS_OK;
    if (parsed->datatype_kind == SXML_EXI_DATATYPE_HEX_BINARY) {
        for (size_t byte_index = 0; byte_index < parsed->byte_count && status == SXML_STATUS_OK; byte_index++) {
            const uint8_t pair[2] = {(uint8_t)hex_alphabet[bytes[byte_index] >> 4U],
                                     (uint8_t)hex_alphabet[bytes[byte_index] & 0xFU]};
            status = sxml_exi_buffer_append(output, (sxml_byte_view){.bytes = pair, .byte_count = 2});
            bytes = print->context->work->bytes + parsed->byte_offset;
        }
        return status;
    }
    for (size_t byte_index = 0; byte_index < parsed->byte_count && status == SXML_STATUS_OK; byte_index += 3) {
        bytes = print->context->work->bytes + parsed->byte_offset;
        const size_t left = parsed->byte_count - byte_index;
        const uint32_t group = ((uint32_t)bytes[byte_index] << 16U) | (left > 1 ? (uint32_t)bytes[byte_index + 1] << 8U : 0U) |
                               (left > 2 ? (uint32_t)bytes[byte_index + 2] : 0U);
        const uint8_t quad[4] = {(uint8_t)base64_alphabet[(group >> 18U) & 0x3FU],
                                 (uint8_t)base64_alphabet[(group >> 12U) & 0x3FU],
                                 left > 1 ? (uint8_t)base64_alphabet[(group >> 6U) & 0x3FU] : (uint8_t)'=',
                                 left > 2 ? (uint8_t)base64_alphabet[group & 0x3FU] : (uint8_t)'='};
        status = sxml_exi_buffer_append(output, (sxml_byte_view){.bytes = quad, .byte_count = 4});
    }
    return status;
}

/* A signed integer's text: the sign and the magnitude. Copies through
   output first: work may be output's neighbour, not output. */
static sxml_status sxml_exi_print_signed(const sxml_exi_print *print, sxml_exi_signed number, sxml_exi_buffer *output) {
    sxml_status status = SXML_STATUS_OK;
    if (number.is_negative) {
        status = sxml_exi_buffer_append(output, SXML_EXI_DATATYPE_TEXT("-"));
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_buffer_append(output, sxml_exi_magnitude(print->context->work, number));
    }
    return status;
}

static sxml_status sxml_exi_print_value(const sxml_exi_print *print, sxml_exi_buffer *output) {
    const sxml_exi_parsed_value *parsed = print->parsed;
    const sxml_exi_schema_grammars *grammars = print->context->grammars;
    sxml_status status = SXML_STATUS_OK;
    switch (parsed->datatype_kind) {
    case SXML_EXI_DATATYPE_BOOLEAN:
        return sxml_exi_buffer_append(output, parsed->boolean_value ? SXML_EXI_DATATYPE_TEXT("true")
                                                                    : SXML_EXI_DATATYPE_TEXT("false"));
    case SXML_EXI_DATATYPE_PATTERN_BOOLEAN: {
        static const sxml_byte_view codes[4] = {SXML_BYTE_VIEW_INITIALIZER("false"), SXML_BYTE_VIEW_INITIALIZER("0"),
                                                SXML_BYTE_VIEW_INITIALIZER("true"), SXML_BYTE_VIEW_INITIALIZER("1")};
        return sxml_exi_buffer_append(output, codes[parsed->boolean_code & 3U]);
    }
    case SXML_EXI_DATATYPE_DECIMAL: {
        status = sxml_exi_print_signed(print, parsed->integer, output);
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_buffer_append(output, SXML_EXI_DATATYPE_TEXT("."));
        }
        const sxml_byte_view fraction = sxml_exi_magnitude(print->context->work, parsed->fraction);
        for (size_t digit_index = fraction.byte_count; digit_index > 0 && status == SXML_STATUS_OK && fraction.bytes != NULL;
             digit_index--) {
            const uint8_t digit = fraction.bytes[digit_index - 1];
            status = sxml_exi_buffer_append(output, (sxml_byte_view){.bytes = &digit, .byte_count = 1});
        }
        return status;
    }
    case SXML_EXI_DATATYPE_FLOAT:
        if (parsed->exponent == SXML_EXI_FLOAT_SPECIAL_EXPONENT) {
            return sxml_exi_buffer_append(output, parsed->mantissa == 1    ? SXML_EXI_DATATYPE_TEXT("INF")
                                                  : parsed->mantissa == -1 ? SXML_EXI_DATATYPE_TEXT("-INF")
                                                                           : SXML_EXI_DATATYPE_TEXT("NaN"));
        }
        status = sxml_exi_append_int64(output, parsed->mantissa);
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_buffer_append(output, SXML_EXI_DATATYPE_TEXT("E"));
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_append_int64(output, parsed->exponent);
        }
        return status;
    case SXML_EXI_DATATYPE_INTEGER:
    case SXML_EXI_DATATYPE_UNSIGNED_INTEGER:
    case SXML_EXI_DATATYPE_BOUNDED_INTEGER:
        return sxml_exi_print_signed(print, parsed->integer, output);
    case SXML_EXI_DATATYPE_DATE_TIME:
        return sxml_exi_print_date_time(parsed, output);
    case SXML_EXI_DATATYPE_BASE64_BINARY:
    case SXML_EXI_DATATYPE_HEX_BINARY:
        return sxml_exi_print_binary(print, output);
    case SXML_EXI_DATATYPE_ENUMERATION:
    case SXML_EXI_DATATYPE_STRING:
    case SXML_EXI_DATATYPE_RESTRICTED_STRING:
    case SXML_EXI_DATATYPE_LIST:
        break;
    }
    (void)grammars;
    return status;
}

/* ------------------------------------------------------------- Writing */

/* An Integer (7.1.5): its sign, then the magnitude (less one when
   negative). */
typedef struct sxml_exi_integer_write {
    sxml_exi_bit_writer *writer;
    const sxml_exi_value_context *context;
    sxml_exi_signed number;
} sxml_exi_integer_write;

static sxml_status sxml_exi_write_signed(const sxml_exi_integer_write *write) {
    sxml_exi_buffer *work = write->context->work;
    sxml_status status = sxml_exi_write_boolean(write->writer, (sxml_exi_boolean){.value = write->number.is_negative});
    if (status != SXML_STATUS_OK) {
        return status;
    }
    if (!write->number.is_negative) {
        return sxml_exi_write_magnitude(&(sxml_exi_magnitude_write){
            .writer = write->writer, .digits = sxml_exi_magnitude(work, write->number), .work = work});
    }
    const size_t start = work->byte_count;
    sxml_exi_buffer copy = {.allocator = work->allocator};
    status = sxml_exi_buffer_append(&copy, sxml_exi_magnitude(work, write->number));
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_buffer_append(work, sxml_exi_buffer_view(&copy));
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_step_magnitude(&(sxml_exi_step_request){.buffer = work, .offset = start, .is_increment = false});
    }
    if (status == SXML_STATUS_OK) {
        sxml_exi_buffer_release(&copy);
        status = sxml_exi_buffer_append(&copy, (sxml_byte_view){.bytes = work->bytes + start, .byte_count = work->byte_count - start});
    }
    if (status == SXML_STATUS_OK) {
        work->byte_count = start;
        status = sxml_exi_write_magnitude(
            &(sxml_exi_magnitude_write){.writer = write->writer, .digits = sxml_exi_buffer_view(&copy), .work = work});
    }
    sxml_exi_buffer_release(&copy);
    return status;
}

/* An Integer of 64 bits (Float's parts, Date-Time's year). */
static sxml_status sxml_exi_write_int64(sxml_exi_bit_writer *writer, int64_t value) {
    sxml_status status = sxml_exi_write_boolean(writer, (sxml_exi_boolean){.value = value < 0});
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_write_unsigned(writer, value < 0 ? (uint64_t)(-(value + 1)) : (uint64_t)value);
    }
    return status;
}

/* Writes a parsed value in its representation (7.1, 7.2). */
typedef struct sxml_exi_parsed_write {
    sxml_exi_bit_writer *writer;
    const sxml_exi_value_context *context;
    uint32_t datatype;
} sxml_exi_parsed_write;

static sxml_status sxml_exi_write_date_time(sxml_exi_bit_writer *writer, const sxml_exi_parsed_value *parsed) {
    const sxml_exi_date_time_kind kind = parsed->date_time_kind;
    sxml_status status = SXML_STATUS_OK;
    const bool has_year = kind == SXML_EXI_DATE_TIME_G_YEAR || kind == SXML_EXI_DATE_TIME_G_YEAR_MONTH ||
                          kind == SXML_EXI_DATE_TIME_DATE || kind == SXML_EXI_DATE_TIME_DATE_TIME;
    const bool has_month_day = kind != SXML_EXI_DATE_TIME_G_YEAR && kind != SXML_EXI_DATE_TIME_TIME;
    const bool has_time = kind == SXML_EXI_DATE_TIME_DATE_TIME || kind == SXML_EXI_DATE_TIME_TIME;
    if (has_year) {
        status = sxml_exi_write_int64(writer, parsed->year - SXML_EXI_YEAR_OFFSET);
    }
    if (status == SXML_STATUS_OK && has_month_day) {
        status = sxml_exi_write_n_bit(writer, (sxml_exi_bits){.value = parsed->month_day & 0x1FFU,
                                                              .bit_count = SXML_EXI_MONTH_DAY_BITS});
    }
    if (status == SXML_STATUS_OK && has_time) {
        status = sxml_exi_write_n_bit(writer, (sxml_exi_bits){.value = parsed->time & 0x1FFFFU, .bit_count = SXML_EXI_TIME_BITS});
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_write_boolean(writer, (sxml_exi_boolean){.value = parsed->fraction_seconds != 0});
        }
        if (status == SXML_STATUS_OK && parsed->fraction_seconds != 0) {
            status = sxml_exi_write_unsigned(writer, parsed->fraction_seconds);
        }
    }
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_write_boolean(writer, (sxml_exi_boolean){.value = parsed->has_time_zone});
    }
    if (status == SXML_STATUS_OK && parsed->has_time_zone) {
        status = sxml_exi_write_n_bit(writer, (sxml_exi_bits){.value = (uint32_t)(parsed->time_zone + SXML_EXI_TIME_ZONE_OFFSET) & 0x7FFU,
                                                              .bit_count = SXML_EXI_TIME_ZONE_BITS});
    }
    return status;
}

static sxml_status sxml_exi_write_parsed(const sxml_exi_parsed_write *write, const sxml_exi_parsed_value *parsed) {
    const sxml_exi_datatype *datatype = &write->context->grammars->datatypes[write->datatype];
    sxml_exi_bit_writer *writer = write->writer;
    sxml_exi_buffer *work = write->context->work;
    sxml_status status = SXML_STATUS_OK;
    switch (datatype->datatype_kind) {
    case SXML_EXI_DATATYPE_BOOLEAN:
        return sxml_exi_write_boolean(writer, (sxml_exi_boolean){.value = parsed->boolean_value});
    case SXML_EXI_DATATYPE_PATTERN_BOOLEAN:
        return sxml_exi_write_n_bit(writer, (sxml_exi_bits){.value = parsed->boolean_code, .bit_count = 2});
    case SXML_EXI_DATATYPE_DECIMAL:
        status = sxml_exi_write_boolean(writer, (sxml_exi_boolean){.value = parsed->integer.is_negative});
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_write_magnitude(&(sxml_exi_magnitude_write){
                .writer = writer, .digits = sxml_exi_magnitude(work, parsed->integer), .work = work});
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_write_magnitude(&(sxml_exi_magnitude_write){
                .writer = writer, .digits = sxml_exi_magnitude(work, parsed->fraction), .work = work});
        }
        return status;
    case SXML_EXI_DATATYPE_FLOAT:
        status = sxml_exi_write_int64(writer, parsed->mantissa);
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_write_int64(writer, parsed->exponent);
        }
        return status;
    case SXML_EXI_DATATYPE_INTEGER:
        return sxml_exi_write_signed(
            &(sxml_exi_integer_write){.writer = writer, .context = write->context, .number = parsed->integer});
    case SXML_EXI_DATATYPE_UNSIGNED_INTEGER:
        return sxml_exi_write_magnitude(&(sxml_exi_magnitude_write){
            .writer = writer, .digits = sxml_exi_magnitude(work, parsed->integer), .work = work});
    case SXML_EXI_DATATYPE_BOUNDED_INTEGER:
    case SXML_EXI_DATATYPE_ENUMERATION:
        return sxml_exi_write_n_bit(writer, (sxml_exi_bits){.value = parsed->ordinal, .bit_count = datatype->bit_count});
    case SXML_EXI_DATATYPE_DATE_TIME:
        return sxml_exi_write_date_time(writer, parsed);
    case SXML_EXI_DATATYPE_BASE64_BINARY:
    case SXML_EXI_DATATYPE_HEX_BINARY:
        status = sxml_exi_write_unsigned(writer, parsed->byte_count);
        for (size_t byte_index = 0; byte_index < parsed->byte_count && status == SXML_STATUS_OK; byte_index++) {
            status = sxml_exi_write_bits(writer, (sxml_exi_bits){.value = work->bytes[parsed->byte_offset + byte_index], .bit_count = 8});
        }
        return status;
    case SXML_EXI_DATATYPE_STRING:
    case SXML_EXI_DATATYPE_RESTRICTED_STRING:
    case SXML_EXI_DATATYPE_LIST:
        break;
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

sxml_status sxml_exi_write_typed_value(const sxml_exi_typed_write *write, sxml_exi_value_text value,
                                       sxml_exi_validity *validity) {
    sxml_exi_buffer work = {.allocator = write->grammars->allocator};
    const sxml_exi_value_context context = {.grammars = write->grammars, .work = &work};
    sxml_exi_parsed_value parsed = {0};
    sxml_exi_parse_result result = {.parsed = &parsed};
    sxml_status status = sxml_exi_parse_value(&context, value, &result);
    validity->is_valid = status == SXML_STATUS_OK && result.is_valid;
    if (validity->is_valid && write->writer != NULL) {
        status = sxml_exi_write_parsed(
            &(sxml_exi_parsed_write){.writer = write->writer, .context = &context, .datatype = value.datatype}, &parsed);
    }
    sxml_exi_buffer_release(&work);
    return status;
}

sxml_status sxml_exi_canonicalize_value(const sxml_exi_schema_grammars *grammars, sxml_exi_value_text value,
                                        sxml_exi_canonical_output *output) {
    sxml_exi_buffer work = {.allocator = grammars->allocator};
    const sxml_exi_value_context context = {.grammars = grammars, .work = &work};
    sxml_exi_parsed_value parsed = {0};
    sxml_exi_parse_result result = {.parsed = &parsed};
    sxml_status status = sxml_exi_parse_value(&context, value, &result);
    output->is_valid = status == SXML_STATUS_OK && result.is_valid;
    if (output->is_valid) {
        status = sxml_exi_print_value(&(sxml_exi_print){.context = &context, .parsed = &parsed}, output->buffer);
    }
    sxml_exi_buffer_release(&work);
    return status;
}

/* ------------------------------------------------------------- Reading */

/* An Integer of 64 bits; MALFORMED_INPUT past them. */
static sxml_status sxml_exi_read_int64(sxml_exi_bit_reader *reader, int64_t *value) {
    sxml_exi_boolean sign = {0};
    uint64_t magnitude = 0;
    sxml_status status = sxml_exi_read_boolean(reader, &sign);
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_read_unsigned(reader, &magnitude);
    }
    if (status == SXML_STATUS_OK && magnitude > (uint64_t)INT64_MAX) {
        status = SXML_STATUS_MALFORMED_INPUT;
    }
    if (status == SXML_STATUS_OK) {
        *value = sign.value ? -(int64_t)magnitude - 1 : (int64_t)magnitude;
    }
    return status;
}

static sxml_status sxml_exi_read_date_time(sxml_exi_bit_reader *reader, sxml_exi_parsed_value *parsed) {
    const sxml_exi_date_time_kind kind = parsed->date_time_kind;
    const bool has_year = kind == SXML_EXI_DATE_TIME_G_YEAR || kind == SXML_EXI_DATE_TIME_G_YEAR_MONTH ||
                          kind == SXML_EXI_DATE_TIME_DATE || kind == SXML_EXI_DATE_TIME_DATE_TIME;
    const bool has_month_day = kind != SXML_EXI_DATE_TIME_G_YEAR && kind != SXML_EXI_DATE_TIME_TIME;
    const bool has_time = kind == SXML_EXI_DATE_TIME_DATE_TIME || kind == SXML_EXI_DATE_TIME_TIME;
    sxml_status status = SXML_STATUS_OK;
    if (has_year) {
        int64_t year = 0;
        status = sxml_exi_read_int64(reader, &year);
        if (status == SXML_STATUS_OK && (year > INT64_C(1) << 40 || year < -(INT64_C(1) << 40))) {
            status = SXML_STATUS_MALFORMED_INPUT;
        }
        parsed->year = year + SXML_EXI_YEAR_OFFSET;
    }
    if (status == SXML_STATUS_OK && has_month_day) {
        status = sxml_exi_read_n_bit(reader, (sxml_exi_bits_request){.bit_count = SXML_EXI_MONTH_DAY_BITS}, &parsed->month_day);
    }
    if (status == SXML_STATUS_OK && has_time) {
        status = sxml_exi_read_n_bit(reader, (sxml_exi_bits_request){.bit_count = SXML_EXI_TIME_BITS}, &parsed->time);
        sxml_exi_boolean has_fraction = {0};
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_read_boolean(reader, &has_fraction);
        }
        if (status == SXML_STATUS_OK && has_fraction.value) {
            uint64_t fraction = 0;
            status = sxml_exi_read_unsigned(reader, &fraction);
            if (status == SXML_STATUS_OK && fraction > SXML_EXI_FRACTION_LIMIT) {
                status = SXML_STATUS_MALFORMED_INPUT;
            }
            parsed->fraction_seconds = (uint32_t)fraction;
        }
    }
    sxml_exi_boolean has_time_zone = {0};
    if (status == SXML_STATUS_OK) {
        status = sxml_exi_read_boolean(reader, &has_time_zone);
    }
    if (status == SXML_STATUS_OK && has_time_zone.value) {
        uint32_t time_zone = 0;
        status = sxml_exi_read_n_bit(reader, (sxml_exi_bits_request){.bit_count = SXML_EXI_TIME_ZONE_BITS}, &time_zone);
        parsed->has_time_zone = true;
        parsed->time_zone = (int32_t)time_zone - SXML_EXI_TIME_ZONE_OFFSET;
    }
    if (status == SXML_STATUS_OK) {
        sxml_exi_normalize_date_time(parsed);
    }
    return status;
}

/* Reads a magnitude into work as a signed number. */
typedef struct sxml_exi_signed_read {
    sxml_exi_bit_reader *reader;
    sxml_exi_buffer *work;
    bool is_negative;
} sxml_exi_signed_read;

static sxml_status sxml_exi_read_signed_magnitude(const sxml_exi_signed_read *read, sxml_exi_signed *number) {
    sxml_exi_buffer groups = {.allocator = read->work->allocator};
    const size_t offset = read->work->byte_count;
    sxml_status status = sxml_exi_read_magnitude(
        &(sxml_exi_magnitude_read){.reader = read->reader, .output = read->work, .work = &groups});
    sxml_exi_buffer_release(&groups);
    *number = (sxml_exi_signed){.is_negative = read->is_negative, .offset = offset, .digit_count = read->work->byte_count - offset};
    return status;
}

static sxml_status sxml_exi_read_parsed(const sxml_exi_typed_read *read, const sxml_exi_value_context *context,
                                        sxml_exi_parsed_value *parsed);

sxml_status sxml_exi_read_typed_value(const sxml_exi_typed_read *read, uint32_t datatype) {
    sxml_exi_buffer work = {.allocator = read->grammars->allocator};
    const sxml_exi_value_context context = {.grammars = read->grammars, .work = &work};
    const sxml_exi_datatype *entry = &read->grammars->datatypes[datatype];
    sxml_exi_parsed_value parsed = {.datatype_kind = entry->datatype_kind, .date_time_kind = entry->date_time_kind,
                                    .ordinal = datatype};
    sxml_status status = sxml_exi_read_parsed(read, &context, &parsed);
    if (status == SXML_STATUS_OK && entry->datatype_kind == SXML_EXI_DATATYPE_ENUMERATION) {
        const sxml_exi_text stored = read->grammars->enumeration_values[entry->first_value + parsed.ordinal];
        status = sxml_exi_buffer_append(read->output, (sxml_byte_view){.bytes = read->grammars->text.bytes + stored.offset,
                                                                       .byte_count = stored.byte_count});
    } else if (status == SXML_STATUS_OK) {
        status = sxml_exi_print_value(&(sxml_exi_print){.context = &context, .parsed = &parsed}, read->output);
    }
    sxml_exi_buffer_release(&work);
    return status;
}

/* Reads the bits of a value; parsed->ordinal comes in as the datatype. */
static sxml_status sxml_exi_read_parsed(const sxml_exi_typed_read *read, const sxml_exi_value_context *context,
                                        sxml_exi_parsed_value *parsed) {
    const sxml_exi_datatype *datatype = &read->grammars->datatypes[parsed->ordinal];
    sxml_exi_bit_reader *reader = read->reader;
    sxml_exi_buffer *work = context->work;
    sxml_status status = SXML_STATUS_OK;
    parsed->ordinal = 0;
    switch (datatype->datatype_kind) {
    case SXML_EXI_DATATYPE_BOOLEAN: {
        sxml_exi_boolean boolean = {0};
        status = sxml_exi_read_boolean(reader, &boolean);
        parsed->boolean_value = boolean.value;
        return status;
    }
    case SXML_EXI_DATATYPE_PATTERN_BOOLEAN:
        return sxml_exi_read_n_bit(reader, (sxml_exi_bits_request){.bit_count = 2}, &parsed->boolean_code);
    case SXML_EXI_DATATYPE_DECIMAL: {
        sxml_exi_boolean sign = {0};
        status = sxml_exi_read_boolean(reader, &sign);
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_read_signed_magnitude(
                &(sxml_exi_signed_read){.reader = reader, .work = work, .is_negative = sign.value}, &parsed->integer);
        }
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_read_signed_magnitude(&(sxml_exi_signed_read){.reader = reader, .work = work},
                                                    &parsed->fraction);
        }
        if (status == SXML_STATUS_OK && sxml_exi_is_zero(sxml_exi_magnitude(work, parsed->integer)) &&
            sxml_exi_is_zero(sxml_exi_magnitude(work, parsed->fraction))) {
            parsed->integer.is_negative = false;
        }
        return status;
    }
    case SXML_EXI_DATATYPE_FLOAT:
        status = sxml_exi_read_int64(reader, &parsed->mantissa);
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_read_int64(reader, &parsed->exponent);
        }
        if (status == SXML_STATUS_OK) {
            sxml_exi_normalize_float(parsed);
        }
        return status;
    case SXML_EXI_DATATYPE_INTEGER: {
        sxml_exi_boolean sign = {0};
        status = sxml_exi_read_boolean(reader, &sign);
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_read_signed_magnitude(
                &(sxml_exi_signed_read){.reader = reader, .work = work, .is_negative = sign.value}, &parsed->integer);
        }
        if (status == SXML_STATUS_OK && sign.value) {
            status = sxml_exi_step_magnitude(
                &(sxml_exi_step_request){.buffer = work, .offset = parsed->integer.offset, .is_increment = true});
            parsed->integer.digit_count = work->byte_count - parsed->integer.offset;
        }
        return status;
    }
    case SXML_EXI_DATATYPE_UNSIGNED_INTEGER:
        return sxml_exi_read_signed_magnitude(&(sxml_exi_signed_read){.reader = reader, .work = work}, &parsed->integer);
    case SXML_EXI_DATATYPE_BOUNDED_INTEGER: {
        uint32_t offset = 0;
        status = sxml_exi_read_n_bit(reader, (sxml_exi_bits_request){.bit_count = datatype->bit_count}, &offset);
        if (status == SXML_STATUS_OK && offset >= datatype->value_count) {
            return SXML_STATUS_MALFORMED_INPUT;
        }
        /* lower + offset, from the lower bound's text. */
        sxml_exi_signed_result lower = {0};
        const sxml_byte_view lower_text = {.bytes = read->grammars->text.bytes + datatype->lower_bound.offset,
                                           .byte_count = datatype->lower_bound.byte_count};
        if (status == SXML_STATUS_OK) {
            status = sxml_exi_parse_signed(&(sxml_exi_integer_parse){.text = lower_text, .work = work}, &lower);
        }
        sxml_exi_signed distance = {.offset = work->byte_count};
        if (status == SXML_STATUS_OK) {
            /* -(offset): lower - (-offset) = lower + offset. */
            status = sxml_exi_append_int64(work, offset);
            distance.digit_count = work->byte_count - distance.offset;
            distance.is_negative = offset != 0;
        }
        if (status == SXML_STATUS_OK && lower.is_valid) {
            status = sxml_exi_signed_difference(
                &(sxml_exi_signed_pair){.first = lower.number, .second = distance, .work = work}, &parsed->integer);
        }
        return status;
    }
    case SXML_EXI_DATATYPE_ENUMERATION:
        status = sxml_exi_read_n_bit(reader, (sxml_exi_bits_request){.bit_count = datatype->bit_count}, &parsed->ordinal);
        if (status == SXML_STATUS_OK && parsed->ordinal >= datatype->value_count) {
            status = SXML_STATUS_MALFORMED_INPUT;
        }
        return status;
    case SXML_EXI_DATATYPE_DATE_TIME:
        return sxml_exi_read_date_time(reader, parsed);
    case SXML_EXI_DATATYPE_BASE64_BINARY:
    case SXML_EXI_DATATYPE_HEX_BINARY: {
        uint64_t byte_count = 0;
        status = sxml_exi_read_unsigned(reader, &byte_count);
        if (status == SXML_STATUS_OK && byte_count > reader->input.byte_count) {
            return SXML_STATUS_UNEXPECTED_END_OF_INPUT;
        }
        parsed->byte_offset = work->byte_count;
        for (uint64_t byte_index = 0; byte_index < byte_count && status == SXML_STATUS_OK; byte_index++) {
            uint32_t byte = 0;
            status = sxml_exi_read_bits(reader, (sxml_exi_bits_request){.bit_count = 8}, &byte);
            const uint8_t octet = (uint8_t)byte;
            if (status == SXML_STATUS_OK) {
                status = sxml_exi_buffer_append(work, (sxml_byte_view){.bytes = &octet, .byte_count = 1});
            }
        }
        parsed->byte_count = work->byte_count - parsed->byte_offset;
        return status;
    }
    case SXML_EXI_DATATYPE_STRING:
    case SXML_EXI_DATATYPE_RESTRICTED_STRING:
    case SXML_EXI_DATATYPE_LIST:
        break;
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}
