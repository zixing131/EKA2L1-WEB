set(SOURCE_12L1R_PUBLIC
        include/cpu/12l1r/common.h
        include/cpu/12l1r/exclusive_monitor.h
        include/cpu/12l1r/tlb.h
        src/12l1r/common.cpp
        src/12l1r/exclusive_monitor.cpp)

set(SOURCE_12L1R
        include/cpu/12l1r/decoder/decoder_detail.h
        include/cpu/12l1r/decoder/matcher.h
        include/cpu/12l1r/encoding/arm.h
        include/cpu/12l1r/encoding/thumb16.h
        include/cpu/12l1r/encoding/thumb32.h
        include/cpu/12l1r/arm_12l1r.h
        include/cpu/12l1r/arm_visitor.h
        include/cpu/12l1r/block_cache.h
        include/cpu/12l1r/block_gen.h
        include/cpu/12l1r/core_state.h
        include/cpu/12l1r/float_marker.h
        include/cpu/12l1r/reg_cache.h
        include/cpu/12l1r/reg_loc.h
        include/cpu/12l1r/thumb_visitor.h
        include/cpu/12l1r/visit_session.h
        src/12l1r/tests/imb_range.cpp
        src/12l1r/tests/mem_stress.cpp
        src/12l1r/tests/reg_cache_stress.cpp
        src/12l1r/tests/test_cpu.cpp
        src/12l1r/tests/test_cpu.h
        src/12l1r/tests/test_entry.cpp
        src/12l1r/tests/vfp.cpp
        src/12l1r/translate/branch.cpp
        src/12l1r/translate/coprocessor.cpp
        src/12l1r/translate/data_processing.cpp
        src/12l1r/translate/extension.cpp
        src/12l1r/translate/hint.cpp
        src/12l1r/translate/interrupt.cpp
        src/12l1r/translate/load_store.cpp
        src/12l1r/translate/miscs.cpp
        src/12l1r/translate/multiply.cpp
        src/12l1r/translate/saturated.cpp
        src/12l1r/translate/status_register_access.cpp
        src/12l1r/translate/synchronization.cpp
        src/12l1r/translate/vfp.cpp
        src/12l1r/arm_12l1r.cpp
        src/12l1r/arm_visitor.cpp
        src/12l1r/block_cache.cpp
        src/12l1r/block_gen.cpp
        src/12l1r/core_state.cpp
        src/12l1r/float_marker.cpp
        src/12l1r/reg_cache.cpp
        src/12l1r/reg_loc.cpp
        src/12l1r/thumb_visitor.cpp
        src/12l1r/visit_session.cpp)

option(EKA2L1_CPU_DYNCOM_ONLY "Build the portable dyncom backend without native JITs" OFF)
option(EKA2L1_DYNCOM_PROFILE "Profile guest instruction frequencies in native dyncom" OFF)
option(EKA2L1_DYNCOM_VFP_TRACE "Keep native dyncom VFP trace logging" OFF)

set(SOURCE_DYNCOM
        include/cpu/dyncom/vfp/asm_vfp.h
        include/cpu/dyncom/vfp/vfp_helper.h
        include/cpu/dyncom/vfp/vfp.h
        include/cpu/dyncom/vfp/vfpinstr.h
        include/cpu/dyncom/arm_dyncom.h
        include/cpu/dyncom/arm_dyncom_dec.h
        include/cpu/dyncom/arm_dyncom_interpreter.h
        include/cpu/dyncom/arm_dyncom_run.h
        include/cpu/dyncom/arm_dyncom_thumb.h
        include/cpu/dyncom/arm_dyncom_trans.h
        include/cpu/dyncom/arm_regformat.h
        include/cpu/dyncom/armstate.h
        include/cpu/dyncom/armsupp.h
        src/dyncom/vfp/vfp.cpp
        src/dyncom/vfp/vfpdouble.cpp
        src/dyncom/vfp/vfpsingle.cpp
        src/dyncom/arm_dyncom.cpp
        src/dyncom/arm_dyncom_dec.cpp
        src/dyncom/arm_dyncom_interpreter.cpp
        src/dyncom/arm_dyncom_thumb.cpp
        src/dyncom/arm_dyncom_trans.cpp
        src/dyncom/arm_dyncom_wasmjit.cpp
        src/dyncom/arm_counted_loop.inl
        src/dyncom/thumb_counted_loop.inl
        include/cpu/dyncom/arm_dyncom_jit.h
        src/dyncom/armstate.cpp
        src/dyncom/armsupp.cpp)

