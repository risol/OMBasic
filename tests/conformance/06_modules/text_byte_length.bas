IMPORT "@std/text/text.bas"

ACTION main()
    print($"scalar={text.length("café 東京")}")
    print($"bytes={text.byte_length("café 東京")}")
END ACTION
