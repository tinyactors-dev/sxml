/*
 * Arithmetic on durations, dates and times (src/operator_internal.h).
 *
 * Golden file: src/memory.c (and src/operator.c).
 *
 * Durations are months plus seconds and nanoseconds (D36). A day is a
 * position in a 400-year Gregorian cycle of 146097 days, counted from
 * 1 March so that leap days come last (the days-from-civil method of
 * H. Hinnant, "chrono-Compatible Low-Level Date Algorithms"); cycles are
 * counted separately, so every year of int64_t can be represented and only
 * results beyond it overflow (FODT0001). Year 0 is 1 BCE (XSD 1.1
 * numbering), which the proleptic Gregorian arithmetic gives directly.
 *
 * Specification: XPath 4.0, section 4.9 (the operator table); F&O 4.0,
 * section 8.5 "Arithmetic operators on durations" (with PR2628: a number
 * multiplying or dividing any xs:duration is cast to xs:decimal, months
 * rounded by fn:round#1, seconds to the nanosecond) and section 9.8
 * "Arithmetic operators on durations, dates, and times"; XSD 1.1 Part 2,
 * Appendix E "Adding durations to dateTimes".
 */
#include "cast_internal.h"
#include "operator_internal.h"

#include <math.h>

/* --------------------------------------------------------------- Errors */

static const sxml_static_error sxml_temporal_operand_type_error = {
    .error_status = SXML_STATUS_TYPE_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("XPTY0004"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "the operator is not defined for operands of these types (XPath "
        "4.0, section 4.9, binary arithmetic operators)"),
};

static const sxml_static_error sxml_temporal_duration_overflow_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FODT0002"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "duration overflow: the months or seconds do not fit in 64 bits, or "
        "a duration was divided by zero (F&O 4.0, section 8.1.2)"),
};

static const sxml_static_error sxml_temporal_date_overflow_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FODT0001"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "date/time overflow: the year of the result does not fit in 64 bits "
        "(F&O 4.0, section 9.8)"),
};

static const sxml_static_error sxml_temporal_not_a_number_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOCA0005"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "a duration cannot be multiplied or divided by NaN (F&O 4.0, "
        "op:multiply-duration-by-number)"),
};

static const sxml_static_error sxml_temporal_ratio_by_zero_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOAR0001"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "division by a zero-length duration (F&O 4.0, "
        "op:divide-yearMonthDuration-by-yearMonthDuration)"),
};

static const sxml_static_error sxml_temporal_digit_limit_error = {
    .error_status = SXML_STATUS_DYNAMIC_ERROR,
    .code_local_name = SXML_BYTE_VIEW_INITIALIZER("FOAR0002"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "numeric overflow: the result has more digits than the processor "
        "keeps (F&O 4.0, section 4.2)"),
};

/* ---------------------------------------------------- Operand kinds */

/* What an operand of temporal arithmetic is. */
typedef enum sxml_temporal_operand_kind {
    SXML_TEMPORAL_OPERAND_KIND_OTHER,
    SXML_TEMPORAL_OPERAND_KIND_NUMBER,
    SXML_TEMPORAL_OPERAND_KIND_YEAR_MONTH_DURATION,
    SXML_TEMPORAL_OPERAND_KIND_DAY_TIME_DURATION,
    /* An xs:duration that is neither of the two above. */
    SXML_TEMPORAL_OPERAND_KIND_DURATION,
    SXML_TEMPORAL_OPERAND_KIND_DATE_TIME,
    SXML_TEMPORAL_OPERAND_KIND_DATE,
    SXML_TEMPORAL_OPERAND_KIND_TIME,
} sxml_temporal_operand_kind;

static sxml_temporal_operand_kind
sxml_temporal_operand_kind_of(const sxml_item_heap *item_heap, sxml_item item) {
    if (sxml_atomic_item_is_numeric(item)) {
        return SXML_TEMPORAL_OPERAND_KIND_NUMBER;
    }
    switch (sxml_atomic_item_get_primitive_type(item).index) {
    case SXML_TYPE_XS_DURATION:
        if (sxml_operator_item_has_type(
                item_heap,
                (sxml_type_test){.item = item,
                                 .type = SXML_TYPE_XS_YEAR_MONTH_DURATION})) {
            return SXML_TEMPORAL_OPERAND_KIND_YEAR_MONTH_DURATION;
        }
        if (sxml_operator_item_has_type(
                item_heap,
                (sxml_type_test){.item = item,
                                 .type = SXML_TYPE_XS_DAY_TIME_DURATION})) {
            return SXML_TEMPORAL_OPERAND_KIND_DAY_TIME_DURATION;
        }
        return SXML_TEMPORAL_OPERAND_KIND_DURATION;
    case SXML_TYPE_XS_DATE_TIME:
        return SXML_TEMPORAL_OPERAND_KIND_DATE_TIME;
    case SXML_TYPE_XS_DATE:
        return SXML_TEMPORAL_OPERAND_KIND_DATE;
    case SXML_TYPE_XS_TIME:
        return SXML_TEMPORAL_OPERAND_KIND_TIME;
    default:
        return SXML_TEMPORAL_OPERAND_KIND_OTHER;
    }
}

static bool
sxml_temporal_operand_kind_is_duration(sxml_temporal_operand_kind kind) {
    return kind == SXML_TEMPORAL_OPERAND_KIND_YEAR_MONTH_DURATION ||
           kind == SXML_TEMPORAL_OPERAND_KIND_DAY_TIME_DURATION ||
           kind == SXML_TEMPORAL_OPERAND_KIND_DURATION;
}

static bool
sxml_temporal_operand_kind_is_point(sxml_temporal_operand_kind kind) {
    return kind == SXML_TEMPORAL_OPERAND_KIND_DATE_TIME ||
           kind == SXML_TEMPORAL_OPERAND_KIND_DATE ||
           kind == SXML_TEMPORAL_OPERAND_KIND_TIME;
}

/* The duration of a duration item (never fails for one). */
static sxml_duration sxml_duration_of(sxml_item item) {
    sxml_atomic_datum datum;
    if (sxml_atomic_item_get_datum(item, &datum) != SXML_STATUS_OK) {
        return (sxml_duration){.months = 0, .seconds = 0, .nanoseconds = 0};
    }
    return datum.datum_details.duration;
}

