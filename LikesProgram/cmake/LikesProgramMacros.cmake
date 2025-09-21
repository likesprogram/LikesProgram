# ======================================================
# LikesProgramMacros.cmake
# ======================================================

# 添加插件
function(likesprogram_add_plugin plugin_name)
    set(options)
    set(oneValueArgs SOURCES)
    cmake_parse_arguments(LP_PLUGIN "${options}" "${oneValueArgs}" "" ${ARGN})

    if(NOT LP_PLUGIN_SOURCES)
        message(FATAL_ERROR "likesprogram_add_plugin: 必须提供 SOURCES 参数")
    endif()

    add_library(${plugin_name} MODULE ${LP_PLUGIN_SOURCES})
    target_link_libraries(${plugin_name} PRIVATE LikesProgram)

    install(TARGETS ${plugin_name}
        DESTINATION lib/LikesProgram/plugins
    )
endfunction()
