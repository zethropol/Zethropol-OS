cmake_minimum_required(VERSION 3.16)
project(control_center LANGUAGES CXX)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick DBus)

set(ZETHROPOL_DBUS_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/zethropol_security_client.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/zethropol_services_client.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/zethropol_update_client.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/zethropol_recovery_client.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/zethropol_applications_client.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/zethropol_power_client.cpp
   ${CMAKE_CURRENT_SOURCE_DIR}/src/core/zethropol_monitoring_client.cpp
)

set(CONTROL_CENTER_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/HardwareServiceClient.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/PostUpdateFlow.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/SourceReview.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/SourceUpdateRunner.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/UpdateService.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/RecoveryService.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/SecurityService.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ServiceService.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ApplicationService.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/NotificationService.cpp
)

qt_add_executable(control_center
    src/main.cpp
    ${CONTROL_CENTER_SRCS}
    ${ZETHROPOL_DBUS_SRCS}
)

set_source_files_properties(qml/SystemState.qml PROPERTIES
    QT_QML_SINGLETON_TYPE TRUE
)

qt_add_qml_module(control_center
    URI Zethropol.ControlCenter
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/PostUpdateWindows.qml
        qml/UpdatePlanPreview.qml
        qml/UpdateTransactionConfirmation.qml
        qml/AurInstallConfirmation.qml
        qml/AurCancellationConfirmation.qml
        qml/LocalAurCancellationConfirmation.qml
        qml/UpdateErrorDialog.qml
        qml/SystemState.qml
)

