IMPORT "@std/collections/collections.bas"

FUNCTION double(value)
    RETURN value * 2
END FUNCTION

FUNCTION is_even(value)
    RETURN value % 2 = 0
END FUNCTION

ACTION main()
    values = [1, 2, 3]
    print($"length={collections.length(values)}")
    print($"empty={collections.is_empty(values)}")
    print($"contains={collections.contains(values, 2)}")
    print($"index={collections.index_of(values, 3)}")
    combined = collections.concat(values, [4])
    print($"concat={combined[3]}")
    print($"reverse={collections.reverse(values)[0]}")
    print($"take={collections.take(values, 2)[1]}")
    print($"take-empty={collections.length(collections.take(values, 0))}")
    print($"drop={collections.drop(values, 2)[0]}")
    print($"slice={collections.slice(values, 1, 2)[1]}")
    print($"slice-empty={collections.length(collections.slice(values, 8, 3))}")
    print($"slice-large={collections.slice(values, 1, 9223372036854775807)[1]}")
    print($"map={collections.map(values, double)[2]}")
    print($"map-empty={collections.length(collections.map([], double))}")
    print($"filter={collections.filter(values, is_even)[0]}")
    print($"filter-empty={collections.length(collections.filter(values, is_even))}")
    mapping = {"alpha": 1, "beta": 2}
    print($"map-length={collections.map_length(mapping)}")
    print($"map-empty={collections.map_is_empty(mapping)}")
    print($"map-has={collections.contains_key(mapping, "beta")}")
    mapping_keys = collections.keys(mapping)
    print($"map-keys={mapping_keys[0]}|{mapping_keys[1]}")
    print($"map-get={collections.get_or(mapping, "beta", 0)}")
    print($"map-default={collections.get_or(mapping, "missing", 9)}")
END ACTION
