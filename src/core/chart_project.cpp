#include "core/chart_project.h"

namespace infalsus {

QString difficultyName(const Difficulty difficulty) {
    switch (difficulty) {
    case Difficulty::Minimal:
        return QStringLiteral("minimal");
    case Difficulty::Evolved:
        return QStringLiteral("evolved");
    case Difficulty::Ultimate:
        return QStringLiteral("ultimate");
    case Difficulty::Forbidden:
        return QStringLiteral("forbidden");
    }

    return {};
}

Difficulty difficultyForIndex(const int index) {
    switch (index) {
    case 0:
        return Difficulty::Minimal;
    case 1:
        return Difficulty::Evolved;
    case 2:
        return Difficulty::Ultimate;
    case 3:
        return Difficulty::Forbidden;
    default:
        return Difficulty::Minimal;
    }
}

int difficultyIndex(const Difficulty difficulty) {
    switch (difficulty) {
    case Difficulty::Minimal:
        return 0;
    case Difficulty::Evolved:
        return 1;
    case Difficulty::Ultimate:
        return 2;
    case Difficulty::Forbidden:
        return 3;
    }

    return 0;
}

} // namespace infalsus