static sxml_date_time sxml_date_time_of(sxml_item item) {
    sxml_atomic_datum datum;
    sxml_date_time date_time = {.year = 0,
                                .month = 0,
                                .day = 0,
                                .hour = 0,
                                .minute = 0,
                                .second = 0,
                                .nanosecond = 0,
                                .has_timezone = false,
                                .timezone_offset_minutes = 0};
    if (sxml_atomic_item_get_datum(item, &datum) == SXML_STATUS_OK) {
        date_time = datum.datum_details.date_time;
    }
    return date_time;
}

/* ---------------------------------------------------- Checked integers */

typedef struct sxml_integer_pair {
    int64_t first;
    int64_t second;
} sxml_integer_pair;

/* True, with *sum written, when first + second fits in int64_t. */
static bool sxml_checked_add(sxml_integer_pair pair, int64_t *sum) {
    if ((pair.second > 0 && pair.first > INT64_MAX - pair.second) ||
        (pair.second < 0 && pair.first < INT64_MIN - pair.second)) {
        return false;
    }
    *sum = pair.first + pair.second;
    return true;
}

static bool sxml_checked_multiply(sxml_integer_pair pair, int64_t *product) {
    int64_t first = pair.first;
    int64_t second = pair.second;
    if (first == 0 || second == 0) {
        *product = 0;
        return true;
    }
    bool overflows = false;
    if (first > 0) {
        overflows = second > 0 ? first > INT64_MAX / second
                               : second < INT64_MIN / first;
    } else {
        overflows = second > 0 ? first < INT64_MIN / second
                               : second < INT64_MAX / first;
    }
    if (overflows) {
        return false;
    }
    *product = first * second;
    return true;
}

/* Floor division and the remainder that goes with it (divisor > 0). */
static int64_t sxml_floor_divide(int64_t dividend, int64_t divisor) {
    int64_t quotient = dividend / divisor;
    return (dividend % divisor < 0) ? quotient - 1 : quotient;
}

static int64_t sxml_floor_modulo(int64_t dividend, int64_t divisor) {
    int64_t remainder = dividend % divisor;
    return remainder < 0 ? remainder + divisor : remainder;
}

/* --------------------------------------------------------- Calendar */

enum {
    SXML_DAYS_PER_CYCLE = 146097,
    SXML_SECONDS_PER_DAY = 86400,
    SXML_NANOSECONDS_PER_SECOND = 1000000000,
};

/*
 * A day: the number of its 400-year cycle and its position in it, from 0
 * (1 March of the cycle's first year) to 146096.
 */
typedef struct sxml_cycle_day {
    int64_t cycle_number;
    int64_t day_of_cycle;
} sxml_cycle_day;

/* A moment on a day: the day plus seconds and nanoseconds into it. */
typedef struct sxml_moment {
    sxml_cycle_day day;
    int64_t second_of_day;
    int64_t nanosecond;
} sxml_moment;

/* The day of a civil date; year must be above INT64_MIN. */
static sxml_cycle_day sxml_cycle_day_of(const sxml_date_time *date_time) {
    int64_t year = date_time->year - (date_time->month <= 2 ? 1 : 0);
    int64_t cycle_number = sxml_floor_divide(year, 400);
    int64_t year_of_cycle = year - cycle_number * 400;
    int64_t month = date_time->month;
    int64_t day_of_year = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 +
                          date_time->day - 1;
    return (sxml_cycle_day){
        .cycle_number = cycle_number,
        .day_of_cycle = year_of_cycle * 365 + year_of_cycle / 4 -
                        year_of_cycle / 100 + day_of_year,
    };
}

/* Writes the civil date of day into *date_time (year, month, day); false
   when the year does not fit in int64_t. */
static bool sxml_civil_date_of(sxml_cycle_day day, sxml_date_time *date_time) {
    int64_t day_of_cycle = day.day_of_cycle;
    int64_t year_of_cycle = (day_of_cycle - day_of_cycle / 1460 +
                             day_of_cycle / 36524 - day_of_cycle / 146096) /
                            365;
    int64_t day_of_year =
        day_of_cycle -
        (365 * year_of_cycle + year_of_cycle / 4 - year_of_cycle / 100);
    int64_t month_from_march = (5 * day_of_year + 2) / 153;
    int64_t day_of_month = day_of_year - (153 * month_from_march + 2) / 5 + 1;
    int64_t month =
        month_from_march < 10 ? month_from_march + 3 : month_from_march - 9;
    int64_t cycle_years = 0;
    int64_t year = 0;
    if (!sxml_checked_multiply(
            (sxml_integer_pair){.first = day.cycle_number, .second = 400},
            &cycle_years) ||
        !sxml_checked_add(
            (sxml_integer_pair){.first = cycle_years,
                                .second = year_of_cycle + (month <= 2 ? 1 : 0)},
            &year)) {
        return false;
    }
    date_time->year = year;
    date_time->month = (uint8_t)month;
    date_time->day = (uint8_t)day_of_month;
    return true;
}

/* Moves day by day_count days; false on overflow of the cycle number. */
static bool sxml_cycle_day_add(sxml_cycle_day *day, int64_t day_count) {
    int64_t day_of_cycle = day->day_of_cycle + day_count % SXML_DAYS_PER_CYCLE;
    int64_t cycle_count = day_count / SXML_DAYS_PER_CYCLE +
                          sxml_floor_divide(day_of_cycle, SXML_DAYS_PER_CYCLE);
    int64_t cycle_number = 0;
    if (!sxml_checked_add((sxml_integer_pair){.first = day->cycle_number,
                                              .second = cycle_count},
                          &cycle_number)) {
        return false;
    }
    day->cycle_number = cycle_number;
    day->day_of_cycle = sxml_floor_modulo(day_of_cycle, SXML_DAYS_PER_CYCLE);
    return true;
}

/* The days from second to first; false on overflow. */
static bool sxml_cycle_days_between(sxml_cycle_day first, sxml_cycle_day second,
                                    int64_t *day_count) {
    int64_t cycle_difference = 0;
    int64_t cycle_days = 0;
    if (second.cycle_number == INT64_MIN ||
        !sxml_checked_add((sxml_integer_pair){.first = first.cycle_number,
                                              .second = -second.cycle_number},
                          &cycle_difference) ||
        !sxml_checked_multiply(
            (sxml_integer_pair){.first = cycle_difference,
                                .second = SXML_DAYS_PER_CYCLE},
            &cycle_days)) {
        return false;
    }
    return sxml_checked_add(
        (sxml_integer_pair){.first = cycle_days,
                            .second = first.day_of_cycle - second.day_of_cycle},
        day_count);
}

