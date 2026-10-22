#pragma once
class QWidget;
// X11 without compositing needs a native shape, rather than transparent corners.
void installWindowCornerFallback(QWidget *window, int radius);
