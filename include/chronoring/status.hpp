#pragma once

#include <atomic>
#include <cstdint>
#include <new>
#include <string_view>
#include <type_traits>
#include <utility>

#define CHRONORING_VERSION_MAJOR 1
#define CHRONORING_VERSION_MINOR 1
#define CHRONORING_VERSION_PATCH 0
#define CHRONORING_VERSION_STRING "1.1.0"

namespace chronoring {

[[nodiscard]] constexpr std::string_view version() noexcept {
    return CHRONORING_VERSION_STRING;
}

enum class RingStatus : std::uint8_t {
    Ok = 0,
    Full,
    Empty,
    Closed,
    InvalidArgument,
    MappingError,
    IoError,
    CorruptedPage,
    Timeout
};

[[nodiscard]] constexpr std::string_view status_string(RingStatus s) noexcept {
    switch (s) {
    case RingStatus::Ok:
        return "Ok";
    case RingStatus::Full:
        return "Full";
    case RingStatus::Empty:
        return "Empty";
    case RingStatus::Closed:
        return "Closed";
    case RingStatus::InvalidArgument:
        return "InvalidArgument";
    case RingStatus::MappingError:
        return "MappingError";
    case RingStatus::IoError:
        return "IoError";
    case RingStatus::CorruptedPage:
        return "CorruptedPage";
    case RingStatus::Timeout:
        return "Timeout";
    default:
        return "Unknown";
    }
}

template <typename E>
struct Unexpected {
    E error;
};

template <typename E>
[[nodiscard]] constexpr Unexpected<std::decay_t<E>> unexpected(E&& e) noexcept(
    std::is_nothrow_constructible_v<std::decay_t<E>, E&&>) {
    return Unexpected<std::decay_t<E>>{std::forward<E>(e)};
}

template <typename T, typename E = RingStatus>
class Expected {
public:
    using value_type = T;
    using error_type = E;

    constexpr Expected(const T& val) noexcept(std::is_nothrow_copy_constructible_v<T>)
        : has_val_(true) {
        std::construct_at(&storage_.val, val);
    }

    constexpr Expected(T&& val) noexcept(std::is_nothrow_move_constructible_v<T>)
        : has_val_(true) {
        std::construct_at(&storage_.val, std::move(val));
    }

    constexpr Expected(const Unexpected<E>& err) noexcept(
        std::is_nothrow_copy_constructible_v<E>)
        : has_val_(false) {
        std::construct_at(&storage_.err, err.error);
    }

    constexpr Expected(Unexpected<E>&& err) noexcept(
        std::is_nothrow_move_constructible_v<E>)
        : has_val_(false) {
        std::construct_at(&storage_.err, std::move(err.error));
    }

    template <typename Err = E>
        requires (!std::is_same_v<std::decay_t<T>, std::decay_t<Err>> &&
                  std::is_constructible_v<E, Err&&>)
    constexpr Expected(Err&& err) noexcept(std::is_nothrow_constructible_v<E, Err&&>)
        : has_val_(false) {
        std::construct_at(&storage_.err, std::forward<Err>(err));
    }

    constexpr Expected(const Expected& other) noexcept(
        std::is_nothrow_copy_constructible_v<T> && std::is_nothrow_copy_constructible_v<E>)
        : has_val_(other.has_val_) {
        if (has_val_) {
            std::construct_at(&storage_.val, other.storage_.val);
        } else {
            std::construct_at(&storage_.err, other.storage_.err);
        }
    }

    constexpr Expected(Expected&& other) noexcept(
        std::is_nothrow_move_constructible_v<T> && std::is_nothrow_move_constructible_v<E>)
        : has_val_(other.has_val_) {
        if (has_val_) {
            std::construct_at(&storage_.val, std::move(other.storage_.val));
        } else {
            std::construct_at(&storage_.err, std::move(other.storage_.err));
        }
    }

    constexpr Expected& operator=(const Expected& other) {
        if (this == &other) {
            return *this;
        }
        destroy();
        has_val_ = other.has_val_;
        if (has_val_) {
            std::construct_at(&storage_.val, other.storage_.val);
        } else {
            std::construct_at(&storage_.err, other.storage_.err);
        }
        return *this;
    }

    constexpr Expected& operator=(Expected&& other) noexcept(
        std::is_nothrow_move_constructible_v<T> && std::is_nothrow_move_constructible_v<E>) {
        if (this == &other) {
            return *this;
        }
        destroy();
        has_val_ = other.has_val_;
        if (has_val_) {
            std::construct_at(&storage_.val, std::move(other.storage_.val));
        } else {
            std::construct_at(&storage_.err, std::move(other.storage_.err));
        }
        return *this;
    }

