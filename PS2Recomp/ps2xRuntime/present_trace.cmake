# TEMP(transformers): offline dependency hooks, source-built with the linked libraries.
# Called after both targets exist. No dependency files are changed in place.
function(ps2x_trace_replace var before after)
    string(FIND "${${var}}" "${before}" pos)
    if(pos LESS 0)
        message(FATAL_ERROR "Presentation trace dependency anchor missing: ${before}")
    endif()
    string(REPLACE "${before}" "" remainder "${${var}}")
    string(LENGTH "${${var}}" n)
    string(LENGTH "${remainder}" m)
    string(LENGTH "${before}" k)
    math(EXPR expected "${m}+${k}")
    if(NOT n EQUAL expected)
        message(FATAL_ERROR "Presentation trace dependency anchor is ambiguous: ${before}")
    endif()
    string(REPLACE "${before}" "${after}" result "${${var}}")
    set(${var} "${result}" PARENT_SCOPE)
endfunction()
if(TARGET raylib)
    get_target_property(trace_sources raylib SOURCES)
    get_target_property(trace_dir raylib SOURCE_DIR)
    set(trace_found FALSE)
    foreach(trace_source IN LISTS trace_sources)
        if(trace_source MATCHES "(^|/)rcore[.]c$")
            if(IS_ABSOLUTE "${trace_source}")
                set(trace_input "${trace_source}")
            else()
                set(trace_input "${trace_dir}/${trace_source}")
            endif()
            file(READ "${trace_input}" text)
            ps2x_trace_replace(text [==[void EndDrawing(void)]==] [==[void (*ps2xPresentTraceHook)(int,int) = 0; // TEMP(transformers)

void EndDrawing(void)]==])
            ps2x_trace_replace(text [==[    SwapScreenBuffer();                  // Copy back buffer to front buffer (screen)]==] [==[    // TEMP(transformers): exact call boundaries; null hook costs branches only when OFF.
    void (*presentHook)(int,int) = ps2xPresentTraceHook;
    if (presentHook) presentHook(29, 1);
    SwapScreenBuffer();                  // Copy back buffer to front buffer (screen)
    if (presentHook) presentHook(29, 0);
    if (presentHook) presentHook(32, 1);]==])
            ps2x_trace_replace(text [==[        WaitTime(CORE.Time.target - CORE.Time.frame);]==] [==[        if (presentHook) presentHook(31, 1); // TEMP(transformers)
        WaitTime(CORE.Time.target - CORE.Time.frame);
        if (presentHook) presentHook(31, 0);]==])
            ps2x_trace_replace(text [==[    PollInputEvents();      // Poll user events (before next frame update)]==] [==[    if (presentHook) presentHook(32, 0); // TEMP(transformers)
    if (presentHook) presentHook(30, 1);
    PollInputEvents();      // Poll user events (before next frame update)
    if (presentHook) presentHook(30, 0);]==])
            set(trace_output "${CMAKE_CURRENT_BINARY_DIR}/ps2_present_rcore.c")
            file(WRITE "${trace_output}" "${text}")
            list(REMOVE_ITEM trace_sources "${trace_source}")
            list(APPEND trace_sources "${trace_output}")
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${trace_input}")
            set(trace_found TRUE)
        endif()
    endforeach()
    if(NOT trace_found)
        message(FATAL_ERROR "Presentation trace did not find raylib/rcore.c")
    endif()
    set_property(TARGET raylib PROPERTY SOURCES "${trace_sources}")
    target_include_directories(raylib PRIVATE "${trace_dir}" "${CMAKE_CURRENT_LIST_DIR}/src/lib")
