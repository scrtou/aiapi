#pragma once

#include <platform/result/Error.h>

#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

namespace platform {

/**
 * Explicit success-or-failure boundary value.
 *
 * Value is wrapped before it enters the variant so Result<Error> remains
 * unambiguous and move-only values remain supported.
 */
template <typename T>
class [[nodiscard]] Result
{
    static_assert(!std::is_reference<T>::value,
                  "Result<T> stores a value, not a reference");

  public:
    static Result success(T value)
    {
        return Result(Value{std::move(value)});
    }

    static Result failure(Error error)
    {
        return Result(std::move(error));
    }

    [[nodiscard]] bool ok() const noexcept
    {
        return std::holds_alternative<Value>(data_);
    }

    explicit operator bool() const noexcept { return ok(); }

    T& value() &
    {
        ensureValue();
        return std::get<Value>(data_).value;
    }

    const T& value() const &
    {
        ensureValue();
        return std::get<Value>(data_).value;
    }

    T&& value() &&
    {
        ensureValue();
        return std::move(std::get<Value>(data_).value);
    }

    Error& error() &
    {
        ensureError();
        return std::get<Error>(data_);
    }

    const Error& error() const &
    {
        ensureError();
        return std::get<Error>(data_);
    }

    Error&& error() &&
    {
        ensureError();
        return std::move(std::get<Error>(data_));
    }

  private:
    struct Value {
        T value;
    };

    explicit Result(Value value) : data_(std::move(value)) {}
    explicit Result(Error error) : data_(std::move(error)) {}

    void ensureValue() const
    {
        if (!ok()) {
            throw std::logic_error("Result::value() called on failure");
        }
    }

    void ensureError() const
    {
        if (ok()) {
            throw std::logic_error("Result::error() called on success");
        }
    }

    std::variant<Value, Error> data_;
};

template <>
class [[nodiscard]] Result<void>
{
  public:
    static Result success() { return Result(); }

    static Result failure(Error error)
    {
        Result result;
        result.error_ = std::move(error);
        return result;
    }

    [[nodiscard]] bool ok() const noexcept { return !error_.has_value(); }
    explicit operator bool() const noexcept { return ok(); }

    void value() const
    {
        if (!ok()) {
            throw std::logic_error("Result<void>::value() called on failure");
        }
    }

    const Error& error() const
    {
        if (ok()) {
            throw std::logic_error("Result<void>::error() called on success");
        }
        return *error_;
    }

  private:
    std::optional<Error> error_;
};

}  // namespace platform
