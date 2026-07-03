# Shared compiler warning configuration (§15: -Wall -Wextra, -Werror behind SP_WERROR).
function(sp_set_warnings target)
    target_compile_options(${target} PRIVATE
        -Wall -Wextra
        $<$<BOOL:${SP_WERROR}>:-Werror>
    )
endfunction()