/* The time of day of a date/time datum, in seconds. */
static int64_t sxml_second_of_day_of(const sxml_date_time *date_time) {
    return ((int64_t)date_time->hour * 60 + date_time->minute) * 60 +
           date_time->second;
}

/* The reference date of op:subtract-times and time arithmetic. */
static const sxml_date_time sxml_reference_date = {.year = 1972,
                                                   .month = 12,
                                                   .day = 31,
                                                   .hour = 0,
                                                   .minute = 0,
                                                   .second = 0,
                                                   .nanosecond = 0,
                                                   .has_timezone = false,
                                                   .timezone_offset_minutes =
                                                       0};

/* The moment of a date/time item in local time: dates at 00:00:00, times
   on the reference date. */
static sxml_moment sxml_local_moment_of(sxml_temporal_operand_kind kind,
                                        const sxml_date_time *date_time) {
    sxml_cycle_day day = kind == SXML_TEMPORAL_OPERAND_KIND_TIME
                             ? sxml_cycle_day_of(&sxml_reference_date)
                             : sxml_cycle_day_of(date_time);
    bool has_time = kind != SXML_TEMPORAL_OPERAND_KIND_DATE;
    return (sxml_moment){
        .day = day,
        .second_of_day = has_time ? sxml_second_of_day_of(date_time) : 0,
        .nanosecond = has_time ? (int64_t)date_time->nanosecond : 0,
    };
}

/* Moves moment by seconds and nanoseconds of the same sign; false when the
   cycle number overflows. */
typedef struct sxml_time_shift {
    int64_t seconds;
    int64_t nanoseconds;
} sxml_time_shift;

static bool sxml_moment_shift(sxml_moment *moment, sxml_time_shift shift) {
    int64_t nanosecond = moment->nanosecond + shift.nanoseconds;
    int64_t second_carry =
        sxml_floor_divide(nanosecond, SXML_NANOSECONDS_PER_SECOND);
    nanosecond = sxml_floor_modulo(nanosecond, SXML_NANOSECONDS_PER_SECOND);
    int64_t second_of_day = moment->second_of_day +
                            shift.seconds % SXML_SECONDS_PER_DAY + second_carry;
    int64_t day_count = shift.seconds / SXML_SECONDS_PER_DAY +
                        sxml_floor_divide(second_of_day, SXML_SECONDS_PER_DAY);
    sxml_cycle_day day = moment->day;
    if (!sxml_cycle_day_add(&day, day_count)) {
        return false;
    }
    *moment = (sxml_moment){
        .day = day,
        .second_of_day = sxml_floor_modulo(second_of_day, SXML_SECONDS_PER_DAY),
        .nanosecond = nanosecond,
    };
    return true;
}

/* ------------------------------------------------------- Making items */

/* The duration annotation of a kind. */
static sxml_builtin_type
sxml_duration_type_of(sxml_temporal_operand_kind kind) {
    if (kind == SXML_TEMPORAL_OPERAND_KIND_YEAR_MONTH_DURATION) {
        return SXML_TYPE_XS_YEAR_MONTH_DURATION;
    }
    if (kind == SXML_TEMPORAL_OPERAND_KIND_DAY_TIME_DURATION) {
        return SXML_TYPE_XS_DAY_TIME_DURATION;
    }
    return SXML_TYPE_XS_DURATION;
}

/* A duration to make with its annotation. */
typedef struct sxml_typed_duration {
    sxml_builtin_type type;
    sxml_duration duration;
} sxml_typed_duration;

static sxml_status sxml_typed_duration_make(sxml_item_heap *item_heap,
                                            sxml_typed_duration typed_duration,
                                            sxml_item *item) {
    return sxml_atomic_item_make(
        item_heap,
        &(sxml_atomic_item_request){
            .type = sxml_builtin_type_identifier(typed_duration.type),
            .datum = {.datum_kind = SXML_DATUM_KIND_DURATION,
                      .datum_details = {.duration = typed_duration.duration}}},
        item);
}

/* A date/time result: its kind and its datum. */
typedef struct sxml_typed_date_time {
    sxml_temporal_operand_kind kind;
    sxml_date_time date_time;
} sxml_typed_date_time;

static sxml_status sxml_date_time_item_make(sxml_item_heap *item_heap,
                                            sxml_typed_date_time typed,
                                            sxml_item *item) {
    sxml_builtin_type type = typed.kind == SXML_TEMPORAL_OPERAND_KIND_DATE
                                 ? SXML_TYPE_XS_DATE
                             : typed.kind == SXML_TEMPORAL_OPERAND_KIND_TIME
                                 ? SXML_TYPE_XS_TIME
                                 : SXML_TYPE_XS_DATE_TIME;
    return sxml_atomic_item_make(
        item_heap,
        &(sxml_atomic_item_request){
            .type = sxml_builtin_type_identifier(type),
            .datum = {.datum_kind = SXML_DATUM_KIND_DATE_TIME,
                      .datum_details = {.date_time = typed.date_time}}},
        item);
}

/* -------------------------------------------- Duration plus duration */

/* Adds two durations of one kind; FODT0002 on overflow. */
typedef struct sxml_duration_sum {
    sxml_temporal_operand_kind kind;
    sxml_duration first;
    sxml_duration second;
    sxml_error_record *error_record;
} sxml_duration_sum;

