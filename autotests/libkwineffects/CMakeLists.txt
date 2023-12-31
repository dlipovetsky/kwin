include(ECMMarkAsTest)

macro(KWINEFFECTS_UNIT_TESTS)
   foreach(_testname ${ARGN})
      add_executable(${_testname} ${_testname}.cpp)
      add_test(NAME kwineffects-${_testname} COMMAND ${_testname})
      target_link_libraries(${_testname} Qt::Test kwineffects)
      ecm_mark_as_test(${_testname})
   endforeach()
endmacro()

kwineffects_unit_tests(
    windowquadlisttest
    timelinetest
)

add_executable(kwinglplatformtest kwinglplatformtest.cpp mock_gl.cpp ../../src/libkwineffects/kwinglplatform.cpp ../../src/libkwineffects/openglcontext.cpp)
add_test(NAME kwineffects-kwinglplatformtest COMMAND kwinglplatformtest)
target_link_libraries(kwinglplatformtest Qt::Test Qt::Gui KF6::ConfigCore XCB::XCB)
ecm_mark_as_test(kwinglplatformtest)
