function(huxerui_configure_linux_project_package target_name install_component)
    if (NOT HUXERUI_PACKAGE)
        return()
    endif ()

    install(TARGETS ${target_name}
            RUNTIME DESTINATION usr/bin
            COMPONENT "${install_component}"
    )
    _huxerui_install_runtime_dependencies(${target_name} "${install_component}"
            usr "bin/$<TARGET_FILE_NAME:${target_name}>"
    )
    get_target_property(HUXERUI_LINUX_APP_RESOURCES
            ${target_name}
            HUXERUI_RESOURCE_PACKAGE
    )
    if (HUXERUI_LINUX_APP_RESOURCES
            AND NOT HUXERUI_LINUX_APP_RESOURCES MATCHES "-NOTFOUND$")
        install(DIRECTORY "${HUXERUI_LINUX_APP_RESOURCES}/"
                DESTINATION "usr/bin/${target_name}.resources"
                COMPONENT "${install_component}"
        )
    endif ()
    set(app_run "${CMAKE_CURRENT_BINARY_DIR}/huxerui-platform/linux/AppRun")
    configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/package/AppRun" "${app_run}" @ONLY NEWLINE_STYLE UNIX)
    install(PROGRAMS "${app_run}"
            DESTINATION .
            COMPONENT "${install_component}"
    )
    set(desktop_entry "${CMAKE_CURRENT_BINARY_DIR}/huxerui-platform/linux/${target_name}.desktop")
    configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/package/${target_name}.desktop.in"
            "${desktop_entry}" @ONLY)
    install(FILES "${desktop_entry}"
            DESTINATION .
            COMPONENT "${install_component}"
    )
    install(FILES "${DRAFTSPAN_APP_ICON}"
            DESTINATION .
            RENAME "${target_name}.svg"
            COMPONENT "${install_component}"
    )
endfunction()
