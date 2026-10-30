IMPORT "@std/time/datetime.bas"

ACTION main()
    epoch_result = datetime.datetime_from_unix_milliseconds(0)
    SELECT epoch_result
    CASE Ok(epoch)
        print($"epoch={epoch.year}-{epoch.month}-{epoch.day}")
    CASE Error(problem)
        print(problem)
    END SELECT

    before_epoch_result = datetime.datetime_from_unix_milliseconds(-1)
    SELECT before_epoch_result
    CASE Ok(before_epoch)
        print($"before={before_epoch.year}-{before_epoch.month}-{before_epoch.day} {before_epoch.hour}:{before_epoch.minute}:{before_epoch.second}.{before_epoch.millisecond}")
        roundtrip = datetime.datetime_to_unix_milliseconds(before_epoch)
        SELECT roundtrip
        CASE Ok(timestamp)
            print($"roundtrip={timestamp}")
        CASE Error(problem)
            print(problem)
        END SELECT
    CASE Error(problem)
        print(problem)
    END SELECT

    leap_day_result = datetime.datetime_from_unix_milliseconds(951782400000)
    SELECT leap_day_result
    CASE Ok(leap_day)
        print($"leap={leap_day.year}-{leap_day.month}-{leap_day.day}")
    CASE Error(problem)
        print(problem)
    END SELECT

    invalid_date_result = datetime.datetime_to_unix_milliseconds(datetime.Date_Time(1900, 2, 29, 0, 0, 0, 0))
    SELECT invalid_date_result
    CASE Ok(timestamp)
        print($"invalid={timestamp}")
    CASE Error(problem)
        print($"invalid={problem}")
    END SELECT

    minimum_result = datetime.datetime_from_unix_milliseconds(-62135596800000)
    SELECT minimum_result
    CASE Ok(minimum)
        print($"minimum={minimum.year}-{minimum.month}-{minimum.day}")
        minimum_timestamp_result = datetime.datetime_to_unix_milliseconds(minimum)
        SELECT minimum_timestamp_result
        CASE Ok(timestamp)
            print($"minimum-roundtrip={timestamp}")
        CASE Error(problem)
            print(problem)
        END SELECT
    CASE Error(problem)
        print(problem)
    END SELECT

    maximum_result = datetime.datetime_from_unix_milliseconds(253402300799999)
    SELECT maximum_result
    CASE Ok(maximum)
        print($"maximum={maximum.year}-{maximum.month}-{maximum.day} {maximum.hour}:{maximum.minute}:{maximum.second}.{maximum.millisecond}")
        maximum_timestamp_result = datetime.datetime_to_unix_milliseconds(maximum)
        SELECT maximum_timestamp_result
        CASE Ok(timestamp)
            print($"maximum-roundtrip={timestamp}")
        CASE Error(problem)
            print(problem)
        END SELECT
    CASE Error(problem)
        print(problem)
    END SELECT
END ACTION
