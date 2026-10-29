FUNCTION result_value(valid)
    IF valid THEN
        RETURN Ok(1)
    ELSE
        RETURN Error("bad")
    END IF
END FUNCTION

ACTION main()
    normal_reason = Actor_Down_Reason.normal
    failed_reason = Actor_Down_Reason.failed
    unreachable_reason = Actor_Down_Reason.node_unreachable
    success = result_value(TRUE)
    failure = result_value(FALSE)
END ACTION

ACTOR Watcher
    RECEIVE Actor_Down(sender, why)
        print("down")
    END RECEIVE
END ACTOR
