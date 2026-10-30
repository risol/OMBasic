ACTOR Binary_File_Client
    RECEIVE File_Write_Bytes_Completed(outcome)
        write_result = TRY outcome
        SEND File_System, File_Append_Bytes("binary.bin", [0, 255], SELF)
    END RECEIVE

    RECEIVE File_Append_Bytes_Completed(outcome)
        append_result = TRY outcome
        SEND File_System, File_Read_Bytes("binary.bin", SELF)
    END RECEIVE

    RECEIVE File_Read_Bytes_Completed(outcome)
        bytes = TRY outcome
        print($"{bytes[0]}|{bytes[1]}|{bytes[2]}|{bytes[3]}")
        STOP
    END RECEIVE
END ACTOR

ACTION main()
    client_ref = SPAWN Binary_File_Client
    SEND File_System, File_Write_Bytes("binary.bin", [0, 1, 254, 255], client_ref)
END ACTION
