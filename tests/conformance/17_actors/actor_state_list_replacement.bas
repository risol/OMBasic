MESSAGE Begin_List_Update(reply_id)
MESSAGE List_Update_Done(values)

ACTOR List_Update_Actor
    VAR values = [1, 2]

    RECEIVE Begin_List_Update(reply_id)
        updated = list_set(values, 0, 99)
        values = updated
        SEND reply_id, List_Update_Done(updated)
    END RECEIVE
END ACTOR

ACTOR List_Update_Receiver
    RECEIVE List_Update_Done(values)
        print($"updated={values[0]},{values[1]}")
        STOP
    END RECEIVE
END ACTOR

ACTION main()
    receiver = SPAWN List_Update_Receiver
    updater = SPAWN List_Update_Actor
    SEND updater, Begin_List_Update(Actor_Id(receiver))
END ACTION
