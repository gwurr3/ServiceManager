cmake_minimum_required (VERSION 2.8)
project (s16.registryd)

add_executable (s16.restartd restartd.c sd_notify.c timer.c unit.c)
target_link_libraries (s16.restartd s16 Kqueue::Kqueue)

install(TARGETS s16.restartd RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR})