static sxml_status sxml_durations_add(sxml_item_heap *item_heap,
                                      const sxml_duration_sum *sum,
                                      sxml_item *result) {
    sxml_duration total = {.months = 0, .seconds = 0, .nanoseconds = 0};
    int64_t nanoseconds =
        (int64_t)sum->first.nanoseconds + (int64_t)sum->second.nanoseconds;
    int64_t carry = nanoseconds / SXML_NANOSECONDS_PER_SECOND;
    nanoseconds -= carry * SXML_NANOSECONDS_PER_SECOND;
    int64_t seconds = 0;
    bool fits =
        sxml_checked_add((sxml_integer_pair){.first = sum->first.months,
                                             .second = sum->second.months},
                         &total.months) &&
        sxml_checked_add((sxml_integer_pair){.first = sum->first.seconds,
                                             .second = sum->second.seconds},
                         &seconds) &&
        sxml_checked_add((sxml_integer_pair){.first = seconds, .second = carry},
                         &seconds);
    if (!fits) {
        return sxml_error_record_report(sum->error_record,
                                        &sxml_temporal_duration_overflow_error);
    }
    /* Seconds and nanoseconds take one sign (D36). */
    if (seconds > 0 && nanoseconds < 0) {
        seconds -= 1;
        nanoseconds += SXML_NANOSECONDS_PER_SECOND;
    } else if (seconds < 0 && nanoseconds > 0) {
        seconds += 1;
        nanoseconds -= SXML_NANOSECONDS_PER_SECOND;
    }
    total.seconds = seconds;
    total.nanoseconds = (int32_t)nanoseconds;
    return sxml_typed_duration_make(
        item_heap,
        (sxml_typed_duration){.type = sxml_duration_type_of(sum->kind),
                              .duration = total},
        result);
}

/* -duration; false on overflow. */
static bool sxml_duration_negate(sxml_duration duration,
                                 sxml_duration *negation) {
    if (duration.months == INT64_MIN || duration.seconds == INT64_MIN) {
        return false;
    }
    *negation = (sxml_duration){.months = -duration.months,
                                .seconds = -duration.seconds,
                                .nanoseconds = -duration.nanoseconds};
    return true;
}

/* -------------------------------------------- Duration times number */

/* A duration multiplied or divided by a number (F&O 4.0, 8.5.1, 8.5.2). */
typedef struct sxml_duration_scaling {
    sxml_temporal_operand_kind kind;
    sxml_duration duration;
    sxml_item number;
    bool is_division;
    sxml_error_record *error_record;
} sxml_duration_scaling;

/* The xs:decimal of a number: the shortest decimal of a double or float,
   as casting gives (D57). */
static sxml_status
sxml_scaling_factor_make(const sxml_decimal_configuration *configuration,
                         sxml_item number, sxml_decimal *factor) {
    switch (sxml_item_get_representation(number)) {
    case SXML_ITEM_REPRESENTATION_INTEGER:
        return sxml_decimal_from_integer(configuration, number.payload.integer,
                                         factor);
    case SXML_ITEM_REPRESENTATION_FLOAT:
        return sxml_decimal_from_float_shortest(
            configuration, number.payload.float_number, factor);
    case SXML_ITEM_REPRESENTATION_DOUBLE:
        return sxml_decimal_from_double_shortest(
            configuration, number.payload.double_number, factor);
    case SXML_ITEM_REPRESENTATION_ATOMIC_CELL:
        return sxml_decimal_copy(configuration,
                                 sxml_atomic_item_get_decimal(number), factor);
    case SXML_ITEM_REPRESENTATION_NONE:
    case SXML_ITEM_REPRESENTATION_BOOLEAN:
    case SXML_ITEM_REPRESENTATION_QNAME:
    case SXML_ITEM_REPRESENTATION_NODE:
    case SXML_ITEM_REPRESENTATION_JSON_NODE_CELL:
    case SXML_ITEM_REPRESENTATION_FUNCTION_CELL:
    case SXML_ITEM_REPRESENTATION_MAP_CELL:
    case SXML_ITEM_REPRESENTATION_ARRAY_CELL:
    case SXML_ITEM_REPRESENTATION_VECTOR_CELL:
    case SXML_ITEM_REPRESENTATION_INTEGER_RANGE_CELL:
    case SXML_ITEM_REPRESENTATION_CONCATENATION_CELL:
    case SXML_ITEM_REPRESENTATION_LAZY_CELL:
        break;
    }
    return SXML_STATUS_INVALID_ARGUMENT;
}

/* The double value of a float or double number, else 1 (finite). */
static double sxml_binary_number_value(sxml_item number) {
    if (number.representation == SXML_ITEM_REPRESENTATION_DOUBLE) {
        return number.payload.double_number;
    }
    if (number.representation == SXML_ITEM_REPRESENTATION_FLOAT) {
        return (double)number.payload.float_number;
    }
    return 1.0;
}

/* One component (months, or nanoseconds in all) scaled and rounded. */
typedef struct sxml_component_scaling {
    const sxml_decimal *component;
    const sxml_decimal *factor;
    bool is_division;
    sxml_rounding_mode rounding_mode;
} sxml_component_scaling;

static sxml_status
sxml_component_scale(const sxml_decimal_configuration *configuration,
                     const sxml_component_scaling *scaling,
                     sxml_decimal *rounded) {
    sxml_decimal scaled;
    sxml_decimal_operands operands = {.left_operand = scaling->component,
                                      .right_operand = scaling->factor};
    sxml_status status =
        scaling->is_division
            ? sxml_decimal_divide(configuration, &operands, &scaled)
            : sxml_decimal_multiply(configuration, &operands, &scaled);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    status = sxml_decimal_round(
        configuration,
        &(sxml_decimal_rounding){.decimal = &scaled,
                                 .precision = 0,
                                 .rounding_mode = scaling->rounding_mode},
        rounded);
    sxml_decimal_finalize(&scaled);
    return status;
}

/* 10^9 as a decimal, for nanoseconds. */
static sxml_status
sxml_billion_make(const sxml_decimal_configuration *configuration,
                  sxml_decimal *billion) {
    return sxml_decimal_from_integer(configuration, SXML_NANOSECONDS_PER_SECOND,
                                     billion);
}