target_link_libraries(control_center PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::DBus
)

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)

    qt_add_executable(control_center_update_state_decoder_test
        tests/UpdateStateDecoderTest.cpp
    )
    target_include_directories(control_center_update_state_decoder_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(control_center_update_state_decoder_test PRIVATE
        Qt6::Core Qt6::DBus Qt6::Test
    )
    add_test(NAME control_center_update_state_decoder_test
             COMMAND control_center_update_state_decoder_test)

    qt_add_executable(control_center_update_service_test
        tests/UpdateServiceTest.cpp
        src/SourceReview.cpp src/SourceReview.h
        src/SourceUpdateRunner.cpp src/SourceUpdateRunner.h
        src/UpdateService.cpp
        src/UpdateService.h
        src/core/zethropol_update_client.cpp
        src/core/zethropol_update_client.h
    )
    target_include_directories(control_center_update_service_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(control_center_update_service_test PRIVATE
        Qt6::Core
        Qt6::DBus
        Qt6::Test
    )
    add_test(NAME control_center_update_service_test
             COMMAND control_center_update_service_test)
    set_tests_properties(control_center_update_service_test PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    qt_add_executable(control_center_update_plan_preview_ui_test
        tests/UpdatePlanPreviewUiTest.cpp
    )
    target_link_libraries(control_center_update_plan_preview_ui_test PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick
        Qt6::Test
    )
    target_compile_definitions(control_center_update_plan_preview_ui_test PRIVATE
        UPDATE_CONFIRMATION_QML_PATH="${CMAKE_CURRENT_SOURCE_DIR}/qml/UpdateTransactionConfirmation.qml"
        UPDATE_PLAN_PREVIEW_QML_PATH="${CMAKE_CURRENT_SOURCE_DIR}/qml/UpdatePlanPreview.qml"
    )
    qt_add_executable(control_center_aur_confirmation_ui_test
        tests/AurConfirmationUiTest.cpp
    )
    target_link_libraries(control_center_aur_confirmation_ui_test PRIVATE
        Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::Test
    )
    target_compile_definitions(control_center_aur_confirmation_ui_test PRIVATE
        AUR_INSTALL_QML_PATH="${CMAKE_CURRENT_SOURCE_DIR}/qml/AurInstallConfirmation.qml"
        AUR_CANCEL_QML_PATH="${CMAKE_CURRENT_SOURCE_DIR}/qml/AurCancellationConfirmation.qml"
        AUR_LOCAL_CANCEL_QML_PATH="${CMAKE_CURRENT_SOURCE_DIR}/qml/LocalAurCancellationConfirmation.qml"
    )
    add_test(NAME control_center_aur_confirmation_ui_test
             COMMAND control_center_aur_confirmation_ui_test)
    set_tests_properties(control_center_aur_confirmation_ui_test PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )
    add_test(NAME control_center_update_plan_preview_ui_test
             COMMAND control_center_update_plan_preview_ui_test)
    set_tests_properties(control_center_update_plan_preview_ui_test PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )
    qt_add_executable(control_center_pending_updates_decoder_test
        tests/PendingUpdatesDecoderTest.cpp
        src/core/zethropol_update_client.cpp
        src/core/zethropol_update_client.h
    )
    target_include_directories(control_center_pending_updates_decoder_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(control_center_pending_updates_decoder_test PRIVATE
        Qt6::Core Qt6::DBus Qt6::Test
    )
    add_test(NAME control_center_pending_updates_decoder_test
             COMMAND control_center_pending_updates_decoder_test)
    qt_add_executable(control_center_operation_result_decoder_test
        tests/OperationResultDecoderTest.cpp
        src/core/zethropol_update_client.cpp
        src/core/zethropol_update_client.h
    )
    target_include_directories(control_center_operation_result_decoder_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(control_center_operation_result_decoder_test PRIVATE
        Qt6::Core Qt6::DBus Qt6::Test
    )
    add_test(NAME control_center_operation_result_decoder_test
             COMMAND control_center_operation_result_decoder_test)
    qt_add_executable(control_center_post_update_flow_test
        tests/PostUpdateFlowTest.cpp src/PostUpdateFlow.cpp src/PostUpdateFlow.h
    )
    target_include_directories(control_center_post_update_flow_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(control_center_post_update_flow_test PRIVATE
        Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::DBus Qt6::Test)
    target_compile_definitions(control_center_post_update_flow_test PRIVATE
        POST_UPDATE_QML_PATH="${CMAKE_CURRENT_SOURCE_DIR}/qml/PostUpdateWindows.qml")
    add_test(NAME control_center_post_update_flow_test COMMAND control_center_post_update_flow_test)
    set_tests_properties(control_center_post_update_flow_test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    qt_add_executable(control_center_source_workflow_test
        tests/SourceWorkflowTest.cpp src/SourceReview.cpp src/SourceReview.h
        src/SourceUpdateRunner.cpp src/SourceUpdateRunner.h)
    target_include_directories(control_center_source_workflow_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(control_center_source_workflow_test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME control_center_source_workflow_test COMMAND control_center_source_workflow_test)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME control_center_aur_worker_test
            COMMAND ${Python3_EXECUTABLE} -B -m unittest discover
                -s ${CMAKE_CURRENT_SOURCE_DIR}/aur/tests -v)
        option(ZETHROPOL_AUR_LIVE_TESTS "Run real temporary AUR sandbox and user-cgroup tests" OFF)
        if(ZETHROPOL_AUR_LIVE_TESTS)
            foreach(aur_live_test live_sandbox live_build live_lifecycle live_quota live_signed_root live_polkit)
                add_test(NAME control_center_aur_${aur_live_test}_test
                    COMMAND ${Python3_EXECUTABLE} -B
                        ${CMAKE_CURRENT_SOURCE_DIR}/aur/tests/${aur_live_test}.py)
                set_tests_properties(control_center_aur_${aur_live_test}_test
                    PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
            endforeach()
        endif()
    endif()
endif()

# Build-tree worker composition only; no install or global source replacement.
set(ZETHROPOL_AUR_PIPELINE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/aur/pipeline.py" CACHE FILEPATH "Unprivileged AUR pipeline path; set installed path only for separately reviewed release packaging")
foreach(aur_pipeline_target control_center control_center_update_service_test control_center_source_workflow_test)
    if(TARGET ${aur_pipeline_target})
        target_compile_definitions(${aur_pipeline_target} PRIVATE
            ZETHROPOL_AUR_PIPELINE="${ZETHROPOL_AUR_PIPELINE_PATH}")
    endif()
endforeach()
