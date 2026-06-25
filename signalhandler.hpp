#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <fstream>
#include <filesystem>
#include <cpptrace/cpptrace.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>

const char* DUMP_FILE = "./crash_raw.dump";

void safe_crash_handler(int signum) {
    const size_t max_frames = 100;
    cpptrace::frame_ptr buffer[max_frames];

    size_t count = cpptrace::safe_generate_raw_trace(buffer, max_frames);

    int fd = ::open(DUMP_FILE, O_CREAT | O_WRONLY | O_TRUNC, 0666);
    if (fd != -1) {
        ::write(fd, buffer, count * sizeof(cpptrace::frame_ptr));
        ::close(fd);
    }

    ::_Exit(1);
}

void setup_alternate_stack() {
    stack_t ss;

    ss.ss_sp = malloc(SIGSTKSZ*2);
    if (ss.ss_sp == nullptr) {
        perror("malloc for alternate stack failed");
        exit(EXIT_FAILURE);
    }
    ss.ss_size = SIGSTKSZ*2;
    ss.ss_flags = 0;

    if (sigaltstack(&ss, nullptr) == -1) {
        perror("sigaltstack failed");
        exit(EXIT_FAILURE);
    }
}

void register_crash_signals() {
    struct sigaction sa;
    sa.sa_handler = safe_crash_handler;
    sigemptyset(&sa.sa_mask);

    sa.sa_flags = SA_ONSTACK; // Use alternate stack for signal handler

    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
    sigaction(SIGFPE, &sa, nullptr);
    sigaction(SIGILL, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
}

void check_and_print_previous_crash() {
    int fd = ::open(DUMP_FILE, O_RDONLY);
    if (fd != -1) {
        std::cout <<"\n===========================================\n";
        std::cout << "⚠️WARNING: Previous crash detected. Generating stack trace...\n";
        std::cout <<"===========================================\n";

        std::vector<cpptrace::frame_ptr> buffer(100);
        ssize_t bytes_read = ::read(fd, buffer.data(), buffer.size() * sizeof(cpptrace::frame_ptr));
        ::close(fd);

        size_t count = bytes_read / sizeof(cpptrace::frame_ptr);
        if (count > 0) {
            cpptrace::raw_trace trace;
            trace.frames.assign(buffer.begin(), buffer.begin() + count);
            trace.resolve().print();
        }
        
        ::unlink(DUMP_FILE);
    }

}

void infinite_recursion() {
    volatile char heavy_payload[10000];
    heavy_payload[0] = 'a'; // Prevent optimization
    infinite_recursion();
}