/* The total nanoseconds of a duration's seconds part, as a decimal. */
static sxml_status
sxml_duration_nanoseconds_make(const sxml_decimal_configuration *configuration,
                               sxml_duration duration,
                               sxml_decimal *nanoseconds) {
    sxml_decimal seconds;
    sxml_decimal billion;
    sxml_decimal seconds_in_nanoseconds;
    sxml_decimal fraction;
    sxml_status status =
        sxml_decimal_from_integer(configuration, duration.seconds, &seconds);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    status = sxml_billion_make(configuration, &billion);
    if (status == SXML_STATUS_OK) {
        status = sxml_decimal_multiply(
            configuration,
            &(sxml_decimal_operands){.left_operand = &seconds,
                                     .right_operand = &billion},
            &seconds_in_nanoseconds);
        sxml_decimal_finalize(&billion);
    }
    sxml_decimal_finalize(&seconds);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    status = sxml_decimal_from_integer(configuration, duration.nanoseconds,
                                       &fraction);
    if (status == SXML_STATUS_OK) {
        status = sxml_decimal_add(
            configuration,
            &(sxml_decimal_operands){.left_operand = &seconds_in_nanoseconds,
                                     .right_operand = &fraction},
            nanoseconds);
        sxml_decimal_finalize(&fraction);
    }
    sxml_decimal_finalize(&seconds_in_nanoseconds);
    return status;
}

/*
 * Splits whole nanoseconds into seconds and nanoseconds (same sign).
 * Returns SXML_STATUS_ARITHMETIC_OVERFLOW when the seconds do not fit.
 */
static sxml_status
sxml_duration_seconds_split(const sxml_decimal_configuration *configuration,
                            const sxml_decimal *nanoseconds,
                            sxml_duration *duration) {
    sxml_decimal billion;
    sxml_decimal seconds;
    sxml_decimal remainder;
    sxml_status status = sxml_billion_make(configuration, &billion);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_decimal_operands operands = {.left_operand = nanoseconds,
                                      .right_operand = &billion};
    status = sxml_decimal_integer_divide(configuration, &operands, &seconds);
    if (status == SXML_STATUS_OK) {
        status = sxml_decimal_modulo(configuration, &operands, &remainder);
        if (status == SXML_STATUS_OK) {
            int64_t whole_seconds = 0;
            int64_t fraction = 0;
            status = sxml_decimal_to_integer(&seconds, &whole_seconds);
            if (status == SXML_STATUS_OK) {
                status = sxml_decimal_to_integer(&remainder, &fraction);
            }
            if (status == SXML_STATUS_OK) {
                duration->seconds = whole_seconds;
                duration->nanoseconds = (int32_t)fraction;
            }
            sxml_decimal_finalize(&remainder);
        }
        sxml_decimal_finalize(&seconds);
    }
    sxml_decimal_finalize(&billion);
    return status;
}

/* Maps a decimal failure in duration scaling to its error. */
static sxml_status sxml_scaling_failure(sxml_status status,
                                        sxml_error_record *error_record) {
    if (status == SXML_STATUS_ARITHMETIC_OVERFLOW) {
        return sxml_error_record_report(error_record,
                                        &sxml_temporal_duration_overflow_error);
    }
    if (status == SXML_STATUS_LIMIT_EXCEEDED) {
        return sxml_error_record_report(error_record,
                                        &sxml_temporal_digit_limit_error);
    }
    return status;
}

/* op:multiply-duration-by-number and op:divide-duration-by-number. */
static sxml_status sxml_duration_scale(sxml_item_heap *item_heap,
                                       const sxml_duration_scaling *scaling,
                                       sxml_item *result) {
    double binary_value = sxml_binary_number_value(scaling->number);
    if (isnan(binary_value)) {
        return sxml_error_record_report(scaling->error_record,
                                        &sxml_temporal_not_a_number_error);
    }
    sxml_builtin_type result_type = sxml_duration_type_of(scaling->kind);
    if (isinf(binary_value)) {
        /* An infinite factor overflows; an infinite divisor gives a
           zero-length duration (F&O 4.0, 8.1.2: underflow). */
        if (!scaling->is_division) {
            return sxml_error_record_report(
                scaling->error_record, &sxml_temporal_duration_overflow_error);
        }
        return sxml_typed_duration_make(
            item_heap,
            (sxml_typed_duration){
                .type = result_type,
                .duration = {.months = 0, .seconds = 0, .nanoseconds = 0}},
            result);
    }
    const sxml_decimal_configuration *configuration =
        sxml_item_heap_get_decimal_configuration(item_heap);
    sxml_decimal factor;
    sxml_status status =
        sxml_scaling_factor_make(configuration, scaling->number, &factor);
    if (status != SXML_STATUS_OK) {
        return sxml_scaling_failure(status, scaling->error_record);
    }
    if (scaling->is_division && sxml_decimal_is_zero(&factor)) {
        sxml_decimal_finalize(&factor);
        return sxml_error_record_report(scaling->error_record,
                                        &sxml_temporal_duration_overflow_error);
    }
    sxml_duration scaled = {.months = 0, .seconds = 0, .nanoseconds = 0};
    sxml_decimal months;
    sxml_decimal rounded_months;
    status = sxml_decimal_from_integer(configuration, scaling->duration.months,
                                       &months);
    if (status == SXML_STATUS_OK) {
        /* Months round as fn:round#1 does: a tie toward positive
           infinity. */
        status = sxml_component_scale(
            configuration,
            &(sxml_component_scaling){.component = &months,
                                      .factor = &factor,
                                      .is_division = scaling->is_division,
                                      .rounding_mode =
                                          SXML_ROUNDING_MODE_HALF_TO_CEILING},
            &rounded_months);
        sxml_decimal_finalize(&months);
        if (status == SXML_STATUS_OK) {
            status = sxml_decimal_to_integer(&rounded_months, &scaled.months);
            sxml_decimal_finalize(&rounded_months);
        }
    }
    sxml_decimal nanoseconds;
    sxml_decimal rounded_nanoseconds;
    if (status == SXML_STATUS_OK) {
        status = sxml_duration_nanoseconds_make(
            configuration, scaling->duration, &nanoseconds);
    }
    if (status == SXML_STATUS_OK) {
        /* Seconds to the nanosecond, the closest value; a tie to even
           (implementation-defined, F&O 4.0, section 8.1.2). */
        status = sxml_component_scale(
            configuration,
            &(sxml_component_scaling){.component = &nanoseconds,
                                      .factor = &factor,
                                      .is_division = scaling->is_division,
                                      .rounding_mode =
                                          SXML_ROUNDING_MODE_HALF_TO_EVEN},
            &rounded_nanoseconds);
        sxml_decimal_finalize(&nanoseconds);
        if (status == SXML_STATUS_OK) {
            status = sxml_duration_seconds_split(configuration,
                                                 &rounded_nanoseconds, &scaled);
            sxml_decimal_finalize(&rounded_nanoseconds);
        }
    }
    sxml_decimal_finalize(&factor);
    if (status != SXML_STATUS_OK) {
        return sxml_scaling_failure(status, scaling->error_record);
    }
    return sxml_typed_duration_make(
        item_heap,
        (sxml_typed_duration){.type = result_type, .duration = scaled}, result);
}

