EXPORT RECORD Date_Time(year, month, day, hour, minute, second, millisecond)

FUNCTION is_leap_year(year)
    RETURN year % 4 = 0 AND (year % 100 <> 0 OR year % 400 = 0)
END FUNCTION

FUNCTION days_in_year(year)
    IF is_leap_year(year) THEN
        RETURN 366
    END IF
    RETURN 365
END FUNCTION

FUNCTION days_in_month(year, month)
    IF month = 2 THEN
        IF is_leap_year(year) THEN
            RETURN 29
        END IF
        RETURN 28
    END IF
    IF month = 4 OR month = 6 OR month = 9 OR month = 11 THEN
        RETURN 30
    END IF
    RETURN 31
END FUNCTION

FUNCTION floor_divide(value, divisor)
    VAR quotient = value / divisor
    VAR remainder = value % divisor
    IF remainder < 0 THEN
        RETURN quotient - 1
    END IF
    RETURN quotient
END FUNCTION

EXPORT FUNCTION datetime_from_unix_milliseconds(timestamp)
    IF timestamp < -62135596800000 OR timestamp > 253402300799999 THEN
        RETURN Error("out_of_range")
    END IF

    VAR days = timestamp / 86400000
    VAR within_day = timestamp % 86400000
    IF within_day < 0 THEN
        days = days - 1
        within_day = within_day + 86400000
    END IF

    VAR civil_days = days + 719468
    VAR era = civil_days / 146097
    VAR day_of_era = civil_days - era * 146097
    VAR year_of_era = (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365
    VAR year = year_of_era + era * 400
    VAR day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100)
    VAR month_prime = (5 * day_of_year + 2) / 153
    VAR day = day_of_year - (153 * month_prime + 2) / 5 + 1
    VAR month = month_prime
    IF month_prime < 10 THEN
        month = month_prime + 3
    ELSE
        month = month_prime - 9
    END IF
    IF month <= 2 THEN
        year = year + 1
    END IF
    VAR hour = within_day / 3600000
    within_day = within_day % 3600000
    VAR minute = within_day / 60000
    within_day = within_day % 60000
    VAR second = within_day / 1000
    millisecond = within_day % 1000
    RETURN Ok(Date_Time(year, month, day, hour, minute, second, millisecond))
END FUNCTION

EXPORT FUNCTION datetime_to_unix_milliseconds(value)
    IF value.year < 1 OR value.year > 9999 OR value.month < 1 OR value.month > 12 THEN
        RETURN Error("out_of_range")
    END IF
    IF value.day < 1 OR value.day > days_in_month(value.year, value.month) OR value.hour < 0 OR value.hour > 23 OR value.minute < 0 OR value.minute > 59 OR value.second < 0 OR value.second > 59 OR value.millisecond < 0 OR value.millisecond > 999 THEN
        RETURN Error("out_of_range")
    END IF

    VAR adjusted_year = value.year
    IF value.month <= 2 THEN
        adjusted_year = adjusted_year - 1
    END IF
    VAR era = adjusted_year / 400
    VAR year_of_era = adjusted_year - era * 400
    VAR month_prime = value.month
    IF value.month > 2 THEN
        month_prime = value.month - 3
    ELSE
        month_prime = value.month + 9
    END IF
    VAR day_of_year = (153 * month_prime + 2) / 5 + value.day - 1
    VAR day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year
    VAR days = era * 146097 + day_of_era - 719468
    VAR timestamp = days * 86400000 + value.hour * 3600000 + value.minute * 60000 + value.second * 1000 + value.millisecond
    RETURN Ok(timestamp)
END FUNCTION
