MESSAGE Remote_Ping(code_value, reason, values, meta)
MESSAGE Begin_Watch(target)

ACTOR Remote_Sink
    RECEIVE Remote_Ping(code_value, reason, values, meta)
        IF values[0] = 1 AND values[1] = 2 AND meta["count"] = 42 THEN
            STOP
        ELSE
            failure = TRY Error("distributed value mismatch")
        END IF
    END RECEIVE
END ACTOR

ACTOR Remote_Watcher
    RECEIVE Begin_Watch(target)
        VAR probe = SPAWN Remote_Sink
        IF target = probe THEN
            probe = target
        END IF
        WATCH target
    END RECEIVE
    RECEIVE Actor_Down(code_value, reason)
        IF reason = NORMAL OR reason = NODE_UNREACHABLE THEN
            STOP
        ELSE
            failure = TRY Error("unexpected remote actor down")
        END IF
    END RECEIVE
END ACTOR

ACTION prepare_remote_watcher()
    watcher = SPAWN Remote_Watcher
END ACTION

ACTION spawn_remote(target_node)
    sink = SPAWN Remote_Sink AT target_node
    SEND sink, Remote_Ping(7, NORMAL, [1, 2], {"count": 42})
END ACTION

ACTION spawn_remote_idle(target_node)
    sink = SPAWN Remote_Sink AT target_node
END ACTION

ACTION pump_remote()
END ACTION

ACTION main()
END ACTION
