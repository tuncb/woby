#include "count_format.h"
#include "analysis_presentation.h"

#include <doctest/doctest.h>
#include <limits>

TEST_CASE("count display groups digits in threes without rounding")
{
    CHECK(woby::formatCount(0) == "0");
    CHECK(woby::formatCount(1) == "1");
    CHECK(woby::formatCount(12) == "12");
    CHECK(woby::formatCount(999) == "999");
    CHECK(woby::formatCount(1000) == "1,000");
    CHECK(woby::formatCount(1001) == "1,001");
    CHECK(woby::formatCount(999999) == "999,999");
    CHECK(woby::formatCount(1000000) == "1,000,000");
    CHECK(woby::formatCount(1234567890) == "1,234,567,890");
    CHECK(woby::formatCount(std::numeric_limits<uint32_t>::max()) == "4,294,967,295");
    CHECK(woby::formatCount(std::numeric_limits<uint64_t>::max()) == "18,446,744,073,709,551,615");
}

TEST_CASE("diagnostic count display keeps digit groups and result status")
{
    using namespace woby;
    CHECK(diagnosticSummaryText({AnalysisResultState::ready, 1234567}) == "1,234,567");
    CHECK(diagnosticSummaryText({AnalysisResultState::partial, 1234567}) == "1,234,567+ Partial");
    CHECK(diagnosticSummaryText({AnalysisResultState::notRun, 1234567}) == "Not run");
}
