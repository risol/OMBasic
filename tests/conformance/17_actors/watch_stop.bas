MESSAGE Begin_Watching(watched_ref)
MESSAGE Stop_Target()

ACTOR Target
    RECEIVE Stop_Target()
        STOP
    END RECEIVE
END ACTOR

ACTOR Observer
    RECEIVE Begin_Watching(watched_ref)
        WATCH watched_ref
        SEND watched_ref, Stop_Target()
    END RECEIVE

    RECEIVE Actor_Down(down_id, why)
        print("target-down")
        STOP
    END RECEIVE
END ACTOR

ACTION main()
    observer_ref = SPAWN Observer
    target_ref = SPAWN Target
    SEND observer_ref, Begin_Watching(target_ref)
END ACTION
