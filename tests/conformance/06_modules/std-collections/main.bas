IMPORT "@std/collections/collections.bas"

ACTION main()
    values = [1, 2, 3]
    print($"length={collections.length(values)}")
    print($"empty={collections.is_empty(values)}")
    print($"contains={collections.contains(values, 2)}")
    print($"index={collections.index_of(values, 3)}")
    combined = collections.concat(values, [4])
    print($"concat={combined[3]}")
    print($"reverse={collections.reverse(values)[0]}")
    mapping = {"alpha": 1, "beta": 2}
    print($"map-length={collections.map_length(mapping)}")
    print($"map-empty={collections.map_is_empty(mapping)}")
    print($"map-has={collections.contains_key(mapping, "beta")}")
    mapping_keys = collections.keys(mapping)
    print($"map-keys={mapping_keys[0]}|{mapping_keys[1]}")
    print($"map-get={collections.get_or(mapping, "beta", 0)}")
    print($"map-default={collections.get_or(mapping, "missing", 9)}")
END ACTION
