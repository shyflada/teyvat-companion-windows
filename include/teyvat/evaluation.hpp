#pragma once
#include "teyvat/detection.hpp"
#include <QJsonObject>

namespace teyvat {
// Read-only evaluation. Labels never enter detection or localization inputs.
[[nodiscard]] QJsonObject evaluate_recording(const RecordingAnalysis& analysis,
    const Dataset& dataset, const QByteArray& labels);
}