/* ----------------------------------------------- Duration by duration */

/* op:divide-yearMonthDuration-by-yearMonthDuration and
   op:divide-dayTimeDuration-by-dayTimeDuration: an xs:decimal. */
typedef struct sxml_duration_ratio {
    sxml_temporal_operand_kind kind;
    sxml_duration dividend;
    sxml_duration divisor;
    sxml_error_record *error_record;
} sxml_duration_ratio;

/* A duration whose length is wanted in months or in nanoseconds. */
typedef struct sxml_duration_length {
    sxml_temporal_operand_kind kind;
    sxml_duration duration;
} sxml_duration_length;

static sxml_status
sxml_duration_length_make(const sxml_decimal_configuration *configuration,
                          sxml_duration_length duration_length,
                          sxml_decimal *length) {
    if (duration_length.kind ==
        SXML_TEMPORAL_OPERAND_KIND_YEAR_MONTH_DURATION) {
        return sxml_decimal_from_integer(
            configuration, duration_length.duration.months, length);
    }
    return sxml_duration_nanoseconds_make(configuration,
                                          duration_length.duration, length);
}

static sxml_status sxml_durations_divide(sxml_item_heap *item_heap,
                                         const sxml_duration_ratio *ratio,
                                         sxml_item *result) {
    const sxml_decimal_configuration *configuration =
        sxml_item_heap_get_decimal_configuration(item_heap);
    sxml_decimal dividend;
    sxml_decimal divisor;
    sxml_decimal quotient;
    sxml_status status = sxml_duration_length_make(
        configuration,
        (sxml_duration_length){.kind = ratio->kind,
                               .duration = ratio->dividend},
        &dividend);
    if (status != SXML_STATUS_OK) {
        return sxml_scaling_failure(status, ratio->error_record);
    }
    status = sxml_duration_length_make(
        configuration,
        (sxml_duration_length){.kind = ratio->kind, .duration = ratio->divisor},
        &divisor);
    if (status != SXML_STATUS_OK) {
        sxml_decimal_finalize(&dividend);
        return sxml_scaling_failure(status, ratio->error_record);
    }
    if (sxml_decimal_is_zero(&divisor)) {
        status = sxml_error_record_report(ratio->error_record,
                                          &sxml_temporal_ratio_by_zero_error);
    } else {
        status = sxml_decimal_divide(
            configuration,
            &(sxml_decimal_operands){.left_operand = &dividend,
                                     .right_operand = &divisor},
            &quotient);
        if (status != SXML_STATUS_OK) {
            status = sxml_scaling_failure(status, ratio->error_record);
        }
    }
    sxml_decimal_finalize(&dividend);
    sxml_decimal_finalize(&divisor);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_decimal_item_request request = {
        .type = sxml_builtin_type_identifier(SXML_TYPE_XS_DECIMAL),
        .decimal = &quotient};
    status = sxml_atomic_item_make_taking_decimal(item_heap, &request, result);
    if (status != SXML_STATUS_OK) {
        sxml_decimal_finalize(&quotient);
    }
    return status;
}

/* --------------------------------------------- Dates plus durations */

/* A duration added to a date, time or dateTime (F&O 4.0, 9.8.4 to
   9.8.13); a subtraction adds the negated duration. */
typedef struct sxml_date_time_shift {
    sxml_temporal_operand_kind point_kind;
    sxml_date_time date_time;
    sxml_temporal_operand_kind duration_kind;
    sxml_duration duration;
    sxml_error_record *error_record;
} sxml_date_time_shift;

/* Adds months: the day is kept, or the last day of a shorter month
   (XSD 1.1 Part 2, Appendix E). False on overflow. */
static bool sxml_date_time_add_months(sxml_date_time *date_time,
                                      int64_t months) {
    int64_t year_months = 0;
    int64_t total_months = 0;
    if (!sxml_checked_multiply(
            (sxml_integer_pair){.first = date_time->year, .second = 12},
            &year_months) ||
        !sxml_checked_add(
            (sxml_integer_pair){.first = year_months,
                                .second = (int64_t)date_time->month - 1},
            &total_months) ||
        !sxml_checked_add(
            (sxml_integer_pair){.first = total_months, .second = months},
            &total_months)) {
        return false;
    }
    date_time->year = sxml_floor_divide(total_months, 12);
    date_time->month = (uint8_t)(sxml_floor_modulo(total_months, 12) + 1);
    uint8_t last_day = sxml_days_in_month(date_time->year, date_time->month);
    if (date_time->day > last_day) {
        date_time->day = last_day;
    }
    return true;
}

static sxml_status sxml_date_time_shift_make(sxml_item_heap *item_heap,
                                             const sxml_date_time_shift *shift,
                                             sxml_item *result) {
    sxml_date_time date_time = shift->date_time;
    if (shift->duration_kind ==
        SXML_TEMPORAL_OPERAND_KIND_YEAR_MONTH_DURATION) {
        if (!sxml_date_time_add_months(&date_time, shift->duration.months)) {
            return sxml_error_record_report(shift->error_record,
                                            &sxml_temporal_date_overflow_error);
        }
        return sxml_date_time_item_make(
            item_heap,
            (sxml_typed_date_time){.kind = shift->point_kind,
                                   .date_time = date_time},
            result);
    }
    if (date_time.year == INT64_MIN) {
        return sxml_error_record_report(shift->error_record,
                                        &sxml_temporal_date_overflow_error);
    }
    sxml_moment moment = sxml_local_moment_of(shift->point_kind, &date_time);
    sxml_date_time shifted = date_time;
    if (!sxml_moment_shift(
            &moment,
            (sxml_time_shift){.seconds = shift->duration.seconds,
                              .nanoseconds = shift->duration.nanoseconds}) ||
        (shift->point_kind != SXML_TEMPORAL_OPERAND_KIND_TIME &&
         !sxml_civil_date_of(moment.day, &shifted))) {
        return sxml_error_record_report(shift->error_record,
                                        &sxml_temporal_date_overflow_error);
    }
    if (shift->point_kind != SXML_TEMPORAL_OPERAND_KIND_DATE) {
        /* A time wraps around midnight and keeps no date (9.8.12). */
        shifted.hour = (uint8_t)(moment.second_of_day / 3600);
        shifted.minute = (uint8_t)(moment.second_of_day / 60 % 60);
        shifted.second = (uint8_t)(moment.second_of_day % 60);
        shifted.nanosecond = (uint32_t)moment.nanosecond;
    }
    if (shift->point_kind == SXML_TEMPORAL_OPERAND_KIND_TIME) {
        shifted.year = 0;
        shifted.month = 0;
        shifted.day = 0;
    }
    return sxml_date_time_item_make(
        item_heap,
        (sxml_typed_date_time){.kind = shift->point_kind, .date_time = shifted},
        result);
}

