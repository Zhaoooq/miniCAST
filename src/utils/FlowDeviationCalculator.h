#pragma once

#include "models/GasChannel.h"

struct DeviationResult {
    double percent{0.0};
    bool available{false};
    FlowStatus status{FlowStatus::Normal};
};

class FlowDeviationCalculator
{
public:
    static DeviationResult calculate(double setValue, double realValue,
                                     double warningPercent, double criticalPercent,
                                     double zeroFlowTolerance, bool online = true);
};

