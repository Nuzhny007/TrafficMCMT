#pragma once

#include <cassert>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace tmcmt {

enum class ErrorCode : std::uint8_t {
    Ok = 0,
    InvalidArgument,
    NotFound,
    IoError,
    ParseError,
    Unsupported,
    Timeout,
    Internal,
};

const char* ToString(ErrorCode code) noexcept;

/// Lightweight error object. Used instead of exceptions on the hot path.
class Status {
public:
    Status() = default;

    Status(ErrorCode code, std::string message) : m_code(code), m_message(std::move(message)) {}

    [[nodiscard]] static Status Ok() noexcept {
        return {};
    }

    [[nodiscard]] static Status Error(ErrorCode code, std::string message) {
        return Status(code, std::move(message));
    }

    [[nodiscard]] bool ok() const noexcept {
        return m_code == ErrorCode::Ok;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return ok();
    }

    [[nodiscard]] ErrorCode code() const noexcept {
        return m_code;
    }

    [[nodiscard]] const std::string& message() const noexcept {
        return m_message;
    }

    [[nodiscard]] std::string ToString() const;

private:
    ErrorCode m_code = ErrorCode::Ok;
    std::string m_message;
};

/// Result of an operation that produces a value: either T or Status.
template <typename T>
class Result {
public:
    Result(T value) : m_value(std::move(value)) {}

    Result(Status status) : m_status(std::move(status)) {
        assert(!m_status.ok() && "Result constructed from a successful Status");
    }

    [[nodiscard]] bool has_value() const noexcept {
        return m_value.has_value();
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return has_value();
    }

    [[nodiscard]] T& value() & {
        assert(m_value.has_value());
        return *m_value;
    }

    [[nodiscard]] const T& value() const& {
        assert(m_value.has_value());
        return *m_value;
    }

    [[nodiscard]] T&& value() && {
        assert(m_value.has_value());
        return std::move(*m_value);
    }

    [[nodiscard]] T value_or(T fallback) const {
        return m_value ? *m_value : std::move(fallback);
    }

    [[nodiscard]] const Status& status() const noexcept {
        return m_status;
    }

private:
    std::optional<T> m_value;
    Status m_status;
};

} // namespace tmcmt
