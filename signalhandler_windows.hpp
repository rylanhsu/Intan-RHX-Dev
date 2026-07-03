#pragma once

#define WIN32_LEAN_AND_MEAN
#include <cstdlib>

#include <fstream>
#include <filesystem>
#include <string>
#include <vector>
#include <stdexcept>

#include <windows.h>
#include <DbgHelp.h>
#include <iostream>

#include <cpptrace/cpptrace.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>

#pragma comment(lib, "Dbghelp.lib")

// ===============================================================
// Path Management
// ===============================================================
inline std::filesystem::path get_app_data_dir() {
    const char* userprofile = std::getenv("USERPROFILE");
    std::filesystem::path dir = userprofile
        ? std::filesystem::path(userprofile) / "Documents" / "XDAQ-RHX"
        : std::filesystem::path("XDAQ-RHX");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

// Absolute paths, computed once at startup so the crash handler does not have to
// build them (and so they survive a read-only working directory).
inline const std::string& trace_bin_path() {
    static const std::string path = (get_app_data_dir() / "trace.bin").string();
    return path;
}

inline const std::string& trace_log_path() {
    static const std::string path = (get_app_data_dir() / "rhx_trace.log").string();
    return path;
}

inline const std::string& dump_path() {
    static const std::string path = (get_app_data_dir() / "crash_report.dmp").string();
    return path;
}

// ================================================================
// Async-signal-safe-ish on Windows
// ================================================================

inline void safe_print_err(const char* msg) {
    DWORD bytesWritten;
    WriteFile(
        GetStdHandle(STD_ERROR_HANDLE),
        msg,
        lstrlenA(msg),
        &bytesWritten,
        NULL
    );
}

inline void safe_print_err_code(const char* prefix, DWORD code) {
    char buffer[128];
    std::snprintf(buffer, sizeof(buffer), "%s: %lu\n", prefix, code);
    safe_print_err(buffer);

}

// ================================================================
// Crash Handler
// ================================================================

// Write raw stack frames to the dump file using only async-signal-safe Win32 calls.
inline LONG WINAPI safe_crash_handler(EXCEPTION_POINTERS* ExceptionInfo) {
    safe_print_err("\n!!! [System Crash] Detect fatal signal!!!\n");

    DWORD excCode = ExceptionInfo->ExceptionRecord->ExceptionCode;

    if (excCode == EXCEPTION_ACCESS_VIOLATION) {
        safe_print_err("Reason: EXCEPTION_ACCESS_VIOLATION (Segmentation Fault / Null Pointer)\n)");
    } else if (excCode == EXCEPTION_FLT_DIVIDE_BY_ZERO || excCode == EXCEPTION_INT_DIVIDE_BY_ZERO) {
        safe_print_err("Reason: DIVIDE_BY_ZERO (Floating Point / Integer Exception)\n");
    } else if (excCode == EXCEPTION_ILLEGAL_INSTRUCTION) {
        safe_print_err("Reason: EXCEPTION_ILLEGAL_INSTRUCTION (Illegal Instruction)\n");
    } else if (excCode == EXCEPTION_STACK_OVERFLOW) {
        safe_print_err("Reason: EXCEPTION_STACK_OVERFLOW (Stack Overflow)\n");
    } else {
        safe_print_err("Reason: Unknown Exception Code\n");
    }

    safe_print_err("Generating stack trace...\n\n");
    auto raw_trace = cpptrace::generate_raw_trace();

    // ================================================================
    // First Step: Safely Create minidump (.dmp) first
    // ================================================================

    HANDLE hDumpFile = CreateFileA(
        dump_path().c_str(),
        GENERIC_WRITE,
        0,
        NULL,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    if(hDumpFile != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION dumpInfo;
        dumpInfo.ThreadId = GetCurrentThreadId();
        dumpInfo.ExceptionPointers = ExceptionInfo;
        dumpInfo.ClientPointers = FALSE;

        BOOL success = MiniDumpWriteDump(
            GetCurrentProcess(),
            GetCurrentProcessId(),
            hDumpFile,
            MiniDumpNormal,
            &dumpInfo,
            NULL,
            NULL
        );

        if (success) {
            safe_print_err("\n!!! [Success] Created minidump file successfully: crash_report.dmp\n");
        } else {
            DWORD errorCode = GetLastError();
            safe_print_err_code("\n!!! [Error] Failed to create minidump file, Error Code", errorCode);
        }
        CloseHandle(hDumpFile);
    }

    // ================================================================
    // Second Step: Safely Create binary stack trace for cpptrace (.bin)
    // ================================================================
    HANDLE hTraceFile = CreateFileA(
        trace_bin_path().c_str(),
        GENERIC_WRITE,
        0,
        NULL,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    if (hTraceFile != INVALID_HANDLE_VALUE) {
        DWORD bytesWritten;

        // Write the faulting address as a safe_object_frame (object-relative) so it
        // survives ASLR between runs, just like the other frames.
        cpptrace::safe_object_frame fault_frame{};
        cpptrace::get_safe_object_frame(
            reinterpret_cast<cpptrace::frame_ptr>(ExceptionInfo->ExceptionRecord->ExceptionAddress),
            &fault_frame
        );
        WriteFile(hTraceFile, &fault_frame, sizeof(fault_frame), &bytesWritten, NULL);

        for (auto& frame_ptr: raw_trace.frames) {
            cpptrace::safe_object_frame safe_frame;
            cpptrace::get_safe_object_frame(frame_ptr, &safe_frame);
            WriteFile(hTraceFile, &safe_frame, sizeof(safe_frame), &bytesWritten, NULL);
        }
        CloseHandle(hTraceFile);
    }

    // ================================================================
    // Third Step: Generate text stack trace by cpptrace (write to Log)
    // ================================================================ 
    safe_print_err("Analysis stack trace... \n");
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

inline void get_previous_dump_content(std::shared_ptr<spdlog::logger> logger);
inline void print_previous_cpptrace_crash(std::shared_ptr<spdlog::logger> logger);

inline void check_and_print_previous_crash() {
    auto logger = spdlog::get("rhx_trace");
    if (!logger) {
        try {
            logger = spdlog::basic_logger_mt("rhx_trace", trace_log_path());
            logger->flush_on(spdlog::level::warn);
        } catch (const spdlog::spdlog_ex&) {
            logger = spdlog::default_logger();
        }
    }

    print_previous_cpptrace_crash(logger);

    get_previous_dump_content(logger);
}

inline void print_previous_cpptrace_crash(std::shared_ptr<spdlog::logger> logger) {
    if (!std::filesystem::exists(trace_bin_path())) {
        return;
    }

    std::FILE* f = std::fopen(trace_bin_path().c_str(), "rb");
    if (!f) return;

    logger->warn("\n===========================================");
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
    std::filesystem::remove(trace_bin_path().c_str());

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

static void get_previous_dump_content(std::shared_ptr<spdlog::logger> logger) {
    std::string d_path = dump_path();

    // Step 1
    if (!std::filesystem::exists(d_path)) {
        return;
    }

    logger->warn("\n===========================================");
    logger->warn("Previous crash dump detected. Generating stack trace from minidump...");


    // Step 2 
    HANDLE hMapFile = CreateFileA(
        d_path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL);

    if (hMapFile != INVALID_HANDLE_VALUE){
        HANDLE hMap = CreateFileMappingA(hMapFile, NULL, PAGE_READONLY, 0, 0, NULL);
        if (hMap != NULL) {
            void* pView = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
            if(pView != NULL) {
                MINIDUMP_DIRECTORY* pDir = nullptr;
                void* pStream = nullptr;
                ULONG streamSize = 0;

                if (MiniDumpReadDumpStream(pView, ExceptionStream, &pDir, &pStream, &streamSize)) {
                    auto* pExceptionData = static_cast<MINIDUMP_EXCEPTION_STREAM*>(pStream);
                    DWORD excCode = pExceptionData->ExceptionRecord.ExceptionCode;
                    logger->error("Parse Dump to Exception Code: {}\n", excCode);
                }
                UnmapViewOfFile(pView);
            }
            CloseHandle(hMap);
        }
        CloseHandle(hMapFile);
    }


    // Step 3 
    logger->info("Prepare upload crash dump to server: {}", dump_path());
    // TODO: upload_to_server(dump_path);

    // Step 4 
    std::error_code ec;
    std::filesystem::remove(d_path, ec);

    logger->info("Previous crash minidump has been processed and removed.");
    logger->warn("===========================================\n");
}

// ================================================================
// Testing
// ================================================================
void dereference_null_pointer() {
    volatile int* p = nullptr;
    *p = 0; // Trigger access violation
}

void throw_unhandled_exception() {
    throw std::runtime_error("This is an unhandled exception for testing purposes.");
}

void generate_crash() {
    dereference_null_pointer(); 
}