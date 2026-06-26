#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <execinfo.h>
#include <fstream>
#include <filesystem>

#include <cpptrace/cpptrace.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>

#include <mach-o/dyld.h>


std::filesystem::path get_executable_path() {
    char buf[PATH_MAX];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) {
        return std::filesystem::path(buf).parent_path();
    } else {
        throw std::runtime_error("Unable to get executable path");
    }
}

const std::filesystem::path& get_dump_path() {
    static auto path = get_executable_path() / "crash_raw.dump";
    return path;
}

const std::filesystem::path& get_log_path() {
    static auto path = get_executable_path() / "rhx_trace.log";
    return path;
}

void safe_crash_handler(int signum, siginfo_t *info, void *ctx) {
    void* addrs[100];
    int count = backtrace(addrs, 100);

    // backtrace() can't cross a corrupted stack (e.g. stack overflow); fall back to
    // the crash PC/LR saved in the ucontext so we at least know where it died.
    if (count == 0 && ctx != nullptr) {
        ucontext_t* uc = static_cast<ucontext_t*>(ctx);
        addrs[0] = reinterpret_cast<void*>(uc->uc_mcontext->__ss.__pc);
        addrs[1] = reinterpret_cast<void*>(uc->uc_mcontext->__ss.__lr);
        count = 2;
    }

    // Prepend the main exe's ASLR slide so the reader can map these runtime
    // addresses back to file virtual addresses in any future run.
    intptr_t slide = _dyld_get_image_vmaddr_slide(0);

    int fd = ::open(get_dump_path().c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0666);
    if (fd != -1) {
        ::write(fd, &slide, sizeof(slide));
        ::write(fd, addrs, count * sizeof(void*));
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
    sa.sa_sigaction = safe_crash_handler;
    sigemptyset(&sa.sa_mask);

    sa.sa_flags = SA_ONSTACK | SA_SIGINFO; // Use alternate stack; pass siginfo_t to handler

    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
    sigaction(SIGFPE, &sa, nullptr);
    sigaction(SIGILL, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
}

void check_and_print_previous_crash() {
    int fd = ::open(get_dump_path().c_str(), O_RDONLY);
    if (fd != -1) {
        // Write the previous crash report to a dedicated log file (rhx_trace.log).
        auto logger = spdlog::get("rhx_trace");
        if (!logger) {
            try {
                logger = spdlog::basic_logger_mt("rhx_trace", get_log_path().c_str());
                logger->flush_on(spdlog::level::warn);
            } catch (const spdlog::spdlog_ex &) {
                logger = spdlog::default_logger(); // fall back to console if the file can't be opened
            }
        }

        logger->warn("===========================================");
        logger->warn("Previous crash detected. Generating stack trace...");
        logger->warn("===========================================");

        // Read the ASLR slide written by the crash handler, then the raw addresses.
        intptr_t old_slide = 0;
        ::read(fd, &old_slide, sizeof(old_slide));

        std::vector<void*> raw_addrs(100);
        ssize_t bytes_read = ::read(fd, raw_addrs.data(), raw_addrs.size() * sizeof(void*));
        ::close(fd);

        if (bytes_read > 0) {
            size_t count = bytes_read / sizeof(void*);
            logger->info("Successfully read {} frames from dump file.", count);

            try {
                // ASLR normalization: crash addresses are runtime VAs from the previous run.
                // The main executable on macOS arm64 always prefers 0x100000000 as its base;
                // the dyld shared cache lives at 0x180000000+. For each address:
                //   - If it's in the app's file range [0x100000000, 0x180000000), remap it to
                //     the current run's layout by swapping old_slide for current_slide.
                //   - Otherwise (system dylibs in the shared cache), pass as-is.
                intptr_t current_slide = _dyld_get_image_vmaddr_slide(0);
                const uintptr_t APP_BASE  = 0x0000000100000000ULL;
                const uintptr_t APP_LIMIT = 0x0000000180000000ULL;

                cpptrace::raw_trace raw;
                for (size_t i = 0; i < count; i++) {
                    uintptr_t a     = reinterpret_cast<uintptr_t>(raw_addrs[i]);
                    uintptr_t file_a = a - static_cast<uintptr_t>(old_slide);
                    if (file_a >= APP_BASE && file_a < APP_LIMIT) {
                        raw.frames.push_back(file_a + static_cast<uintptr_t>(current_slide));
                    } else {
                        raw.frames.push_back(a);
                    }
                }

                auto resolved_trace = raw.resolve();
                logger->error("Stack trace from previous crash:\n{}", resolved_trace.to_string(false));
            }
            catch (const std::exception& e){
                logger->error("Failed to resolve stack trace: {}", e.what());
            }
        } else {
            logger->error("Dump file was empty or could not be read! (bytes_read = {})", bytes_read);
        }

        logger->flush();
        ::unlink(get_dump_path().c_str());
    }

}

void infinite_recursion() {
    volatile char heavy_payload[10000];
    heavy_payload[0] = 'a'; // Prevent optimization
    infinite_recursion();
}