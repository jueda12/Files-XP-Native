#pragma once

#include <cwctype>
#include <string_view>

namespace filesxp::core
{
    [[nodiscard]] inline int compare_natural(std::wstring_view left, std::wstring_view right) noexcept
    {
        std::size_t leftIndex = 0;
        std::size_t rightIndex = 0;

        while (leftIndex < left.size() && rightIndex < right.size())
        {
            const auto leftChar = left[leftIndex];
            const auto rightChar = right[rightIndex];
            if (std::iswdigit(leftChar) && std::iswdigit(rightChar))
            {
                auto leftZeroEnd = leftIndex;
                auto rightZeroEnd = rightIndex;
                while (leftZeroEnd < left.size() && left[leftZeroEnd] == L'0')
                {
                    ++leftZeroEnd;
                }
                while (rightZeroEnd < right.size() && right[rightZeroEnd] == L'0')
                {
                    ++rightZeroEnd;
                }

                auto leftNumberEnd = leftZeroEnd;
                auto rightNumberEnd = rightZeroEnd;
                while (leftNumberEnd < left.size() && std::iswdigit(left[leftNumberEnd]))
                {
                    ++leftNumberEnd;
                }
                while (rightNumberEnd < right.size() && std::iswdigit(right[rightNumberEnd]))
                {
                    ++rightNumberEnd;
                }

                const auto leftDigits = leftNumberEnd - leftZeroEnd;
                const auto rightDigits = rightNumberEnd - rightZeroEnd;
                if (leftDigits != rightDigits)
                {
                    return leftDigits < rightDigits ? -1 : 1;
                }

                for (std::size_t offset = 0; offset < leftDigits; ++offset)
                {
                    if (left[leftZeroEnd + offset] != right[rightZeroEnd + offset])
                    {
                        return left[leftZeroEnd + offset] < right[rightZeroEnd + offset] ? -1 : 1;
                    }
                }

                const auto leftLeadingZeros = leftZeroEnd - leftIndex;
                const auto rightLeadingZeros = rightZeroEnd - rightIndex;
                if (leftLeadingZeros != rightLeadingZeros)
                {
                    return leftLeadingZeros < rightLeadingZeros ? -1 : 1;
                }

                leftIndex = leftNumberEnd;
                rightIndex = rightNumberEnd;
                continue;
            }

            const auto foldedLeft = std::towlower(leftChar);
            const auto foldedRight = std::towlower(rightChar);
            if (foldedLeft != foldedRight)
            {
                return foldedLeft < foldedRight ? -1 : 1;
            }

            ++leftIndex;
            ++rightIndex;
        }

        if (leftIndex == left.size() && rightIndex == right.size())
        {
            return 0;
        }
        return leftIndex == left.size() ? -1 : 1;
    }

    struct NaturalLess final
    {
        [[nodiscard]] bool operator()(std::wstring_view left, std::wstring_view right) const noexcept
        {
            return compare_natural(left, right) < 0;
        }
    };
}