    constexpr ~Expected() { destroy(); }

    [[nodiscard]] constexpr bool has_value() const noexcept { return has_val_; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return has_val_; }

    [[nodiscard]] constexpr T& value() & noexcept { return storage_.val; }
    [[nodiscard]] constexpr const T& value() const& noexcept { return storage_.val; }
    [[nodiscard]] constexpr T&& value() && noexcept { return std::move(storage_.val); }

    [[nodiscard]] constexpr E& error() & noexcept { return storage_.err; }
    [[nodiscard]] constexpr const E& error() const& noexcept { return storage_.err; }

    template <typename U>
    [[nodiscard]] constexpr T value_or(U&& fallback) const& {
        return has_val_ ? storage_.val : static_cast<T>(std::forward<U>(fallback));
    }

    template <typename U>
    [[nodiscard]] constexpr T value_or(U&& fallback) && {
        return has_val_ ? std::move(storage_.val)
                        : static_cast<T>(std::forward<U>(fallback));
    }

    [[nodiscard]] constexpr T* operator->() noexcept { return &storage_.val; }
    [[nodiscard]] constexpr const T* operator->() const noexcept { return &storage_.val; }
    [[nodiscard]] constexpr T& operator*() & noexcept { return storage_.val; }
    [[nodiscard]] constexpr const T& operator*() const& noexcept { return storage_.val; }

private:
    constexpr void destroy() noexcept {
        if (has_val_) {
            if constexpr (!std::is_trivially_destructible_v<T>) {
                std::destroy_at(&storage_.val);
            }
        } else {
            if constexpr (!std::is_trivially_destructible_v<E>) {
                std::destroy_at(&storage_.err);
            }
        }
    }

    union Storage {
        constexpr Storage() noexcept {}
        constexpr ~Storage() noexcept {}
        T val;
        E err;
    } storage_;
    bool has_val_;
};

template <typename E>
class Expected<void, E> {
public:
    using value_type = void;
    using error_type = E;

    constexpr Expected() noexcept : has_val_(true), err_{} {}
    constexpr Expected(const Unexpected<E>& err) noexcept : has_val_(false), err_(err.error) {}
    constexpr Expected(Unexpected<E>&& err) noexcept
        : has_val_(false), err_(std::move(err.error)) {}
    constexpr Expected(E err) noexcept : has_val_(false), err_(std::move(err)) {}

    [[nodiscard]] constexpr bool has_value() const noexcept { return has_val_; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return has_val_; }
    [[nodiscard]] constexpr const E& error() const& noexcept { return err_; }

private:
    bool has_val_;
    E err_;
};

enum class LogLevel : std::uint8_t {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4
};

[[nodiscard]] constexpr std::string_view log_level_string(LogLevel lvl) noexcept {
    switch (lvl) {
    case LogLevel::Trace:
        return "TRACE";
    case LogLevel::Debug:
        return "DEBUG";
    case LogLevel::Info:
        return "INFO";
    case LogLevel::Warn:
        return "WARN";
    case LogLevel::Error:
        return "ERROR";
    default:
        return "UNKNOWN";
    }
}

struct LogEvent {
    LogLevel level{LogLevel::Info};
    RingStatus status{RingStatus::Ok};
    std::string_view component{};
    std::string_view message{};
    std::uint64_t metric_value{0};
};

using LogCallback = void (*)(const LogEvent& event, void* user_data) noexcept;

namespace detail {

struct LoggerRegistry {
    inline static std::atomic<LogCallback> callback{nullptr};
    inline static std::atomic<void*> user_data{nullptr};
};

} // namespace detail

inline void set_logger(LogCallback cb, void* user_data = nullptr) noexcept {
    detail::LoggerRegistry::callback.store(nullptr, std::memory_order_release);
    detail::LoggerRegistry::user_data.store(user_data, std::memory_order_release);
    detail::LoggerRegistry::callback.store(cb, std::memory_order_release);
}

inline void emit_log(LogLevel level, RingStatus status, std::string_view component,
                     std::string_view message, std::uint64_t metric_value = 0) noexcept {
    const LogCallback cb =
        detail::LoggerRegistry::callback.load(std::memory_order_acquire);
    if (cb != nullptr) {
        void* const ctx =
            detail::LoggerRegistry::user_data.load(std::memory_order_acquire);
        const LogEvent evt{level, status, component, message, metric_value};
        cb(evt, ctx);
    }
}

} // namespace chronoring
