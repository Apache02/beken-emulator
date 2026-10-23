# embed_file(INPUT OUTPUT NAMESPACE SYMBOL)
#
# Writes OUTPUT, a C++ source defining
#   namespace NAMESPACE { extern const unsigned char SYMBOL[]; extern const unsigned long SYMBOL_size; }
# with the bytes of INPUT. OUTPUT is only touched when its content changes, and
# the configuration step runs again when INPUT changes.
function (embed_file input output namespace symbol)
    file(READ "${input}" hex HEX)
    string(LENGTH "${hex}" hex_length)
    math(EXPR size "${hex_length} / 2")
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
    set(temporary "${output}.tmp")
    file(WRITE "${temporary}"
            "// Generated from ${input}; do not edit.\n"
            "namespace ${namespace} {\n"
            "extern const unsigned char ${symbol}[] = {${bytes}};\n"
            "extern const unsigned long ${symbol}_size = ${size};\n"
            "} // namespace ${namespace}\n")
    execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different "${temporary}" "${output}")
    file(REMOVE "${temporary}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${input}")
endfunction ()
