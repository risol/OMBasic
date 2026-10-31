ACTION main()
    original = [1, 2, 3, 4, 5]
    updated = list_set(original, 1, 9)
    print($"original={original[0]},{original[1]},{original[2]} updated={updated[0]},{updated[1]},{updated[2]}")
    labels = ["old", "kept"]
    updated_labels = list_set(labels, 0, "new")
    print($"labels={labels[0]},{labels[1]} updated={updated_labels[0]},{updated_labels[1]}")
    range_updated = set_range(original, 1, [8, 9])
    range_filled = fill_range(original, 1, 3, 6)
    range_replaced = replace_range(original, 1, 3, [8, 9])
    print($"set={range_updated[0]},{range_updated[1]},{range_updated[2]},{range_updated[3]},{range_updated[4]} fill={range_filled[0]},{range_filled[1]},{range_filled[2]},{range_filled[3]},{range_filled[4]} replace={range_replaced[0]},{range_replaced[1]},{range_replaced[2]},{range_replaced[3]}")
    print($"source={original[0]},{original[1]},{original[2]},{original[3]},{original[4]}")
    updated_labels_range = set_range(labels, 1, ["new"])
    print($"label_range={updated_labels_range[0]},{updated_labels_range[1]} source={labels[0]},{labels[1]}")
END ACTION