endif()
if(TARGET parallel-gs)
    get_target_property(trace_sources parallel-gs SOURCES)
    get_target_property(trace_dir parallel-gs SOURCE_DIR)
    set(trace_found FALSE)
    foreach(trace_source IN LISTS trace_sources)
        if(trace_source MATCHES "(^|/)gs_renderer[.]cpp$")
            if(IS_ABSOLUTE "${trace_source}")
                set(trace_input "${trace_source}")
            else()
                set(trace_input "${trace_dir}/${trace_source}")
            endif()
            file(READ "${trace_input}" text)
            ps2x_trace_replace(text [==[#include "gs_renderer.hpp"]==] [==[#include "gs_renderer.hpp"
#include "ps2_present_gpu.h" // TEMP(transformers)]==])
            ps2x_trace_replace(text [==[cmd.begin_region("flush-readback");]==] [==[cmd.begin_region("flush-readback");
	ps2PresentTrace::GpuScope presentReadback(cmd, ps2PresentTrace::GpuReadback); // TEMP(transformers)]==])
            ps2x_trace_replace(text [==[// Flush rendering work.
	{]==] [==[// Flush rendering work.
	{
		// TEMP(transformers): retain mixed per-instance SSAA sample counts as a bitset.
		uint64_t presentSampleMask=0;
		if(ps2PresentTrace::enabled()) for(uint32_t i=0;i<rp.num_instances;++i)
			presentSampleMask |= 1ull << (rp.instances[i].sampling_rate_x_log2 + rp.instances[i].sampling_rate_y_log2);
		ps2PresentTrace::GpuScope presentShade(cmd, ps2PresentTrace::GpuShade, (presentSampleMask<<32)|rp.num_instances);]==])
            ps2x_trace_replace(text [==[sample_crtc_circuit(cmd, *circuit1, priv.dispfb1, rect, super_samples, promoted1);]==] [==[{ ps2PresentTrace::GpuScope trace(cmd, ps2PresentTrace::GpuCircuit, (uint64_t(high_resolution_scanout)<<32)|super_samples);
			sample_crtc_circuit(cmd, *circuit1, priv.dispfb1, rect, super_samples, promoted1); } // TEMP(transformers)]==])
            ps2x_trace_replace(text [==[sample_crtc_circuit(cmd, *circuit2, priv.dispfb2, rect, super_samples, promoted2);]==] [==[{ ps2PresentTrace::GpuScope trace(cmd, ps2PresentTrace::GpuCircuit, (uint64_t(high_resolution_scanout)<<32)|super_samples);
			sample_crtc_circuit(cmd, *circuit2, priv.dispfb2, rect, super_samples, promoted2); } // TEMP(transformers)]==])
            ps2x_trace_replace(text [==[cmd.begin_region("vsync");]==] [==[cmd.begin_region("vsync");
	ps2PresentTrace::GpuScope presentScanout(cmd, ps2PresentTrace::GpuScanout); // TEMP(transformers)]==])
            ps2x_trace_replace(text [==[	result.image = std::move(merged);]==] [==[	presentScanout.finish(); // TEMP(transformers): before flush_submit invalidates cmd reference
	result.image = std::move(merged);]==])
            ps2x_trace_replace(text [==[		stats.num_primitives += rp.num_primitives;]==] [==[		presentShade.finish(); // TEMP(transformers): before check_flush_stats can submit
		stats.num_primitives += rp.num_primitives;]==])
            ps2x_trace_replace(text [==[		LOGW("Unknown video format.\n");
		cmd.end_region();]==] [==[		LOGW("Unknown video format.\n");
		presentScanout.finish(); // TEMP(transformers)
		cmd.end_region();]==])
            ps2x_trace_replace(text [==[			flush_submit(0);
			return result;]==] [==[			presentScanout.finish(); // TEMP(transformers)
			flush_submit(0);
			return result;]==])
            ps2x_trace_replace(text [==[	cmd.end_region();
	check_flush_stats();
}

void GSRenderer::ensure_command_buffer]==] [==[	presentReadback.finish(); // TEMP(transformers)
	cmd.end_region();
	check_flush_stats();
}

void GSRenderer::ensure_command_buffer]==])
            set(trace_output "${CMAKE_CURRENT_BINARY_DIR}/ps2_present_gs_renderer.cpp")
            file(WRITE "${trace_output}" "${text}")
            list(REMOVE_ITEM trace_sources "${trace_source}")
            list(APPEND trace_sources "${trace_output}")
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${trace_input}")
            set(trace_found TRUE)
        endif()
    endforeach()
    if(NOT trace_found)
        message(FATAL_ERROR "Presentation trace did not find parallel-gs/gs_renderer.cpp")
    endif()
    set_property(TARGET parallel-gs PROPERTY SOURCES "${trace_sources}")
    target_include_directories(parallel-gs PRIVATE "${trace_dir}" "${CMAKE_CURRENT_LIST_DIR}/src/lib")
    if(CMAKE_CXX_COMPILER_LOADED)
        target_compile_features(parallel-gs PRIVATE cxx_std_17)
    endif()
endif()
