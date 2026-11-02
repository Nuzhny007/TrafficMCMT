# Common compiler warning settings for TrafficMCMT targets.

function(tmcmt_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4
            /permissive-
            /utf-8
            /Zc:__cplusplus
            /external:W0
            /external:anglebrackets)
        target_compile_options(${target} PRIVATE /MP)
    else()
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wshadow
            -Wconversion
            -Wsign-conversion
            -Wnon-virtual-dtor
            -Wold-style-cast)
    endif()
endfunction()
