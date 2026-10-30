ACTOR Append_Client
    RECEIVE File_Write_Completed(outcome)
        write_result = TRY outcome
        SEND File_System, File_Append("append.txt", " tail", SELF)
    END RECEIVE

    RECEIVE File_Append_Completed(outcome)
        append_result = TRY outcome
        SEND File_System, File_Read("append.txt", SELF)
    END RECEIVE

    RECEIVE File_Read_Completed(outcome)
        contents = TRY outcome
        print(contents)
        STOP
    END RECEIVE
END ACTOR

ACTION main()
    client_ref = SPAWN Append_Client
    SEND File_System, File_Write("append.txt", "start", client_ref)
END ACTION
