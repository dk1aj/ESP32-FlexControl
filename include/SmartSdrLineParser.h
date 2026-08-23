#pragma once

#include <stddef.h>

template <size_t BufferSize>
class SmartSdrLineParser
{
    static_assert(BufferSize >= 2, "Line buffer must hold data and a terminator");

public:
    enum class Result
    {
        None,
        LineReady,
        Overflow
    };

    Result push(const char character)
    {
        if (discarding_)
        {
            if (isDelimiter(character))
            {
                discarding_ = false;
            }
            return Result::None;
        }

        if (isDelimiter(character))
        {
            if (length_ == 0)
            {
                return Result::None;
            }
            buffer_[length_] = '\0';
            length_ = 0;
            return Result::LineReady;
        }

        if (length_ + 1 < BufferSize)
        {
            buffer_[length_++] = character;
            return Result::None;
        }

        length_ = 0;
        discarding_ = true;
        return Result::Overflow;
    }

    void reset()
    {
        length_ = 0;
        discarding_ = false;
    }

    char *line()
    {
        return buffer_;
    }

    static constexpr size_t maxLineLength()
    {
        return BufferSize - 1;
    }

private:
    static bool isDelimiter(const char character)
    {
        return character == '\r' || character == '\n';
    }

    char buffer_[BufferSize] = {};
    size_t length_ = 0;
    bool discarding_ = false;
};
