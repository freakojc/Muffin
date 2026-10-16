# Keep the DLL interface independent of the compiler's template emission policy.
# MSVC 19.51 emits enough Qt/STL helpers for EXPORT_ALL_SYMBOLS to overflow the
# import-library object limit on both x64 and ARM64.
function(muffin_windows_exports target)
  if(NOT MSVC OR NOT MUFFIN_LIB_TYPE STREQUAL "SHARED")
    return()
  endif()
  set_target_properties(${target} PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS OFF)
  set(_export_dir "${CMAKE_CURRENT_BINARY_DIR}/${target}.dir/$<CONFIG>")
  file(GENERATE OUTPUT "${_export_dir}/export-objects.txt"
    CONTENT "$<JOIN:$<TARGET_OBJECTS:${target}>,\n>\n")
  add_custom_command(TARGET ${target} PRE_LINK
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/filter_windows_exports.py"
      --cmake "${CMAKE_COMMAND}"
      --objects "${_export_dir}/export-objects.txt"
      --output "${_export_dir}/muffin-exports.def"
    COMMENT "Generating ${target} namespace exports"
    VERBATIM)
  target_link_options(${target} PRIVATE "/DEF:${_export_dir}/muffin-exports.def")
  set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/filter_windows_exports.py")
endfunction()
