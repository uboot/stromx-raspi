set(TEST_ENVIRONMENT "LD_LIBRARY_PATH=${LD_LIBRARY_PATH}")

if(BUILD_RASPI)
    add_subdirectory(raspi)
endif()

