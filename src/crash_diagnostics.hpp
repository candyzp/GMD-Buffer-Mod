#pragma once

#include <Geode/Geode.hpp>
#include <array>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>
#include <csignal>
#include <cstdlib>

#if defined(__APPLE__) || defined(__unix__)
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

// Crash diagnostics for iOS (and a persistent trace on desktop).
// Normal logging uses a fixed-size in-memory ring. The ring is periodically
// flushed during gameplay; fatal POSIX signals dump it using write() only,
// on an alternate signal stack, before chaining to the previous handler.
namespace crashdiag {
    inline constexpr size_t EVENT_CAPACITY = 256;
    inline constexpr size_t EVENT_SIZE = 224;
    inline constexpr size_t ACTIVE_SIZE = 112;

    struct Event {
        char text[EVENT_SIZE]{};
        volatile sig_atomic_t length = 0;
    };

    inline Event g_events[EVENT_CAPACITY]{};
    inline volatile sig_atomic_t g_sequence = 0;
    inline volatile sig_atomic_t g_lastFlushed = 0;
    inline volatile sig_atomic_t g_inFatalHandler = 0;
    inline const char* volatile g_activeHook = "startup / hooks not entered";
    inline std::filesystem::path g_currentPath;
    inline std::filesystem::path g_previousPath;
    inline bool g_installed = false;

#if defined(__APPLE__) || defined(__unix__)
    inline int g_fd = -1;
    inline constexpr int FATAL_SIGNALS[] = { SIGSEGV, SIGBUS, SIGABRT, SIGILL, SIGFPE };
    inline struct sigaction g_previousActions[sizeof(FATAL_SIGNALS) / sizeof(FATAL_SIGNALS[0])]{};
    inline bool g_signalInstalled[sizeof(FATAL_SIGNALS) / sizeof(FATAL_SIGNALS[0])]{};
    alignas(16) inline unsigned char g_alternateStack[64 * 1024]{};
#else
    inline std::ofstream g_stream;
#endif

    inline void copyText(char* dest, size_t capacity, const char* source) {
        if (!dest || capacity == 0) return;
        if (!source) source = "<null>";
        size_t i = 0;
        for (; i + 1 < capacity && source[i]; ++i) dest[i] = source[i];
        dest[i] = '\0';
    }

    inline size_t boundedLength(const char* text, size_t limit) {
        size_t n = 0;
        while (n < limit && text[n]) ++n;
        return n;
    }

#if defined(__APPLE__) || defined(__unix__)
    inline void rawWrite(const char* data, size_t length) {
        if (g_fd < 0 || !data) return;
        while (length) {
            auto n = ::write(g_fd, data, length);
            if (n <= 0) return;
            data += n;
            length -= static_cast<size_t>(n);
        }
    }

    inline void rawWriteString(const char* text, size_t limit) {
        rawWrite(text, boundedLength(text, limit));
    }

    inline void rawWriteSigned(long long value) {
        char out[32];
        size_t n = 0;
        bool negative = value < 0;
        unsigned long long magnitude = negative
            ? static_cast<unsigned long long>(-(value + 1)) + 1ULL
            : static_cast<unsigned long long>(value);
        do {
            out[n++] = static_cast<char>('0' + (magnitude % 10ULL));
            magnitude /= 10ULL;
        } while (magnitude && n < sizeof(out) - 1);
        if (negative && n < sizeof(out)) out[n++] = '-';
        while (n) rawWrite(&out[--n], 1);
    }

    inline void rawWriteHex(uintptr_t value) {
        static constexpr char hex[] = "0123456789abcdef";
        char out[2 * sizeof(uintptr_t)];
        size_t n = 0;
        do {
            out[n++] = hex[value & 0xf];
            value >>= 4;
        } while (value && n < sizeof(out));
        rawWrite("0x", 2);
        while (n) rawWrite(&out[--n], 1);
    }

    inline void dumpRingFromSignal() {
        sig_atomic_t newest = g_sequence;
        sig_atomic_t first = newest > static_cast<sig_atomic_t>(EVENT_CAPACITY)
            ? newest - static_cast<sig_atomic_t>(EVENT_CAPACITY)
            : 0;
        rawWrite("\n--- RECENT EVENT RING (oldest to newest) ---\n", 46);
        for (sig_atomic_t seq = first + 1; seq <= newest; ++seq) {
            auto& event = g_events[static_cast<size_t>(seq) % EVENT_CAPACITY];
            sig_atomic_t len = event.length;
            if (len > 0 && len <= static_cast<sig_atomic_t>(EVENT_SIZE)) {
                rawWrite(event.text, static_cast<size_t>(len));
            }
        }
        rawWrite("--- END EVENT RING ---\n", 23);
    }

