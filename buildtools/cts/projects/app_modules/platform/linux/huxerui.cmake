function(huxerui_configure_linux_project_package target_name install_component)
    if (NOT HUXERUI_PACKAGE)
        return()
    endif ()

    set(package_directory usr)
    if (HUXERUI_LINUX_SYSTEM_PACKAGE)
        set(package_directory "usr/lib/org.huxerui.cts.appmodules")
    endif ()
    install(TARGETS ${target_name}
            RUNTIME DESTINATION "${package_directory}/bin"
            COMPONENT "${install_component}"
    )
    _huxerui_install_runtime_dependencies(${target_name} "${install_component}"
            "${package_directory}" "bin/$<TARGET_FILE_NAME:${target_name}>"
    )
    get_target_property(HUXERUI_LINUX_APP_RESOURCES
            ${target_name}
            HUXERUI_RESOURCE_PACKAGE
    )
    if (HUXERUI_LINUX_APP_RESOURCES
            AND NOT HUXERUI_LINUX_APP_RESOURCES MATCHES "-NOTFOUND$")
        install(DIRECTORY "${HUXERUI_LINUX_APP_RESOURCES}/"
                DESTINATION "${package_directory}/bin/$<TARGET_FILE_BASE_NAME:${target_name}>.resources"
                COMPONENT "${install_component}"
        )
    endif ()
    if (HUXERUI_LINUX_SYSTEM_PACKAGE)
        file(READ "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/package/cts_app_modules.desktop" desktop_entry)
        string(REGEX REPLACE "(^|\n)Exec=[^\n]*" "\\1Exec=/${package_directory}/bin/$<TARGET_FILE_NAME:${target_name}>"
                desktop_entry "${desktop_entry}")
        string(REGEX REPLACE "(^|\n)Icon=[^\n]*" "\\1Icon=org.huxerui.cts.appmodules" desktop_entry "${desktop_entry}")
        set(installed_desktop_entry "${CMAKE_CURRENT_BINARY_DIR}/huxerui-package/$<CONFIG>/org.huxerui.cts.appmodules.desktop")
        file(GENERATE OUTPUT "${installed_desktop_entry}" CONTENT "${desktop_entry}")
        install(FILES "${installed_desktop_entry}" DESTINATION usr/share/applications COMPONENT "${install_component}")
        install(FILES "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/package/cts_app_modules.svg"
                DESTINATION usr/share/icons/hicolor/scalable/apps
                RENAME "org.huxerui.cts.appmodules.svg" COMPONENT "${install_component}")
        file(CONFIGURE OUTPUT "${CMAKE_BINARY_DIR}/huxerui-package/LinuxCPack.cmake" CONTENT [=[
set(CPACK_PACKAGE_NAME "org.huxerui.cts.appmodules")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "CTS App Modules")
set(CPACK_PACKAGE_DESCRIPTION "CTS App Modules")
set(CPACK_PACKAGING_INSTALL_PREFIX "/")
set(CPACK_PACKAGE_RELOCATABLE OFF)

set(CPACK_DEBIAN_PACKAGE_MAINTAINER "org.huxerui.cts.appmodules")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libgtk-4-1 (>= 4.14), librsvg2-common")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)

set(CPACK_RPM_PACKAGE_DESCRIPTION "CTS App Modules")
set(CPACK_RPM_PACKAGE_REQUIRES "gtk4 >= 4.14")
set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
        /usr/share/applications /usr/share/icons /usr/share/icons/hicolor
        /usr/share/icons/hicolor/scalable /usr/share/icons/hicolor/scalable/apps)
set(HUXERUI_PACKAGE_APP_DIRECTORY "@package_directory@")
set(CPACK_PRE_BUILD_SCRIPTS "@CMAKE_CURRENT_FUNCTION_LIST_DIR@/package/rpm_dependencies.cmake")
]=] @ONLY)
        return()
    endif ()
    install(PROGRAMS "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/package/AppRun"
            DESTINATION .
            COMPONENT "${install_component}"
    )
    install(FILES
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/package/cts_app_modules.desktop"
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/package/cts_app_modules.svg"
            DESTINATION .
            COMPONENT "${install_component}"
    )
endfunction()
