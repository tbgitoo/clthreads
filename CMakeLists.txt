add_subdirectory(libbthread)


include_directories(
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/libbthread"
)




add_library(clthreads
        SHARED
        a_thread.cpp
        p_thread.cpp
        itc_ctrl.cpp
        itc_ip1q.cpp
        itc_mesg.cpp
        textmsg.cpp
)

find_library( # Sets the name of the path variable.
        android
        #log-lib

        # Specifies the name of the NDK library that
        # you want CMake to locate.
        log
)

target_link_libraries(
        clthreads
        log
)
