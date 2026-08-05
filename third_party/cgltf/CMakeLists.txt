# cgltf — vendored, header-only. Warnings stay relaxed for vendored code
# (SYSTEM include), same policy as libdxfrw.
add_library(cgltf INTERFACE)
target_include_directories(cgltf SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