# The native and Web decoders use different translated instruction layouts.
# Build a single implementation per target so headers and translators agree.
if (NOT EMSCRIPTEN AND NOT EKA2L1_CPU_DYNCOM_ONLY)
    set(EKA2L1_NATIVE_DYNCOM ON)
    set(SOURCE_NATIVE_DYNCOM src/ekalynx/interpreter.cpp)
    foreach(source IN LISTS SOURCE_DYNCOM)
        if (source MATCHES "wasmjit|arm_dyncom_jit|/arm_counted_loop")
            continue()
        endif()
        if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/native/${source}")
            list(APPEND SOURCE_NATIVE_DYNCOM "native/${source}")
        else()
            list(APPEND SOURCE_NATIVE_DYNCOM "${source}")
        endif()
    endforeach()
    set(SOURCE_DYNCOM ${SOURCE_NATIVE_DYNCOM})
endif()

add_library(cpu
        include/cpu/arm_analyser.h
        include/cpu/arm_analyser_capstone.h
        include/cpu/arm_factory.h
        include/cpu/arm_interface.h
        include/cpu/arm_utils.h
        src/arm_analyser_capstone.cpp
        src/arm_analyser.cpp
        src/arm_factory.cpp
        src/arm_utils.cpp
        ${SOURCE_12L1R_PUBLIC}
        ${SOURCE_DYNCOM})

if (EKA2L1_CPU_DYNCOM_ONLY)
    target_compile_definitions(cpu PRIVATE EKA2L1_CPU_DYNCOM_ONLY_BUILD=1)
elseif (EMSCRIPTEN)
    # The Web backend uses portable interpreter dispatch and the Web JIT.
elseif (ARCHITECTURE_ARM32)
    target_sources(cpu PRIVATE ${SOURCE_12L1R})
    target_link_libraries(cpu PRIVATE Catch2)
elseif (NOT EKA2L1_IOS OR EKA2L1_IOS_DYNARMIC)
    target_sources(cpu PRIVATE include/cpu/arm_dynarmic.h src/arm_dynarmic.cpp)
    target_link_libraries(cpu PRIVATE dynarmic)
endif()

if (EKA2L1_NATIVE_DYNCOM)
    target_include_directories(cpu BEFORE PUBLIC native/include)
    if (EKA2L1_DYNCOM_PROFILE)
        target_compile_definitions(cpu PUBLIC EKA2L1_DYNCOM_PROFILE=1)
    endif()
    if (NOT EKA2L1_DYNCOM_VFP_TRACE)
        target_compile_definitions(cpu PRIVATE EKA2L1_DYNCOM_DISABLE_VFP_TRACE=1)
    endif()
endif()

target_include_directories(cpu PUBLIC include)
target_include_directories(cpu PRIVATE ${capstone_INCLUDE_DIRS})

target_link_libraries(cpu PUBLIC common)
target_link_libraries(cpu
        PRIVATE
        capstone)

# Differential test harness for the dyncom interpreter (self-A/B + golden ALU
# oracle over randomized instruction streams). Standalone host tool, default OFF.
# See scripts/cpu_difftest.sh. Used to gate the ASID instruction-cache change and
# other shared-CPU optimizations.
option(EKA2L1_BUILD_DYNCOM_DIFFTEST "Build the dyncom interpreter differential test harness" OFF)
if (EKA2L1_BUILD_DYNCOM_DIFFTEST)
    if (EKA2L1_NATIVE_DYNCOM AND TARGET dynarmic)
        target_compile_definitions(cpu PUBLIC EKA2L1_DYNCOM_DIFFTEST=1)
        add_executable(dyncom_difftest native/src/dyncom/tests/dyncom_difftest.cpp)
        target_link_libraries(dyncom_difftest PRIVATE cpu common dynarmic)
        add_executable(arm_replay src/dyncom/tests/arm_replay.cpp)
        target_link_libraries(arm_replay PRIVATE cpu)
    elseif (NOT EKA2L1_NATIVE_DYNCOM)
        add_executable(dyncom_difftest src/dyncom/tests/dyncom_difftest.cpp)
        target_link_libraries(dyncom_difftest PRIVATE cpu common)
    endif()
endif()

# Runs actual runtime-generated WASM against the interpreter and golden model.
# Explicit target only: it does not add test fixtures to the browser frontend.
if (EMSCRIPTEN)
    add_executable(wasmjit_difftest EXCLUDE_FROM_ALL src/dyncom/tests/wasmjit_difftest.cpp)
    target_link_libraries(wasmjit_difftest PRIVATE cpu common)
    target_link_options(wasmjit_difftest PRIVATE
        -O3 -pthread
        "SHELL:-s ENVIRONMENT=node,web"
        "SHELL:-s ALLOW_TABLE_GROWTH=1"
        "SHELL:-s ALLOW_MEMORY_GROWTH=1"
        "SHELL:-s INITIAL_MEMORY=134217728"
        "SHELL:-s MAXIMUM_MEMORY=1073741824"
        "SHELL:-s STACK_SIZE=8388608"
        "SHELL:-s DISABLE_EXCEPTION_CATCHING=0"
        "SHELL:-s EXIT_RUNTIME=1")
    set_target_properties(wasmjit_difftest PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/jit-tests")
endif()
