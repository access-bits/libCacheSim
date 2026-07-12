# ==============================================================================
# SOSP23
# ==============================================================================

set(traceOneHit_sources_cpp
    oneHit/oneHit.cpp
    oneHit/cli_parser.c
    oneHit/main.c
    ../../cli_reader_utils.c
)
add_executable(traceOneHit ${traceOneHit_sources_cpp})
target_include_directories(traceOneHit PRIVATE
    ../../
    ${libCacheSim_include_dir}
    ${GLib_INCLUDE_DIRS}
)
# target_link_libraries(traceOneHit dataStructure_lib traceReader_lib ${Boost_LIBRARIES} ${dependency_libs})
target_link_libraries(traceOneHit ${PROJECT_NAME} ${Boost_LIBRARIES} ${dependency_libs})

set(flash_sources_cpp
    flash/flash.cpp
    ../../MRC/parser_mini.c
    ../../cli_reader_utils.c
)
add_executable(flash ${flash_sources_cpp})
target_include_directories(flash PRIVATE
    ../../
    ${libCacheSim_include_dir}
    ${GLib_INCLUDE_DIRS}
)
target_link_libraries(flash ${PROJECT_NAME} ${dependency_libs})
