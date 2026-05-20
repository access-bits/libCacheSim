# ==============================================================================
# Cachesim — YAML-driven cache simulator
# ==============================================================================

set(cachesim_sources_c
    main.c
    yaml_config.c
    ../cli_reader_utils.c
)
add_executable(cachesim ${cachesim_sources_c})
target_include_directories(cachesim PRIVATE
    ${libCacheSim_include_dir}
    ${GLib_INCLUDE_DIRS}
    ${libyaml_INCLUDE_DIRS}
    ../)
target_link_libraries(cachesim ${PROJECT_NAME} ${libyaml_LIBRARIES})
install(TARGETS cachesim RUNTIME DESTINATION bin)
