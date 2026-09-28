#pragma once

#include <limits>

namespace kisak::vr::packed_layout
{

struct CaptureLayout
{
    int mainStereoWidth = 0;
    int scopePanelX = 0;
    int scopePanelY = 0;
    int scopePanelSize = 0;
};

inline bool ResolveCaptureLayout(
    const int backbufferWidth,
    const int backbufferHeight,
    const int leftEyeWidth,
    const int rightEyeWidth,
    const int requestedScopeSize,
    CaptureLayout* layout)
{
    if (layout == nullptr ||
        backbufferWidth < 2 ||
        backbufferHeight < 1 ||
        leftEyeWidth < 1 ||
        rightEyeWidth < 1 ||
        requestedScopeSize < 1 ||
        leftEyeWidth >
            (std::numeric_limits<int>::max)() - rightEyeWidth)
    {
        return false;
    }

    int resolvedScopeSize = requestedScopeSize;
    if (resolvedScopeSize < 512)
    {
        resolvedScopeSize = 512;
    }
    if (resolvedScopeSize > backbufferHeight)
    {
        resolvedScopeSize = backbufferHeight;
    }

    if (resolvedScopeSize < 512 ||
        resolvedScopeSize > backbufferWidth)
    {
        return false;
    }

    const int resolvedMainStereoWidth =
        leftEyeWidth + rightEyeWidth;

    if (resolvedMainStereoWidth >
        backbufferWidth - resolvedScopeSize)
    {
        return false;
    }

    layout->mainStereoWidth = resolvedMainStereoWidth;
    layout->scopePanelX = resolvedMainStereoWidth;
    layout->scopePanelY = 0;
    layout->scopePanelSize = resolvedScopeSize;
    return true;
}


// WinlatorXR shows the left half of the game window to the left eye and the
// right half to the right, so unlike OpenXR there is no room beside the eyes
// for a scope panel. Put the panel inside the window instead: the scope camera
// is the first view of the frame, so the two eye views draw over the panel
// before anything is presented, and it is captured in between.
//
// The panel therefore has to sit wholly inside the area the eyes repaint.
inline bool ResolveTransientCaptureLayout(
    const int backbufferWidth,
    const int backbufferHeight,
    const int requestedScopeSize,
    CaptureLayout* layout)
{
    if (layout == nullptr ||
        backbufferWidth < 2 ||
        backbufferHeight < 1 ||
        requestedScopeSize < 1)
    {
        return false;
    }

    const int eyeWidth = backbufferWidth / 2;
    int resolvedScopeSize = requestedScopeSize;
    if (resolvedScopeSize > eyeWidth)
    {
        resolvedScopeSize = eyeWidth;
    }
    if (resolvedScopeSize > backbufferHeight)
    {
        resolvedScopeSize = backbufferHeight;
    }

    if (resolvedScopeSize < 512)
    {
        return false;
    }

    // The eyes keep the whole window; the panel overlaps the left one.
    layout->mainStereoWidth = backbufferWidth;
    layout->scopePanelX = 0;
    layout->scopePanelY = 0;
    layout->scopePanelSize = resolvedScopeSize;
    return true;
}

} // namespace kisak::vr::packed_layout
