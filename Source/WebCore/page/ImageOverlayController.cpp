/*
 * Copyright (C) 2021 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "ImageOverlayController.h"

#include "Chrome.h"
#include "ChromeClient.h"
#include "Document.h"
#include "Editor.h"
#include "Frame.h"
#include "FrameSelection.h"
#include "GraphicsContext.h"
#include "HTMLElement.h"
#include "IntRect.h"
#include "LayoutRect.h"
#include "Page.h"
#include "PageOverlayController.h"
#include "RenderElement.h"
#include "RenderStyle.h"
#include "SimpleRange.h"
#include "VisiblePosition.h"

namespace WebCore {

class FloatQuad;

ImageOverlayController::ImageOverlayController(Page& page)
    : m_page(makeWeakPtr(page))
{
}

void ImageOverlayController::selectionQuadsDidChange(Frame& frame, const Vector<FloatQuad>& quads)
{
    if (!m_page || !m_page->chrome().client().needsImageOverlayControllerForSelectionPainting())
        return;

    if (frame.editor().ignoreSelectionChanges())
        return;

    auto overlayHostRenderer = ([&] () -> RenderElement* {
        if (quads.isEmpty())
            return nullptr;

        auto selectedRange = frame.selection().selection().range();
        if (!selectedRange)
            return nullptr;

        if (!HTMLElement::isInsideImageOverlay(*selectedRange) || selectedRange->collapsed())
            return nullptr;

        auto overlayHost = makeRefPtr(selectedRange->startContainer().shadowHost());
        if (!overlayHost) {
            ASSERT_NOT_REACHED();
            return nullptr;
        }

        return overlayHost->renderer();
    })();

    if (!overlayHostRenderer || !shouldUsePageOverlayToPaintSelection(*overlayHostRenderer)) {
        uninstallPageOverlayIfNeeded();
        return;
    }

    m_overlaySelectionQuads = quads;
    m_selectionOverlayBounds = overlayHostRenderer->absoluteBoundingBoxRect();
    m_selectionBackgroundColor = overlayHostRenderer->selectionBackgroundColor();
    m_currentOverlayDocument = makeWeakPtr(overlayHostRenderer->document());

    installPageOverlayIfNeeded().setNeedsDisplay();
}

bool ImageOverlayController::shouldUsePageOverlayToPaintSelection(const RenderElement& renderer)
{
    // If the selection is already painted (with nonzero opacity) in the overlay host's renderer,
    // then we don't need to fall back to a page overlay to paint the selection.
    return renderer.style().opacity() <= 0.01;
}

void ImageOverlayController::documentDetached(const Document& document)
{
    if (&document == m_currentOverlayDocument)
        uninstallPageOverlayIfNeeded();
}

PageOverlay& ImageOverlayController::installPageOverlayIfNeeded()
{
    if (m_overlay)
        return *m_overlay;

    m_overlay = PageOverlay::create(*this, PageOverlay::OverlayType::Document);
    m_page->pageOverlayController().installPageOverlay(*m_overlay, PageOverlay::FadeMode::DoNotFade);
    return *m_overlay;
}

void ImageOverlayController::uninstallPageOverlayIfNeeded()
{
    m_selectionOverlayBounds = { };
    m_overlaySelectionQuads.clear();
    m_selectionBackgroundColor = Color::transparentBlack;
    m_currentOverlayDocument = nullptr;

    auto overlayToUninstall = std::exchange(m_overlay, nullptr);
    if (!m_page || !overlayToUninstall)
        return;

    m_page->pageOverlayController().uninstallPageOverlay(*overlayToUninstall, PageOverlay::FadeMode::DoNotFade);
}

void ImageOverlayController::willMoveToPage(PageOverlay&, Page* page)
{
    if (!page)
        uninstallPageOverlayIfNeeded();
}

void ImageOverlayController::drawRect(PageOverlay& pageOverlay, GraphicsContext& context, const IntRect& dirtyRect)
{
    if (&pageOverlay != m_overlay) {
        ASSERT_NOT_REACHED();
        return;
    }

    GraphicsContextStateSaver stateSaver(context);
    context.clearRect(dirtyRect);

    if (m_overlaySelectionQuads.isEmpty())
        return;

    Path coalescedSelectionPath;
    for (auto& quad : m_overlaySelectionQuads) {
        coalescedSelectionPath.moveTo(quad.p1());
        coalescedSelectionPath.addLineTo(quad.p2());
        coalescedSelectionPath.addLineTo(quad.p3());
        coalescedSelectionPath.addLineTo(quad.p4());
        coalescedSelectionPath.addLineTo(quad.p1());
        coalescedSelectionPath.closeSubpath();
    }

    context.setFillColor(m_selectionBackgroundColor);
    context.clip(m_selectionOverlayBounds);
    context.fillPath(coalescedSelectionPath);
}

} // namespace WebCore
