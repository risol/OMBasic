ACTOR File_Client
    RECEIVE File_Write_Completed(outcome)
        write_result = TRY outcome
        SEND File_System, File_Read("roundtrip.txt", SELF)
    END RECEIVE

    RECEIVE File_Read_Completed(outcome)
        contents = TRY outcome
        print(contents)
        STOP
    END RECEIVE
END ACTOR

ACTION main()
    client_ref = SPAWN File_Client
    SEND File_System, File_Write("roundtrip.txt", "utf8: café 東京", client_ref)
END ACTION
