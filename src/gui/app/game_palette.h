#pragma once

#include <QtGui/QColor>

namespace infalsus::gui::palette {

inline const QColor canvasBackground{9, 17, 27};
inline const QColor chrome{25, 38, 53};
inline const QColor panel{25, 38, 53};
inline const QColor panelHeader{38, 61, 80};
inline const QColor panelHeaderBorder{69, 96, 120};
inline const QColor panelHover{16, 77, 99};
inline const QColor inputBackground{9, 17, 27};
inline const QColor control{24, 32, 41};
inline const QColor border{52, 72, 91};
inline const QColor selection{19, 66, 86};
inline const QColor gridLine{9, 17, 27};
inline const QColor text{227, 234, 240};
inline const QColor textDisabled{97, 112, 128};
inline const QColor mixedValue{156, 172, 188};
inline const QColor buttonText{227, 234, 240};
inline const QColor white{255, 255, 255};

inline const QColor floorNoteBlue{0, 167, 212};
inline const QColor floorNoteContour{0, 53, 84};
inline const QColor outerNoteRed{244, 37, 90};
inline const QColor outerNotePurple{196, 122, 255};
inline const QColor skyArea{152, 126, 255};
inline const QColor guideGreen{35, 211, 159};
inline const QColor guideYellow{220, 223, 11};

[[nodiscard]] inline QColor withAlpha(QColor color, const int alpha) {
    color.setAlpha(alpha);
    return color;
}

} // namespace infalsus::gui::palette
