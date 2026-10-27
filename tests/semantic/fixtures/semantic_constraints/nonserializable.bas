RECORD Callback_Box(items)
MESSAGE Callback_Message(box)

FUNCTION make_callback()
    RETURN 1
END FUNCTION

ACTOR Worker
    RECEIVE Callback_Message(box)
    END RECEIVE
END ACTOR

ACTION main()
    worker_ref = SPAWN Worker
    callback_data = Callback_Box([make_callback])
    SEND worker_ref, Callback_Message(callback_data)
END ACTION
