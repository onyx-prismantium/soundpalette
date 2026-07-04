# Shared compiler warning configuration (§15: -Wall -Wextra, -Werror behind SP_WERROR).
# MSVC interprets -Wall as /Wall (every warning including system headers), so it gets the
# conventional /W4 instead; /WX is MSVC's warnings-as-errors.
function(sp_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4
            $<$<BOOL:${SP_WERROR}>:/WX>
        )
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra
            $<$<BOOL:${SP_WERROR}>:-Werror>
        )
    endif()
endfunction()
