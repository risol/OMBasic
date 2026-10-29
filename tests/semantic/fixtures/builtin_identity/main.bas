FUNCTION result_value(valid)
    IF valid THEN
        RETURN Ok(1)
    ELSE
        RETURN Error("failed")
    END IF
END FUNCTION

ACTION main()
    normal_reason = Actor_Down_Reason.normal
    failed_reason = Actor_Down_Reason.failed
    unknown_reason = Actor_Down_Reason.node_unreachable
    ok_value = result_value(TRUE)
    error_value = result_value(FALSE)
    down_message = Actor_Down(1, Actor_Down_Reason.failed)
END ACTION
