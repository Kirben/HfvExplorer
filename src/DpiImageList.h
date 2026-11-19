// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "NativeDpi.h"
#include <commctrl.h>

namespace NativeDpi {
class ImageList {
public:
    ImageList(int size, int overlays = 0) : size_(size), overlays_(overlays) {}
    ~ImageList() {
        if (scaled_) ImageList_Destroy(scaled_);
        if (original_) ImageList_Destroy(original_);
    }
    ImageList(const ImageList&) = delete;
    ImageList& operator=(const ImageList&) = delete;

    void Create(int reserve) {
        original_ = ImageList_Create(size_, size_, ILC_COLOR32 | ILC_MASK, reserve, 16);
    }
    HIMAGELIST Handle() const { return scaled_ ? scaled_ : original_; }
    int Count() const { return ImageList_GetImageCount(original_); }

    int Add(HICON icon) {
        const int index = AddAtSize(original_, icon, size_);
        if (index < 0) return -1;
        if (scaled_ && AddAtSize(scaled_, icon, Scale(size_, dpi_)) != index) {
            ImageList_Remove(original_, index);
            return -1;
        }
        return index;
    }

    void SetOverlays() {
        firstOverlay_ = Count()-overlays_;
        SetOverlays(original_);
    }

    void Trim(int count) {
        for (int index = Count()-1; index >= count; --index) {
            if (scaled_) ImageList_Remove(scaled_, index);
            ImageList_Remove(original_, index);
        }
    }

    template<class Attach>
    void Apply(UINT dpi, Attach attach) {
        if (dpi == dpi_ || !original_) return;
        HIMAGELIST replacement = nullptr;
        if (dpi != Base) {
            const int size = Scale(size_, dpi);
            const int count = Count();
            replacement = ImageList_Create(size, size, ILC_COLOR32 | ILC_MASK, count, 16);
            if (!replacement) return;
            for (int index = 0; index < count; ++index) {
                HICON icon = ImageList_GetIcon(original_, index, ILD_TRANSPARENT);
                const auto added = AddAtSize(replacement, icon, size);
                if (icon) DestroyIcon(icon);
                if (added != index) {
                    ImageList_Destroy(replacement);
                    return;
                }
            }
            SetOverlays(replacement);
        }
        attach(replacement ? replacement : original_);
        if (scaled_) ImageList_Destroy(scaled_);
        scaled_ = replacement;
        dpi_ = dpi;
    }

private:
    static int AddAtSize(HIMAGELIST images, HICON icon, int size) {
        if (!images || !icon) return -1;
        auto resized = static_cast<HICON>(CopyImage(icon, IMAGE_ICON, size, size, LR_COPYRETURNORG));
        if (!resized) return -1;
        const int added = ImageList_AddIcon(images, resized);
        if (icon != resized) DestroyIcon(resized);
        return added;
    }

    void SetOverlays(HIMAGELIST images) const {
        for (int slot = 1; slot <= overlays_; ++slot)
            ImageList_SetOverlayImage(images, firstOverlay_+slot-1, slot);
    }

    const int size_, overlays_;
    int firstOverlay_ = 0;
    UINT dpi_ = Base;
    HIMAGELIST original_ = nullptr, scaled_ = nullptr;
};
}
