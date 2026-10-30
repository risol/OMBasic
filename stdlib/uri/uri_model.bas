EXPORT RECORD Uri(
    scheme_present,
    scheme,
    authority_present,
    user_info_present,
    user_info,
    host,
    port_present,
    port,
    path,
    query_present,
    query,
    fragment_present,
    fragment
)

EXPORT RECORD Query_Pair(name, value)
