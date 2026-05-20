add_library(logger_lib OBJECT
    log.c
)

target_include_directories(logger_lib PRIVATE
    ${libCacheSim_include_dir}
    ${GLib_INCLUDE_DIRS}
)
