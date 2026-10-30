NATIVE IMPORT "./platform_native.omi"

EXPORT FUNCTION os_name()
    RETURN native_os_name()
END FUNCTION

EXPORT FUNCTION architecture()
    RETURN native_architecture()
END FUNCTION

EXPORT FUNCTION target_triple()
    RETURN native_target_triple()
END FUNCTION

EXPORT FUNCTION path_separator()
    RETURN native_path_separator()
END FUNCTION

EXPORT FUNCTION line_separator()
    RETURN native_line_separator()
END FUNCTION
