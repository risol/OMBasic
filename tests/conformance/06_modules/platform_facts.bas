IMPORT "@std/platform/platform.bas"
IMPORT "@std/text/text.bas"

ACTION main()
    print($"os={platform.os_name()}")
    print($"architecture={platform.architecture()}")
    print($"triple={platform.target_triple()}")
    print($"path-separator={platform.path_separator()}")
    print($"line-separator-bytes={text.byte_length(platform.line_separator())}")
END ACTION