    inline void fatalSignalHandler(int signalNumber, siginfo_t* info, void* context) {
        if (g_inFatalHandler) {
            static constexpr char recursive[] = "\nFATAL HANDLER RE-ENTERED; exiting immediately.\n";
            rawWrite(recursive, sizeof(recursive) - 1);
            ::_exit(128 + signalNumber);
        }
        g_inFatalHandler = 1;

        rawWrite("\n\n========== INPUT BUFFER FATAL SIGNAL ==========\n", 50);
        rawWrite("signal=", 7);
        rawWriteSigned(signalNumber);
        rawWrite(" code=", 6);
        rawWriteSigned(info ? info->si_code : 0);
        rawWrite(" fault_address=", 15);
        rawWriteHex(reinterpret_cast<uintptr_t>(info ? info->si_addr : nullptr));
        rawWrite("\nactive_hook=", 13);
        rawWriteString(g_activeHook, sizeof(g_activeHook));
        rawWrite("\npid=", 5);
        rawWriteSigned(static_cast<long long>(::getpid()));
        rawWrite("\n", 1);
        dumpRingFromSignal();
        rawWrite("========== CHAINING TO PREVIOUS CRASH HANDLER ==========\n", 57);

        for (size_t i = 0; i < sizeof(FATAL_SIGNALS) / sizeof(FATAL_SIGNALS[0]); ++i) {
            if (FATAL_SIGNALS[i] != signalNumber || !g_signalInstalled[i]) continue;
            auto previous = g_previousActions[i];
            // Preserve the system's prior handler so iOS can still produce its .ips report.
            if (previous.sa_handler != SIG_DFL && previous.sa_handler != SIG_IGN && previous.sa_handler != nullptr) {
                if (previous.sa_flags & SA_SIGINFO) previous.sa_sigaction(signalNumber, info, context);
                else previous.sa_handler(signalNumber);
            }
            break;
        }

        // A returning prior handler is unusual for these fatal signals. Restore the
        // default disposition and re-deliver the signal so the process still crashes.
        struct sigaction defaultAction{};
        defaultAction.sa_handler = SIG_DFL;
        sigemptyset(&defaultAction.sa_mask);
        ::sigaction(signalNumber, &defaultAction, nullptr);
        sigset_t unblock;
        sigemptyset(&unblock);
        sigaddset(&unblock, signalNumber);
        ::sigprocmask(SIG_UNBLOCK, &unblock, nullptr);
        ::kill(::getpid(), signalNumber);
        ::_exit(128 + signalNumber);
    }

    inline void installFatalHandlers() {
        stack_t stack{};
        stack.ss_sp = g_alternateStack;
        stack.ss_size = sizeof(g_alternateStack);
        stack.ss_flags = 0;
        if (::sigaltstack(&stack, nullptr) != 0) {
            // The persistent trace still works if this platform refuses an alternate stack.
        }
        for (size_t i = 0; i < sizeof(FATAL_SIGNALS) / sizeof(FATAL_SIGNALS[0]); ++i) {
            struct sigaction action{};
            action.sa_sigaction = &fatalSignalHandler;
            sigemptyset(&action.sa_mask);
            action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
            if (::sigaction(FATAL_SIGNALS[i], &action, &g_previousActions[i]) == 0) {
                g_signalInstalled[i] = true;
            }
        }
    }
#endif

    inline void record(const char* message) {
        sig_atomic_t next = g_sequence + 1;
        auto& event = g_events[static_cast<size_t>(next) % EVENT_CAPACITY];
        int written = std::snprintf(event.text, sizeof(event.text), "[%08d] %s\n",
                                    static_cast<int>(next), message ? message : "<null>");
        if (written < 0) {
            event.text[0] = '\0';
            event.length = 0;
            return;
        }
        size_t length = static_cast<size_t>(written);
        if (length >= sizeof(event.text)) length = sizeof(event.text) - 1;
        event.length = static_cast<sig_atomic_t>(length);
        g_sequence = next;
    }

    inline void recordf(const char* format, ...) {
        char message[EVENT_SIZE];
        va_list args;
        va_start(args, format);
        std::vsnprintf(message, sizeof(message), format ? format : "<null format>", args);
        va_end(args);
        record(message);
    }

    inline void setActive(const char* name) {
        g_activeHook = name ? name : "<null>";
    }

    // Store only the previous literal pointer, not a copied char buffer. This keeps
    // hot-hook stack overhead to one pointer while accurately restoring nested hook names.
    class ActiveScope {
    public:
        explicit ActiveScope(const char* name) : m_previous(g_activeHook) { setActive(name); }
        ActiveScope(const ActiveScope&) = delete;
        ActiveScope& operator=(const ActiveScope&) = delete;
        ~ActiveScope() { setActive(m_previous); }
    private:
        const char* m_previous;
    };

    // Use only for higher-level boundaries, where ENTER/RETURN events are valuable.
    // Keep the scope itself small because the bug being traced can be stack exhaustion.
    class HookScope {
    public:
        explicit HookScope(const char* name) : m_name(name), m_active(name) {
            recordf("HOOK_ENTER %s", m_name);
        }
        HookScope(const HookScope&) = delete;
        HookScope& operator=(const HookScope&) = delete;
        ~HookScope() {
            setActive(m_name);
            recordf("HOOK_RETURN %s", m_name);
        }
    private:
        const char* m_name;
        ActiveScope m_active;
    };

