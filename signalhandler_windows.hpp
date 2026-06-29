#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdlib>

#include <fstream>
#include <filesystem>
#include <vector>
#include <stdexcept>

#include <cpptrace/cpptrace.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>

#define SAFE_PRINT_ERR(msg) \
    do { \
        DWORD bytesWritten; \
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), msg, lstrlenA(msg), &bytesWritten,  NULL); \
    } while (0)


// Write raw stack frames to the dump file using only async-signal-safe Win32 calls.
LONG WINAPI safe_crash_handler(EXCEPTION_POINTERS* ExceptionInfo) {
    SAFE_PRINT_ERR("\n!!! [System Crash] Detect fatal signal!!!\n");

    DWORD excCode = ExceptionInfo->ExceptionRecord->ExceptionCode;

    if (excCode == EXCEPTION_ACCESS_VIOLATION) {
        SAFE_PRINT_ERR("Reason: EXCEPTION_ACCESS_VIOLATION (Segmentation Fault / Null Pointer)\n)");
    } else if (excCode == EXCEPTION_FLT_DIVIDE_BY_ZERO || excCode == EXCEPTION_INT_DIVIDE_BY_ZERO) {
        SAFE_PRINT_ERR("Reason: DIVIDE_BY_ZERO (Floating Point / Integer Exception)\n");
    } else if (excCode == EXCEPTION_ILLEGAL_INSTRUCTION) {
        SAFE_PRINT_ERR("Reason: EXCEPTION_ILLEGAL_INSTRUCTION (Illegal Instruction)\n");
    } else if (excCode == EXCEPTION_STACK_OVERFLOW) {
        SAFE_PRINT_ERR("Reason: EXCEPTION_STACK_OVERFLOW (Stack Overflow)\n");
    } else {
        SAFE_PRINT_ERR("Reason: Unknown Exception Code\n");
    }

    SAFE_PRINT_ERR("Generating stack trace...\n\n");

    auto raw_trace = cpptrace::generate_raw_trace();

    HANDLE hFile = CreateFileA(
        "trace.bin",
        GENERIC_WRITE,
        0,
        NULL,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    if (hFile != INVALID_HANDLE_VALUE) {
        DWORD bytesWritten;

        // Write the faulting address as a safe_object_frame (object-relative) so it
        // survives ASLR between runs, just like the other frames.
        cpptrace::safe_object_frame fault_frame{};
        cpptrace::get_safe_object_frame(
            reinterpret_cast<cpptrace::frame_ptr>(ExceptionInfo->ExceptionRecord->ExceptionAddress),
            &fault_frame
        );
        WriteFile(hFile, &fault_frame, sizeof(fault_frame), &bytesWritten, NULL);

        for (auto& frame_ptr: raw_trace.frames) {
            cpptrace::safe_object_frame safe_frame;
            cpptrace::get_safe_object_frame(frame_ptr, &safe_frame);
            WriteFile(hFile, &safe_frame, sizeof(safe_frame), &bytesWritten, NULL);
        }
        CloseHandle(hFile);
    }

    cpptrace::generate_trace().print();

    ExitProcess(EXIT_FAILURE);

    return EXCEPTION_EXECUTE_HANDLER; // This line will never be reached, but it's here to satisfy the compiler.
}

// No-op on Windows: alternate signal stacks are a POSIX concept.
inline void setup_alternate_stack() {}

static void register_crash_signals() {
    cpptrace::register_terminate_handler();
    SetUnhandledExceptionFilter(safe_crash_handler);
}



static void check_and_print_previous_crash() {
    std::FILE* f = std::fopen("trace.bin", "rb");
    if (!f) return;

    auto logger = spdlog::get("rhx_trace");
    if (!logger) {
        try {
            logger = spdlog::basic_logger_mt("rhx_trace", "rhx_trace.log");
            logger->flush_on(spdlog::level::warn);
        } catch (const spdlog::spdlog_ex&) {
            logger = spdlog::default_logger();
        }
    }

    logger->warn("===========================================");
    logger->warn("Previous crash detected. Generating stack trace...");
    logger->warn("===========================================");

    // Read the faulting safe_object_frame written first by the crash handler.
    cpptrace::safe_object_frame fault_safe_frame{};
    std::fread(&fault_safe_frame, sizeof(fault_safe_frame), 1, f);

    cpptrace::object_trace obj_trace;
    cpptrace::safe_object_frame safe_frame;
    size_t frame_count = 0;

    while (std::fread(&safe_frame, sizeof(safe_frame), 1, f) == 1) {
        obj_trace.frames.push_back(safe_frame.resolve());
        frame_count++;
    }

    std::fclose(f);

    std::remove("trace.bin");

    if (frame_count > 0) {
        logger->info("Successfully read {} frames from dump file.", frame_count);

        try {
            // Resolve the faulting frame to get the exact crash location.
            {
                cpptrace::object_trace fault_obj;
                fault_obj.frames.push_back(fault_safe_frame.resolve());
                auto fault_trace = fault_obj.resolve();
                if (!fault_trace.frames.empty()) {
                    auto& frame = fault_trace.frames.front();
                    logger->error("Crash Happened at: {}:{}", frame.filename, frame.line.value_or(0));
                }
            }

            cpptrace::stacktrace resolved_trace = obj_trace.resolve();
            logger->error("Crash Stack Trace:\n{}", resolved_trace.to_string());
        } catch (const std::exception& e) {
            logger->error("Failed to resolve stack trace: {}", e.what());
        }
    } else {
        logger->error("Dump file was empty or could not be read!");
    }

    logger->flush();
}

void dereference_null_pointer() {
    volatile int* p = nullptr;
    *p = 0; // Trigger access violation
}

void throw_unhandled_exception() {
    throw std::runtime_error("This is an unhandled exception for testing purposes.");
}

void generate_crash() {
    //dereference_null_pointer(); // This will cause an access violation and trigger the crash handler
}