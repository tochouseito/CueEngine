add_test(NAME CueEngine.Smoke COMMAND CueEngine)

set_tests_properties(CueEngine.Smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "CueEngine smoke OK"
)
