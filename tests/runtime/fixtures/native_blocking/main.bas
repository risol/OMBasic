MESSAGE Start_Slow()
MESSAGE Start_Fast()

ACTOR Slow_Native_Call
    RECEIVE Start_Slow()
        IF native_wait_for_fast(3000) = 0 THEN
            failure = 1 / 0
        END IF
        STOP
    END RECEIVE
END ACTOR

ACTOR Fast_Native_Call
    RECEIVE Start_Fast()
        native_mark_fast()
        STOP
    END RECEIVE
END ACTOR

ACTION main()
    native_reset_fast()
    slow = SPAWN Slow_Native_Call
    fast = SPAWN Fast_Native_Call
    SEND slow, Start_Slow()
    SEND fast, Start_Fast()
END ACTION
