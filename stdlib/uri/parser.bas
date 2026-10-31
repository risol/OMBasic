IMPORT "./uri_error.bas"
IMPORT "./uri_model.bas"
NATIVE IMPORT "./uri_native.omi"
IMPORT "@std/text/text.bas"

FUNCTION fragmentless(value)
    index = text.find(value, "#")
    IF index < 0 THEN
        RETURN value
    END IF
    RETURN text.slice(value, 0, index)
END FUNCTION

FUNCTION queryless(value)
    source = fragmentless(value)
    index = text.find(source, "?")
    IF index < 0 THEN
        RETURN source
    END IF
    RETURN text.slice(source, 0, index)
END FUNCTION

FUNCTION scheme_end(value)
    source = queryless(value)
    colon = text.find(source, ":")
    slash = text.find(source, "/")
    IF colon > 0 AND (slash < 0 OR colon < slash) THEN
        RETURN colon
    END IF
    RETURN -1
END FUNCTION

FUNCTION scheme_present(value)
    RETURN scheme_end(value) >= 0
END FUNCTION

FUNCTION scheme_value(value)
    source = queryless(value)
    index = scheme_end(value)
    IF index < 0 THEN
        RETURN ""
    END IF
    RETURN text.slice(source, 0, index)
END FUNCTION

FUNCTION after_scheme(value)
    source = queryless(value)
    index = scheme_end(value)
    IF index < 0 THEN
        RETURN source
    END IF
    RETURN text.slice(source, index + 1, text.length(source) - index - 1)
END FUNCTION

FUNCTION authority_present(value)
    source = after_scheme(value)
    RETURN text.length(source) >= 2 AND text.slice(source, 0, 2) = "//"
END FUNCTION

FUNCTION authority_text(value)
    source = after_scheme(value)
    IF NOT authority_present(value) THEN
        RETURN ""
    END IF
    remainder = text.slice(source, 2, text.length(source) - 2)
    slash = text.find(remainder, "/")
    IF slash < 0 THEN
        RETURN remainder
    END IF
    RETURN text.slice(remainder, 0, slash)
END FUNCTION

FUNCTION path_value(value)
    source = after_scheme(value)
    IF NOT authority_present(value) THEN
        RETURN source
    END IF
    remainder = text.slice(source, 2, text.length(source) - 2)
    slash = text.find(remainder, "/")
    IF slash < 0 THEN
        RETURN ""
    END IF
    RETURN text.slice(remainder, slash, text.length(remainder) - slash)
END FUNCTION

FUNCTION userinfo_present(value)
    RETURN text.find_last(authority_text(value), "@") >= 0
END FUNCTION

FUNCTION userinfo_value(value)
    authority = authority_text(value)
    at_index = text.find_last(authority, "@")
    IF at_index < 0 THEN
        RETURN ""
    END IF
    RETURN text.slice(authority, 0, at_index)
END FUNCTION

FUNCTION host_port(value)
    authority = authority_text(value)
    at_index = text.find_last(authority, "@")
    IF at_index < 0 THEN
        RETURN authority
    END IF
    RETURN text.slice(authority, at_index + 1, text.length(authority) - at_index - 1)
END FUNCTION

FUNCTION port_present(value)
    authority = host_port(value)
    IF text.length(authority) > 0 AND text.slice(authority, 0, 1) = "[" THEN
        closing = text.find(authority, "]")
        RETURN closing >= 0 AND text.length(authority) > closing + 1
    END IF
    colon = text.find_last(authority, ":")
    RETURN colon >= 0
END FUNCTION

FUNCTION host_value(value)
    authority = host_port(value)
    IF text.length(authority) > 0 AND text.slice(authority, 0, 1) = "[" THEN
        closing = text.find(authority, "]")
        IF closing >= 0 THEN
            RETURN text.slice(authority, 0, closing + 1)
        END IF
        RETURN authority
    END IF
    colon = text.find_last(authority, ":")
    IF colon < 0 THEN
        RETURN authority
    END IF
    RETURN text.slice(authority, 0, colon)
END FUNCTION

FUNCTION port_value(value)
    authority = host_port(value)
    IF text.length(authority) > 0 AND text.slice(authority, 0, 1) = "[" THEN
        closing = text.find(authority, "]")
        IF closing < 0 OR text.length(authority) <= closing + 1 THEN
            RETURN ""
        END IF
        RETURN text.slice(authority, closing + 2, text.length(authority) - closing - 2)
    END IF
    colon = text.find_last(authority, ":")
    IF colon < 0 THEN
        RETURN ""
    END IF
    RETURN text.slice(authority, colon + 1, text.length(authority) - colon - 1)
END FUNCTION

FUNCTION query_present(value)
    RETURN text.find(fragmentless(value), "?") >= 0
END FUNCTION

FUNCTION query_value(value)
    source = fragmentless(value)
    index = text.find(source, "?")
    IF index < 0 THEN
        RETURN ""
    END IF
    RETURN text.slice(source, index + 1, text.length(source) - index - 1)
