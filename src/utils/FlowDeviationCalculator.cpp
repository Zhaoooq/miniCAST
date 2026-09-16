#include "FlowDeviationCalculator.h"
#include <cmath>

DeviationResult FlowDeviationCalculator::calculate(double setValue, double realValue,
                                                    double warning, double critical,
                                                    double zeroTolerance, bool online)
{
    if (!online)
        return {0.0, false, FlowStatus::Offline};
    if (setValue <= 0.0) {
        const auto status = std::abs(realValue) <= zeroTolerance ? FlowStatus::Normal : FlowStatus::Warning;
        return {0.0, false, status};
    }
    const double percent = (realValue - setValue) / setValue * 100.0;
    const double absolute = std::abs(percent);
    const auto status = absolute > critical ? FlowStatus::Critical
                       : absolute > warning ? FlowStatus::Warning : FlowStatus::Normal;
    return {percent, true, status};
}
