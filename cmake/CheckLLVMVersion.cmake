# cmake/CheckLLVMVersion.cmake
function(check_llvm_version MIN_REQUIRED)
    message(STATUS "Checking for LLVM >= ${MIN_REQUIRED}...")
    find_package(LLVM REQUIRED CONFIG)

    if (LLVM_PACKAGE_VERSION VERSION_LESS ${MIN_REQUIRED})
        message(FATAL_ERROR
            "LLVM version ${LLVM_PACKAGE_VERSION} found, but version ${MIN_REQUIRED} or higher is required."
        )
    else()
        message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
    endif()
endfunction()
