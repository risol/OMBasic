MESSAGE Tick(remaining, reporter)
MESSAGE Begin(target)
MESSAGE Seen(value)
MESSAGE Watch_Failure(target)
MESSAGE Trigger()
MESSAGE Fairness_Start(reporter)
MESSAGE Fairness_Quick_Done()
MESSAGE Fairness_Long_Done()
MESSAGE Pulse(remaining, reporter)
MESSAGE Pulse_Done(count)
MESSAGE Callback_Busy()
MESSAGE Runtime_Callback(value)

ACTOR Counter
    VAR items_seen = 0

    RECEIVE Tick(remaining, reporter)
        items_seen = items_seen + 1
        IF remaining > 0 THEN
            SEND SELF, Tick(remaining - 1, reporter)
        ELSE
            SEND reporter, Seen(items_seen)
            STOP
        END IF
    END RECEIVE
END ACTOR

ACTOR Watcher
    VAR observed = 0

    RECEIVE Begin(target)
        WATCH target
    END RECEIVE

    RECEIVE Seen(value)
        observed = observed + value
        IF observed = 2 OR observed >= 5 THEN
            STOP
        END IF
    END RECEIVE

    RECEIVE Actor_Down(downed_actor_id, reason)
        IF reason = NORMAL THEN
            observed = observed + 1
            SEND SELF, Seen(0)
        END IF
    END RECEIVE
END ACTOR

ACTION main()
    counter_worker = SPAWN Counter
    event_watcher = SPAWN Watcher
    SEND event_watcher, Begin(counter_worker)
    SEND event_watcher, Begin(counter_worker)
    SEND counter_worker, Tick(3, event_watcher)
END ACTION

ACTION stop_drops_mailbox()
    counter_worker = SPAWN Counter
    event_watcher = SPAWN Watcher
    SEND counter_worker, Tick(0, event_watcher)
    SEND counter_worker, Tick(0, event_watcher)
END ACTION

ACTION actors_have_isolated_state()
    event_watcher = SPAWN Watcher
    first_counter = SPAWN Counter
    second_counter = SPAWN Counter
    SEND first_counter, Tick(0, event_watcher)
    SEND second_counter, Tick(0, event_watcher)
END ACTION

ACTOR Failing_Actor
    RECEIVE Trigger()
        outcome = TRY Error(7)
    END RECEIVE
END ACTOR

ACTOR Failure_Observer
    RECEIVE Watch_Failure(target)
        WATCH target
    END RECEIVE

    RECEIVE Actor_Down(downed_actor_id, reason)
        IF reason = FAILED THEN
            STOP
        END IF
    END RECEIVE
END ACTOR

ACTOR Long_Task
    RECEIVE Fairness_Start(reporter)
        VAR work = 0
        FOR index = 1 TO 1000
            work = work + 1
        END FOR
        SEND reporter, Fairness_Long_Done()
        STOP
    END RECEIVE
END ACTOR

ACTOR Quick_Task
    RECEIVE Fairness_Start(reporter)
        SEND reporter, Fairness_Quick_Done()
        STOP
    END RECEIVE
END ACTOR

ACTOR Fairness_Observer
    VAR quick_seen = FALSE
    VAR long_after_quick = FALSE

    RECEIVE Fairness_Quick_Done()
        quick_seen = TRUE
    END RECEIVE

    RECEIVE Fairness_Long_Done()
        IF quick_seen THEN
            long_after_quick = TRUE
        END IF
    END RECEIVE
END ACTOR

ACTOR Pulse_Pump
    VAR processed = 0

    RECEIVE Pulse(remaining, reporter)
        processed = processed + 1
        IF remaining > 0 THEN
            SEND SELF, Pulse(remaining - 1, reporter)
        ELSE
            SEND reporter, Pulse_Done(processed)
            STOP
        END IF
    END RECEIVE
END ACTOR

ACTOR Pulse_Collector
    VAR total = 0
    VAR completed = 0

    RECEIVE Pulse_Done(count)
        total = total + count
        completed = completed + 1
    END RECEIVE
END ACTOR

ACTOR Native_Callback_Target
    RECEIVE Callback_Busy()
        VAR work = 0
        FOR index = 1 TO 500000
            work = work + 1
        END FOR
    END RECEIVE

    RECEIVE Runtime_Callback(value)
        IF value = 42 THEN
            STOP
        END IF
    END RECEIVE
END ACTOR

ACTION failed_actor_notifies_watchers()
    failing_worker = SPAWN Failing_Actor
    observer_ref = SPAWN Failure_Observer
    SEND observer_ref, Watch_Failure(failing_worker)
    SEND failing_worker, Trigger()
END ACTION

ACTION watch_stopped_actor()
    counter_worker = SPAWN Counter
    event_watcher = SPAWN Watcher
    SEND counter_worker, Tick(0, event_watcher)
    SEND event_watcher, Begin(counter_worker)
END ACTION

ACTION fairness_quantum()
    long_runner = SPAWN Long_Task
    quick_runner = SPAWN Quick_Task
    observer_ref = SPAWN Fairness_Observer
    SEND long_runner, Fairness_Start(observer_ref)
    SEND quick_runner, Fairness_Start(observer_ref)
END ACTION

ACTION message_pool_stress()
    first_pump = SPAWN Pulse_Pump
    second_pump = SPAWN Pulse_Pump
    collector_ref = SPAWN Pulse_Collector
    SEND first_pump, Pulse(999999, collector_ref)
    SEND second_pump, Pulse(999999, collector_ref)
END ACTION

ACTION native_callback_ingress()
    callback_target = SPAWN Native_Callback_Target
    SEND callback_target, Callback_Busy()
END ACTION