/* ------------------------------------------------ Dates minus dates */

/* Two dates, times or dateTimes of one kind to subtract (9.8.1 to
   9.8.3). */
typedef struct sxml_date_time_difference {
    sxml_temporal_operand_kind kind;
    sxml_date_time first;
    sxml_date_time second;
    int16_t implicit_timezone_offset_minutes;
    sxml_error_record *error_record;
} sxml_date_time_difference;

/* The moment in UTC: the value's timezone, else the implicit one. */
static sxml_moment
sxml_utc_moment_of(const sxml_date_time_difference *difference,
                   const sxml_date_time *date_time) {
    sxml_moment moment = sxml_local_moment_of(difference->kind, date_time);
    int64_t offset_minutes = date_time->has_timezone
                                 ? date_time->timezone_offset_minutes
                                 : difference->implicit_timezone_offset_minutes;
    /* Local time minus the offset is UTC; one day at most, never an
       overflow for a year above INT64_MIN + 1. */
    (void)sxml_moment_shift(
        &moment,
        (sxml_time_shift){.seconds = -offset_minutes * 60, .nanoseconds = 0});
    return moment;
}

static sxml_status
sxml_date_times_subtract(sxml_item_heap *item_heap,
                         const sxml_date_time_difference *difference,
                         sxml_item *result) {
    if (difference->first.year <= INT64_MIN + 1 ||
        difference->second.year <= INT64_MIN + 1) {
        return sxml_error_record_report(difference->error_record,
                                        &sxml_temporal_duration_overflow_error);
    }
    sxml_moment first = sxml_utc_moment_of(difference, &difference->first);
    sxml_moment second = sxml_utc_moment_of(difference, &difference->second);
    int64_t day_count = 0;
    int64_t day_seconds = 0;
    int64_t seconds = 0;
    if (!sxml_cycle_days_between(first.day, second.day, &day_count) ||
        !sxml_checked_multiply(
            (sxml_integer_pair){.first = day_count,
                                .second = SXML_SECONDS_PER_DAY},
            &day_seconds) ||
        !sxml_checked_add((sxml_integer_pair){.first = day_seconds,
                                              .second = first.second_of_day -
                                                        second.second_of_day},
                          &seconds)) {
        return sxml_error_record_report(difference->error_record,
                                        &sxml_temporal_duration_overflow_error);
    }
    int64_t nanoseconds = first.nanosecond - second.nanosecond;
    if (seconds > 0 && nanoseconds < 0) {
        seconds -= 1;
        nanoseconds += SXML_NANOSECONDS_PER_SECOND;
    } else if (seconds < 0 && nanoseconds > 0) {
        seconds += 1;
        nanoseconds -= SXML_NANOSECONDS_PER_SECOND;
    }
    return sxml_typed_duration_make(
        item_heap,
        (sxml_typed_duration){
            .type = SXML_TYPE_XS_DAY_TIME_DURATION,
            .duration = {.months = 0,
                         .seconds = seconds,
                         .nanoseconds = (int32_t)nanoseconds}},
        result);
}

/* ------------------------------------------------------------ Dispatch */

/* The operands of one dispatch, read once. */
typedef struct sxml_temporal_operands {
    const sxml_temporal_arithmetic *arithmetic;
    sxml_temporal_operand_kind left_kind;
    sxml_temporal_operand_kind right_kind;
} sxml_temporal_operands;

/* point ± duration, either order for +: an addition of a duration to a
   date, time or dateTime the operator table has. */
static bool sxml_shift_is_defined(sxml_temporal_operand_kind point_kind,
                                  sxml_temporal_operand_kind duration_kind) {
    if (duration_kind == SXML_TEMPORAL_OPERAND_KIND_DAY_TIME_DURATION) {
        return sxml_temporal_operand_kind_is_point(point_kind);
    }
    return duration_kind == SXML_TEMPORAL_OPERAND_KIND_YEAR_MONTH_DURATION &&
           (point_kind == SXML_TEMPORAL_OPERAND_KIND_DATE_TIME ||
            point_kind == SXML_TEMPORAL_OPERAND_KIND_DATE);
}

static sxml_status sxml_temporal_shift(sxml_item_heap *item_heap,
                                       const sxml_temporal_operands *operands,
                                       sxml_item *result) {
    const sxml_temporal_arithmetic *arithmetic = operands->arithmetic;
    bool point_is_left =
        sxml_temporal_operand_kind_is_point(operands->left_kind);
    sxml_item point =
        point_is_left ? arithmetic->left_item : arithmetic->right_item;
    sxml_item duration_item =
        point_is_left ? arithmetic->right_item : arithmetic->left_item;
    sxml_duration duration = sxml_duration_of(duration_item);
    if (arithmetic->arithmetic_operator == SXML_ARITHMETIC_OPERATOR_SUBTRACT &&
        !sxml_duration_negate(duration, &duration)) {
        return sxml_error_record_report(arithmetic->error_record,
                                        &sxml_temporal_date_overflow_error);
    }
    return sxml_date_time_shift_make(
        item_heap,
        &(sxml_date_time_shift){
            .point_kind =
                point_is_left ? operands->left_kind : operands->right_kind,
            .date_time = sxml_date_time_of(point),
            .duration_kind =
                point_is_left ? operands->right_kind : operands->left_kind,
            .duration = duration,
            .error_record = arithmetic->error_record},
        result);
}

