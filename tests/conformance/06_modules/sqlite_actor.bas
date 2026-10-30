IMPORT "@std/sqlite/sqlite.bas"

MESSAGE Start_Database()

ACTOR Database_Probe
    VAR database = Actor_Id()
    VAR created = FALSE
    VAR queried = FALSE

    RECEIVE Start_Database()
        database = Actor_Id(SPAWN sqlite.Database)
        SEND database, sqlite.Database_Open("database-test.db", Actor_Id(SELF))
    END RECEIVE

    RECEIVE sqlite.Database_Opened(connection_id, error_code, native_code)
        IF connection_id = 0 THEN
            print($"open-error={error_code}|{native_code}")
            STOP
        END IF
        SEND database, sqlite.Database_Execute("CREATE TABLE sample(id INTEGER PRIMARY KEY, name TEXT, payload BLOB)", SELF)
    END RECEIVE

    RECEIVE sqlite.Database_Execute_Completed(success, changes, last_row_id, error_code, native_code)
        IF NOT success THEN
            print($"execute-error={error_code}|{native_code}")
            STOP
        END IF
        IF NOT created THEN
            created = TRUE
            SEND database, sqlite.Database_Execute("INSERT INTO sample(name, payload) VALUES('Ada', X'00FF')", SELF)
        ELSE
            print($"insert={changes}|{last_row_id}")
            SEND database, sqlite.Database_Query("SELECT id, name, payload, 3.5 AS ratio, NULL AS absent FROM sample", SELF)
        END IF
    END RECEIVE

    RECEIVE sqlite.Database_Query_Completed(success, json_text, error_code, native_code)
        IF success AND NOT queried THEN
            queried = TRUE
            print($"query={json_text}")
            SEND database, sqlite.Database_Query("SELEC broken", SELF)
        ELSE IF success THEN
            print("syntax=unexpected-success")
            SEND database, sqlite.Database_Close(SELF)
        ELSE
            IF queried THEN
                print($"syntax={success}|{error_code}|{native_code}")
                SEND database, sqlite.Database_Close(SELF)
            ELSE
                print($"query-error={error_code}|{native_code}")
                STOP
            END IF
        END IF
    END RECEIVE

    RECEIVE sqlite.Database_Closed(success, error_code, native_code)
        print($"close={success}|{error_code}")
        STOP
    END RECEIVE
END ACTOR

ACTION main()
    probe = SPAWN Database_Probe
    SEND probe, Start_Database()
END ACTION
