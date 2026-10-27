' Each dependency is loaded once, including the shared helper module.
IMPORT "acyclic/math.bas"
IMPORT "acyclic/nested/helper.bas" ' Trailing comments do not hide the import.

ACTION main()
END ACTION
