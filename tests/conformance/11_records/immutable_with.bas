RECORD User(name, age)

ACTION main()
    original = User("Ada", 36)
    updated = original WITH age = 37
    print($"{original.age}:{updated.age}")
END ACTION
