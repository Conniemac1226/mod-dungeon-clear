if (BUILD_TESTING)
    function(define_dungeon_clear_tests)
        set(MOD_PATH "${CMAKE_SOURCE_DIR}/modules/mod-dungeon-clear")

        # Define our standalone test target
        add_executable(dungeon_clear_tests
            "${MOD_PATH}/t/TestDoorPolicy.cpp"
            "${MOD_PATH}/t/TestDungeonClearMath.cpp"
            "${MOD_PATH}/t/TestHealReposition.cpp"
            "${MOD_PATH}/t/TestCombatRegroup.cpp"
            "${MOD_PATH}/t/TestSmartRest.cpp"
            "${MOD_PATH}/t/TestPostCombatRez.cpp"
            "${MOD_PATH}/t/TestStrandedRecovery.cpp"
            "${MOD_PATH}/t/TestFightInPlace.cpp"
            "${MOD_PATH}/t/TestBossPullback.cpp"
            "${MOD_PATH}/t/TestWaitAtBoss.cpp"
            "${MOD_PATH}/t/TestDungeonClearUtil.cpp"
            "${MOD_PATH}/t/TestDungeonClearApproach.cpp"
            "${MOD_PATH}/t/TestApproachDecisions.cpp"
            "${MOD_PATH}/t/TestDcProgressWatchdog.cpp"
            "${MOD_PATH}/t/TestPullDecisions.cpp"
            "${MOD_PATH}/t/TestScenarioDriver.cpp"
            "${MOD_PATH}/t/TestRoomAggro.cpp"
            "${MOD_PATH}/t/TestNavPenalty.cpp"
            "${MOD_PATH}/t/TestDcHazard.cpp"
            "${MOD_PATH}/t/TestBossRoster.cpp"
            "${MOD_PATH}/t/TestBossOrdering.cpp"
            "${MOD_PATH}/t/TestDifficultyGate.cpp"
            "${MOD_PATH}/t/TestEventRegistry.cpp"
            "${MOD_PATH}/t/TestDungeonEvent.cpp"
            "${MOD_PATH}/t/TestNavGeometry.cpp"
            "${MOD_PATH}/t/TestSplineWindow.cpp"
            "${MOD_PATH}/t/TestMechanarElevatorProbe.cpp"
            "${MOD_PATH}/t/TestStrategyGate.cpp"
            "${MOD_PATH}/t/TestRelevanceLadder.cpp"
            "${MOD_PATH}/t/TestSettingsRegistry.cpp"
            "${MOD_PATH}/t/TestTestRunVerdict.cpp"
            "${MOD_PATH}/t/TestTestDungeonRegistry.cpp"
            "${MOD_PATH}/t/TestTestGearTiers.cpp"
            "${MOD_PATH}/t/TestTestRunRecord.cpp"
            "${MOD_PATH}/t/TestTestRunSelect.cpp"
            "${MOD_PATH}/t/TestTestRoster.cpp"
            "${MOD_PATH}/t/TestTestRunLiveJson.cpp"
            "${MOD_PATH}/t/TestWipeContext.cpp"
            "${MOD_PATH}/t/TestDcDiagSnapshot.cpp"
            "${MOD_PATH}/t/TestTestComp.cpp"
            "${MOD_PATH}/t/TestTestPlanSchedule.cpp"
            "${MOD_PATH}/t/TestTestPlanSummary.cpp"
            "${MOD_PATH}/t/NavHarness.cpp"
            "${MOD_PATH}/t/replay_decisions.cpp"
            "${MOD_PATH}/t/replay_pull.cpp"
            "${CMAKE_SOURCE_DIR}/src/test/mocks/TestMap.cpp"
        )

        # The replay runner reads the captured-decision fixtures from the source
        # tree (the test binary runs from the build dir). Pass the path in.
        # DC_MAPDATA_DIR points the Tier-2 navmesh geometry suite at a sliced
        # mmaps directory (produced by tools/slice_mapdata.py); the suite
        # GTEST_SKIPs every case when it is absent, so a checkout WITHOUT
        # client-derived map data (which is never committed) still builds and
        # passes the rest of the suite.
        target_compile_definitions(dungeon_clear_tests PRIVATE
            DC_FIXTURE_DIR="${MOD_PATH}/t/fixtures"
            DC_MAPDATA_DIR="${MOD_PATH}/t/fixtures/mapdata"
        )

        # Link the necessary targets
        target_link_libraries(dungeon_clear_tests
            game
            gtest_main
            gmock_main
            game-interface
            modules
            scripts
        )

        # Include directories
        target_include_directories(dungeon_clear_tests PRIVATE
            "${MOD_PATH}/src"
            "${MOD_PATH}/src/Ai/Dungeon/DungeonClear/Util"
            "${CMAKE_SOURCE_DIR}/src/test/mocks"
        )

        # Place executable directly in the main build folder for easy execution
        set_target_properties(dungeon_clear_tests PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        )
    endfunction()

    # Defer execution to the root directory's configuration end, after googletest has been fetched by the core
    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL define_dungeon_clear_tests)
endif()