END FUNCTION

FUNCTION fragment_present(value)
    RETURN text.find(value, "#") >= 0
END FUNCTION

FUNCTION fragment_value(value)
    index = text.find(value, "#")
    IF index < 0 THEN
        RETURN ""
    END IF
    RETURN text.slice(value, index + 1, text.length(value) - index - 1)
END FUNCTION

EXPORT FUNCTION parse(value)
    status = ascii_status(value)
    IF status = 3 THEN
        RETURN Error(uri_error.Uri_Error("resource_exhausted", 0))
    END IF
    IF status <> 0 THEN
        RETURN Error(uri_error.Uri_Error("invalid_character", ascii_offset(value)))
    END IF
    percent_status = percent_syntax_status(value)
    IF percent_status = 1 THEN
        RETURN Error(uri_error.Uri_Error("invalid_percent_encoding", percent_syntax_offset(value)))
    END IF
    IF percent_status = 3 THEN
        RETURN Error(uri_error.Uri_Error("resource_exhausted", 0))
    END IF
    RETURN Ok(uri_model.Uri(scheme_present(value), scheme_value(value), authority_present(value), userinfo_present(value), userinfo_value(value), host_value(value), port_present(value), port_value(value), path_value(value), query_present(value), query_value(value), fragment_present(value), fragment_value(value)))
END FUNCTION

EXPORT FUNCTION format(value)
    VAR scheme = ""
    VAR authority = ""
    VAR user_info = ""
    VAR port = ""
    VAR query = ""
    VAR fragment = ""
    IF value.scheme_present THEN
        scheme = $"{value.scheme}:"
    END IF
    IF value.authority_present THEN
        IF value.user_info_present THEN
            user_info = $"{value.user_info}@"
        END IF
        IF value.port_present THEN
            port = $":{value.port}"
        END IF
        authority = $"//{user_info}{value.host}{port}"
    END IF
    IF value.query_present THEN
        query = $"?{value.query}"
    END IF
    IF value.fragment_present THEN
        fragment = $"#{value.fragment}"
    END IF
    RETURN $"{scheme}{authority}{value.path}{query}{fragment}"
END FUNCTION

EXPORT FUNCTION remove_dot_segments(path)
    RETURN native_remove_dot_segments(path)
END FUNCTION

FUNCTION merge_paths(base, reference)
    IF base.authority_present AND base.path = "" THEN
        RETURN remove_dot_segments("/" + reference)
    END IF
    directory = text.slice(base.path, 0, text.find_last(base.path, "/") + 1)
    RETURN remove_dot_segments(directory + reference)
END FUNCTION

EXPORT FUNCTION resolve_reference(base, reference)
    IF reference.scheme_present THEN
        RETURN uri_model.Uri(reference.scheme_present, reference.scheme, reference.authority_present, reference.user_info_present, reference.user_info, reference.host, reference.port_present, reference.port, remove_dot_segments(reference.path), reference.query_present, reference.query, reference.fragment_present, reference.fragment)
    END IF
    IF reference.authority_present THEN
        RETURN uri_model.Uri(base.scheme_present, base.scheme, reference.authority_present, reference.user_info_present, reference.user_info, reference.host, reference.port_present, reference.port, remove_dot_segments(reference.path), reference.query_present, reference.query, reference.fragment_present, reference.fragment)
    END IF
    IF reference.path = "" THEN
        IF reference.query_present THEN
            RETURN uri_model.Uri(base.scheme_present, base.scheme, base.authority_present, base.user_info_present, base.user_info, base.host, base.port_present, base.port, base.path, TRUE, reference.query, reference.fragment_present, reference.fragment)
        END IF
        RETURN uri_model.Uri(base.scheme_present, base.scheme, base.authority_present, base.user_info_present, base.user_info, base.host, base.port_present, base.port, base.path, base.query_present, base.query, reference.fragment_present, reference.fragment)
    END IF
    IF text.slice(reference.path, 0, 1) = "/" THEN
        RETURN uri_model.Uri(base.scheme_present, base.scheme, base.authority_present, base.user_info_present, base.user_info, base.host, base.port_present, base.port, remove_dot_segments(reference.path), reference.query_present, reference.query, reference.fragment_present, reference.fragment)
    END IF
    merged = merge_paths(base, reference.path)
    RETURN uri_model.Uri(base.scheme_present, base.scheme, base.authority_present, base.user_info_present, base.user_info, base.host, base.port_present, base.port, merged, reference.query_present, reference.query, reference.fragment_present, reference.fragment)
END FUNCTION

EXPORT FUNCTION is_absolute(value)
    RETURN scheme_present(value)
END FUNCTION

EXPORT FUNCTION default_port(scheme)
    IF scheme = "http" OR scheme = "HTTP" OR scheme = "Http" THEN
        RETURN 80
    END IF
    IF scheme = "https" OR scheme = "HTTPS" OR scheme = "Https" THEN
        RETURN 443
    END IF
    RETURN -1
END FUNCTION

