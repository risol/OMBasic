IMPORT "@std/collections/list.bas"

MESSAGE Query_Values(items)

ACTOR Query_Actor
    VAR threshold = 1
    RECEIVE Query_Values(items)
        selected = QUERY item IN items
            WHERE item > threshold
            SELECT item * 2
        END QUERY
        print($"selected={selected[0]}|count={list.length(selected)}")
        STOP
    END RECEIVE
END ACTOR

ACTION main()
    target = SPAWN Query_Actor
    SEND target, Query_Values([0, 1, 2, 3])
END ACTION
