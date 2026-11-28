# CMake generated Testfile for 
# Source directory: /home/Zevor/Zethropol-OS/desktop/zci-chat
# Build directory: /home/Zevor/Zethropol-OS/desktop/zci-chat/build-verify
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test("zci_chat_qml_test" "/home/Zevor/Zethropol-OS/desktop/zci-chat/build-verify/zci_chat_qml_test")
set_tests_properties("zci_chat_qml_test" PROPERTIES  ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QT_QUICK_CONTROLS_STYLE=Basic;QT_QPA_PLATFORMTHEME=" TIMEOUT "60" _CMAKE_TEST_BUILD_DEPENDS "/home/Zevor/Zethropol-OS/desktop/zci-chat/build-verify/zci_chat_qml_test" _BACKTRACE_TRIPLES "/home/Zevor/Zethropol-OS/desktop/zci-chat/CMakeLists.txt;92;add_test;/home/Zevor/Zethropol-OS/desktop/zci-chat/CMakeLists.txt;0;")
