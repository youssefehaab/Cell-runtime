# -DSANITIZE=address|thread|undefined (comma-separate to combine, e.g. address,undefined).
# Applied globally so FetchContent dependencies are instrumented too.
set(SANITIZE "" CACHE STRING "Sanitizer(s) to enable: address, thread, undefined")
if(SANITIZE)
  if(SANITIZE MATCHES "thread" AND SANITIZE MATCHES "address")
    message(FATAL_ERROR "thread and address sanitizers cannot be combined")
  endif()
  message(STATUS "Sanitizers enabled: ${SANITIZE}")
  add_compile_options(-fsanitize=${SANITIZE} -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=${SANITIZE})
  if(SANITIZE MATCHES "undefined")
    add_compile_options(-fno-sanitize-recover=undefined)
  endif()
endif()