/* + and - of two durations of one kind. */
static sxml_status
sxml_temporal_durations_add(sxml_item_heap *item_heap,
                            const sxml_temporal_operands *operands,
                            sxml_item *result) {
    const sxml_temporal_arithmetic *arithmetic = operands->arithmetic;
    sxml_duration second = sxml_duration_of(arithmetic->right_item);
    if (arithmetic->arithmetic_operator == SXML_ARITHMETIC_OPERATOR_SUBTRACT &&
        !sxml_duration_negate(second, &second)) {
        return sxml_error_record_report(arithmetic->error_record,
                                        &sxml_temporal_duration_overflow_error);
    }
    return sxml_durations_add(
        item_heap,
        &(sxml_duration_sum){.kind = operands->left_kind,
                             .first = sxml_duration_of(arithmetic->left_item),
                             .second = second,
                             .error_record = arithmetic->error_record},
        result);
}

/* The operator table of XPath 4.0, section 4.9, for + and -. */
static sxml_status
sxml_temporal_add_or_subtract(sxml_item_heap *item_heap,
                              const sxml_temporal_operands *operands,
                              sxml_item *result) {
    const sxml_temporal_arithmetic *arithmetic = operands->arithmetic;
    sxml_temporal_operand_kind left_kind = operands->left_kind;
    sxml_temporal_operand_kind right_kind = operands->right_kind;
    bool is_addition =
        arithmetic->arithmetic_operator == SXML_ARITHMETIC_OPERATOR_ADD;
    if (left_kind == right_kind &&
        (left_kind == SXML_TEMPORAL_OPERAND_KIND_YEAR_MONTH_DURATION ||
         left_kind == SXML_TEMPORAL_OPERAND_KIND_DAY_TIME_DURATION)) {
        return sxml_temporal_durations_add(item_heap, operands, result);
    }
    if (sxml_shift_is_defined(left_kind, right_kind) ||
        (is_addition && sxml_shift_is_defined(right_kind, left_kind))) {
        return sxml_temporal_shift(item_heap, operands, result);
    }
    if (!is_addition && left_kind == right_kind &&
        sxml_temporal_operand_kind_is_point(left_kind)) {
        return sxml_date_times_subtract(
            item_heap,
            &(sxml_date_time_difference){
                .kind = left_kind,
                .first = sxml_date_time_of(arithmetic->left_item),
                .second = sxml_date_time_of(arithmetic->right_item),
                .implicit_timezone_offset_minutes =
                    arithmetic->implicit_timezone_offset_minutes,
                .error_record = arithmetic->error_record},
            result);
    }
    return sxml_error_record_report(arithmetic->error_record,
                                    &sxml_temporal_operand_type_error);
}

/* The operator table of XPath 4.0, section 4.9, for * and div. */
static sxml_status
sxml_temporal_multiply_or_divide(sxml_item_heap *item_heap,
                                 const sxml_temporal_operands *operands,
                                 sxml_item *result) {
    const sxml_temporal_arithmetic *arithmetic = operands->arithmetic;
    sxml_temporal_operand_kind left_kind = operands->left_kind;
    sxml_temporal_operand_kind right_kind = operands->right_kind;
    bool is_division =
        arithmetic->arithmetic_operator == SXML_ARITHMETIC_OPERATOR_DIVIDE;
    bool left_is_duration = sxml_temporal_operand_kind_is_duration(left_kind);
    if (left_is_duration && right_kind == SXML_TEMPORAL_OPERAND_KIND_NUMBER) {
        return sxml_duration_scale(
            item_heap,
            &(sxml_duration_scaling){
                .kind = left_kind,
                .duration = sxml_duration_of(arithmetic->left_item),
                .number = arithmetic->right_item,
                .is_division = is_division,
                .error_record = arithmetic->error_record},
            result);
    }
    if (!is_division && left_kind == SXML_TEMPORAL_OPERAND_KIND_NUMBER &&
        sxml_temporal_operand_kind_is_duration(right_kind)) {
        return sxml_duration_scale(
            item_heap,
            &(sxml_duration_scaling){
                .kind = right_kind,
                .duration = sxml_duration_of(arithmetic->right_item),
                .number = arithmetic->left_item,
                .is_division = false,
                .error_record = arithmetic->error_record},
            result);
    }
    if (is_division && left_kind == right_kind &&
        (left_kind == SXML_TEMPORAL_OPERAND_KIND_YEAR_MONTH_DURATION ||
         left_kind == SXML_TEMPORAL_OPERAND_KIND_DAY_TIME_DURATION)) {
        return sxml_durations_divide(
            item_heap,
            &(sxml_duration_ratio){
                .kind = left_kind,
                .dividend = sxml_duration_of(arithmetic->left_item),
                .divisor = sxml_duration_of(arithmetic->right_item),
                .error_record = arithmetic->error_record},
            result);
    }
    return sxml_error_record_report(arithmetic->error_record,
                                    &sxml_temporal_operand_type_error);
}

sxml_status
sxml_operator_make_temporal_value(sxml_item_heap *item_heap,
                                  const sxml_temporal_arithmetic *arithmetic,
                                  sxml_item *result) {
    sxml_temporal_operands operands = {
        .arithmetic = arithmetic,
        .left_kind =
            sxml_temporal_operand_kind_of(item_heap, arithmetic->left_item),
        .right_kind =
            sxml_temporal_operand_kind_of(item_heap, arithmetic->right_item),
    };
    switch (arithmetic->arithmetic_operator) {
    case SXML_ARITHMETIC_OPERATOR_ADD:
    case SXML_ARITHMETIC_OPERATOR_SUBTRACT:
        return sxml_temporal_add_or_subtract(item_heap, &operands, result);
    case SXML_ARITHMETIC_OPERATOR_MULTIPLY:
    case SXML_ARITHMETIC_OPERATOR_DIVIDE:
        return sxml_temporal_multiply_or_divide(item_heap, &operands, result);
    case SXML_ARITHMETIC_OPERATOR_INTEGER_DIVIDE:
    case SXML_ARITHMETIC_OPERATOR_MODULO:
        break;
    }
    return sxml_error_record_report(arithmetic->error_record,
                                    &sxml_temporal_operand_type_error);
}
