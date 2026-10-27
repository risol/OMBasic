FUNCTION native_wrapper()
    native_call()
END FUNCTION

FUNCTION hidden_effect()
    native_wrapper()
END FUNCTION

ACTION main()
    hidden_effect()
END ACTION