    inline void flush() {
#if defined(__APPLE__) || defined(__unix__)
        if (g_fd < 0) return;
        sig_atomic_t newest = g_sequence;
        sig_atomic_t first = g_lastFlushed + 1;
        sig_atomic_t earliestAvailable = newest > static_cast<sig_atomic_t>(EVENT_CAPACITY)
            ? newest - static_cast<sig_atomic_t>(EVENT_CAPACITY) + 1
            : 1;
        if (first < earliestAvailable) first = earliestAvailable;
        for (sig_atomic_t seq = first; seq <= newest; ++seq) {
            auto& event = g_events[static_cast<size_t>(seq) % EVENT_CAPACITY];
            sig_atomic_t len = event.length;
            if (len > 0 && len <= static_cast<sig_atomic_t>(EVENT_SIZE)) {
                rawWrite(event.text, static_cast<size_t>(len));
            }
        }
        g_lastFlushed = newest;
#else
        if (!g_stream.is_open()) return;
        sig_atomic_t newest = g_sequence;
        sig_atomic_t first = g_lastFlushed + 1;
        sig_atomic_t earliestAvailable = newest > static_cast<sig_atomic_t>(EVENT_CAPACITY)
            ? newest - static_cast<sig_atomic_t>(EVENT_CAPACITY) + 1
            : 1;
        if (first < earliestAvailable) first = earliestAvailable;
        for (sig_atomic_t seq = first; seq <= newest; ++seq) {
            auto& event = g_events[static_cast<size_t>(seq) % EVENT_CAPACITY];
            if (event.length > 0) g_stream.write(event.text, event.length);
        }
        g_stream.flush();
        g_lastFlushed = newest;
#endif
    }

    inline std::string readTail(const std::filesystem::path& path, size_t maximumBytes) {
        std::ifstream input(path, std::ios::binary);
        if (!input) return {};
        std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        if (content.size() > maximumBytes) {
            content.erase(0, content.size() - maximumBytes);
            content.insert(0, "[TRUNCATED: showing tail of diagnostic file]\n");
        }
        return content;
    }

    inline std::string exportText() {
        constexpr size_t EACH_FILE_LIMIT = 48 * 1024;
        std::string result;
        auto previous = readTail(g_previousPath, EACH_FILE_LIMIT);
        auto current = readTail(g_currentPath, EACH_FILE_LIMIT);
        if (!previous.empty()) {
            result += "===== PREVIOUS SESSION (often the crashed run) =====\n";
            result += previous;
            result += "\n";
        }
        if (!current.empty()) {
            result += "===== CURRENT SESSION =====\n";
            result += current;
        }
        if (result.empty()) result = "No Input Buffer diagnostic files were readable yet.\n";
        constexpr size_t TOTAL_LIMIT = 100 * 1024;
        if (result.size() > TOTAL_LIMIT) {
            result.erase(0, result.size() - TOTAL_LIMIT);
            result.insert(0, "[TRUNCATED: combined diagnostic export]\n");
        }
        return result;
    }

    inline void cleanExit() {
        record("PROCESS_ATEXIT: clean C/C++ exit path reached");
        flush();
    }

    inline void install() {
        if (g_installed) return;
        g_installed = true;

        auto directory = geode::Mod::get()->getSaveDir();
        std::error_code ec;
        std::filesystem::create_directories(directory, ec);
        g_currentPath = directory / "crash-diagnostics-current.log";
        g_previousPath = directory / "crash-diagnostics-previous.log";

        // Preserve the last process's log at next launch. If it crashed, the signal
        // handler will already have appended a fatal marker to this file.
        if (std::filesystem::exists(g_currentPath, ec)) {
            std::filesystem::remove(g_previousPath, ec);
            ec.clear();
            std::filesystem::rename(g_currentPath, g_previousPath, ec);
        }

#if defined(__APPLE__) || defined(__unix__)
        g_fd = ::open(g_currentPath.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
#else
        g_stream.open(g_currentPath, std::ios::out | std::ios::app | std::ios::binary);
#endif
        record("========== INPUT BUFFER DIAGNOSTICS START ==========");
#if defined(__APPLE__) || defined(__unix__)
        recordf("LOGGER pid=%lld os=POSIX altstack=%zuKiB", static_cast<long long>(::getpid()), sizeof(g_alternateStack) / 1024);
#else
        record("LOGGER os=Windows signal-hook disabled; persistent event trace enabled");
#endif
        recordf("MOD id=%s version=%s", geode::Mod::get()->getID().data(),
                geode::Mod::get()->getVersion().toVString().c_str());
        recordf("SAVE_DIR=%s", g_currentPath.parent_path().string().c_str());
        flush();
#if defined(__APPLE__) || defined(__unix__)
        if (g_fd >= 0) installFatalHandlers();
        else record("WARNING: crash log file could not be opened; signal records unavailable");
#endif
        std::atexit(&cleanExit);
        record("CRASH LOGGER READY; fatal signals will be logged then chained to the prior handler");
        flush();
    }
}
