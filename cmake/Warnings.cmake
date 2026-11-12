# Strict warnings for first-party targets only (third-party code is not ours to fix).
function(cell_set_warnings target)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
endfunction()

# Treat a dependency's headers as system headers so -Werror does not apply to them.
function(cell_mark_system target)
  get_target_property(_aliased ${target} ALIASED_TARGET)
  if(_aliased)
    set(target ${_aliased})
  endif()
  get_target_property(_inc ${target} INTERFACE_INCLUDE_DIRECTORIES)
  if(_inc)
    set_target_properties(${target} PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_inc}")
  endif()
endfunction()
