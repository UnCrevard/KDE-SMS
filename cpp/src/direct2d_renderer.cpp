#include "direct2d_renderer.h"

namespace KDEConnect {

    template<class T> void SafeRelease(T** ppT) {
        if (*ppT) {
            (*ppT)->Release();
            *ppT = nullptr;
        }
    }

    Direct2DRenderer::Direct2DRenderer() {
    }

    Direct2DRenderer::~Direct2DRenderer() {
        DiscardDeviceResources();

        SafeRelease(&m_fontHeading);
        SafeRelease(&m_fontBody);
        SafeRelease(&m_fontBodyBold);
        SafeRelease(&m_fontCaption);
        SafeRelease(&m_fontTime);

        SafeRelease(&m_wicFactory);
        SafeRelease(&m_dwriteFactory);
        SafeRelease(&m_d2dFactory);
    }

    bool Direct2DRenderer::Initialize(HWND hwnd) {
        m_hwnd = hwnd;
        if (FAILED(CreateDeviceIndependentResources())) {
            return false;
        }

        // Register theme change listener
        ThemeManager::Instance().RegisterCallback("Direct2DRenderer", [this](bool, const ThemeColors& colors) {
            UpdateBrushes(colors);
            if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
        });

        return SUCCEEDED(CreateDeviceResources());
    }

    HRESULT Direct2DRenderer::CreateDeviceIndependentResources() {
        D2D1_FACTORY_OPTIONS options{};
#if defined(_DEBUG)
        options.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif
        HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &options, reinterpret_cast<void**>(&m_d2dFactory));
        if (FAILED(hr)) return hr;

        hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory2), reinterpret_cast<IUnknown**>(&m_dwriteFactory));
        if (FAILED(hr)) return hr;

        hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_wicFactory));
        if (FAILED(hr)) return hr;

        // Fonts setup: Segoe UI Variable / Segoe UI
        hr = m_dwriteFactory->CreateTextFormat(
            L"Segoe UI",
            nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            16.0f,
            L"en-us",
            &m_fontHeading
        );
        if (FAILED(hr)) return hr;

        hr = m_dwriteFactory->CreateTextFormat(
            L"Segoe UI",
            nullptr,
            DWRITE_FONT_WEIGHT_REGULAR,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            14.0f,
            L"en-us",
            &m_fontBody
        );
        if (FAILED(hr)) return hr;

        hr = m_dwriteFactory->CreateTextFormat(
            L"Segoe UI",
            nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            14.0f,
            L"en-us",
            &m_fontBodyBold
        );
        if (FAILED(hr)) return hr;

        hr = m_dwriteFactory->CreateTextFormat(
            L"Segoe UI",
            nullptr,
            DWRITE_FONT_WEIGHT_REGULAR,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            12.0f,
            L"en-us",
            &m_fontCaption
        );
        if (FAILED(hr)) return hr;

        hr = m_dwriteFactory->CreateTextFormat(
            L"Segoe UI",
            nullptr,
            DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            11.0f,
            L"en-us",
            &m_fontTime
        );
        return hr;
    }

    HRESULT Direct2DRenderer::CreateDeviceResources() {
        if (m_renderTarget) return S_OK;

        RECT rc;
        GetClientRect(m_hwnd, &rc);
        D2D1_SIZE_U size = D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top);

        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
        );

        D2D1_HWND_RENDER_TARGET_PROPERTIES hwndProps = D2D1::HwndRenderTargetProperties(m_hwnd, size);

        HRESULT hr = m_d2dFactory->CreateHwndRenderTarget(props, hwndProps, &m_renderTarget);
        if (FAILED(hr)) return hr;

        m_renderTarget->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        m_renderTarget->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);

        UpdateBrushes(ThemeManager::Instance().GetColors());
        return S_OK;
    }

    void Direct2DRenderer::UpdateBrushes(const ThemeColors& colors) {
        if (!m_renderTarget) return;

        SafeRelease(&m_brushWindowBg);
        SafeRelease(&m_brushSidebarBg);
        SafeRelease(&m_brushSidebarHover);
        SafeRelease(&m_brushSidebarSelected);
        SafeRelease(&m_brushDivider);
        SafeRelease(&m_brushTextPrimary);
        SafeRelease(&m_brushTextSecondary);
        SafeRelease(&m_brushTextMuted);
        SafeRelease(&m_brushBubbleInbound);
        SafeRelease(&m_brushBubbleInboundText);
        SafeRelease(&m_brushBubbleInboundBorder);
        SafeRelease(&m_brushBubbleOutbound);
        SafeRelease(&m_brushBubbleOutboundText);
        SafeRelease(&m_brushAccent);

        m_renderTarget->CreateSolidColorBrush(colors.windowBackground, &m_brushWindowBg);
        m_renderTarget->CreateSolidColorBrush(colors.sidebarBackground, &m_brushSidebarBg);
        m_renderTarget->CreateSolidColorBrush(colors.sidebarHover, &m_brushSidebarHover);
        m_renderTarget->CreateSolidColorBrush(colors.sidebarSelected, &m_brushSidebarSelected);
        m_renderTarget->CreateSolidColorBrush(colors.dividerColor, &m_brushDivider);
        m_renderTarget->CreateSolidColorBrush(colors.textPrimary, &m_brushTextPrimary);
        m_renderTarget->CreateSolidColorBrush(colors.textSecondary, &m_brushTextSecondary);
        m_renderTarget->CreateSolidColorBrush(colors.textMuted, &m_brushTextMuted);
        m_renderTarget->CreateSolidColorBrush(colors.bubbleInbound, &m_brushBubbleInbound);
        m_renderTarget->CreateSolidColorBrush(colors.bubbleInboundText, &m_brushBubbleInboundText);
        m_renderTarget->CreateSolidColorBrush(colors.bubbleInboundBorder, &m_brushBubbleInboundBorder);
        m_renderTarget->CreateSolidColorBrush(colors.bubbleOutbound, &m_brushBubbleOutbound);
        m_renderTarget->CreateSolidColorBrush(colors.bubbleOutboundText, &m_brushBubbleOutboundText);
        m_renderTarget->CreateSolidColorBrush(colors.accent, &m_brushAccent);
    }

    void Direct2DRenderer::DiscardDeviceResources() {
        for (auto& pair : m_bitmapCache) {
            SafeRelease(&pair.second);
        }
        m_bitmapCache.clear();

        SafeRelease(&m_brushWindowBg);
        SafeRelease(&m_brushSidebarBg);
        SafeRelease(&m_brushSidebarHover);
        SafeRelease(&m_brushSidebarSelected);
        SafeRelease(&m_brushDivider);
        SafeRelease(&m_brushTextPrimary);
        SafeRelease(&m_brushTextSecondary);
        SafeRelease(&m_brushTextMuted);
        SafeRelease(&m_brushBubbleInbound);
        SafeRelease(&m_brushBubbleInboundText);
        SafeRelease(&m_brushBubbleInboundBorder);
        SafeRelease(&m_brushBubbleOutbound);
        SafeRelease(&m_brushBubbleOutboundText);
        SafeRelease(&m_brushAccent);
        SafeRelease(&m_renderTarget);
    }

    void Direct2DRenderer::Resize(UINT width, UINT height) {
        if (m_renderTarget) {
            m_renderTarget->Resize(D2D1::SizeU(width, height));
        }
    }

    void Direct2DRenderer::BeginDraw() {
        CreateDeviceResources();
        if (m_renderTarget) {
            m_renderTarget->BeginDraw();
        }
    }

    void Direct2DRenderer::EndDraw() {
        if (!m_renderTarget) return;
        HRESULT hr = m_renderTarget->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET) {
            DiscardDeviceResources();
        }
    }

    ID2D1Bitmap* Direct2DRenderer::LoadImageFromFile(const std::wstring& filePath) {
        if (filePath.empty() || !m_wicFactory || !m_renderTarget) return nullptr;

        auto it = m_bitmapCache.find(filePath);
        if (it != m_bitmapCache.end()) return it->second;

        IWICBitmapDecoder* decoder = nullptr;
        HRESULT hr = m_wicFactory->CreateDecoderFromFilename(filePath.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
        if (FAILED(hr)) return nullptr;

        IWICBitmapFrameDecode* frame = nullptr;
        hr = decoder->GetFrame(0, &frame);
        if (FAILED(hr)) {
            decoder->Release();
            return nullptr;
        }

        IWICFormatConverter* converter = nullptr;
        hr = m_wicFactory->CreateFormatConverter(&converter);
        if (SUCCEEDED(hr)) {
            hr = converter->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeMedianCut);
        }

        ID2D1Bitmap* bitmap = nullptr;
        if (SUCCEEDED(hr)) {
            hr = m_renderTarget->CreateBitmapFromWicBitmap(converter, nullptr, &bitmap);
        }

        SafeRelease(&converter);
        SafeRelease(&frame);
        SafeRelease(&decoder);

        if (SUCCEEDED(hr) && bitmap) {
            m_bitmapCache[filePath] = bitmap;
            return bitmap;
        }
        return nullptr;
    }

    float Direct2DRenderer::MeasureTextHeight(const std::wstring& text, float maxWidth) {
        if (text.empty() || !m_dwriteFactory || !m_fontBody) return 20.0f;

        IDWriteTextLayout* layout = nullptr;
        HRESULT hr = m_dwriteFactory->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.length()), m_fontBody, maxWidth, 5000.0f, &layout);
        if (FAILED(hr) || !layout) return 20.0f;

        DWRITE_TEXT_METRICS metrics;
        layout->GetMetrics(&metrics);
        layout->Release();
        return metrics.height;
    }

    void Direct2DRenderer::RenderSidebar(
        const std::vector<Conversation>& conversations,
        int64_t selectedThreadId,
        int hoveredIndex,
        float width,
        float height,
        float scrollOffset
    ) {
        if (!m_renderTarget) return;

        const float topHeaderH = 54.0f;

        // Background of sidebar
        D2D1_RECT_F sidebarRect = D2D1::RectF(0.0f, 0.0f, width, height);
        m_renderTarget->FillRectangle(sidebarRect, m_brushSidebarBg);

        // Clip conversation list below search bar
        m_renderTarget->PushAxisAlignedClip(
            D2D1::RectF(0.0f, topHeaderH, width, height),
            D2D1_ANTIALIAS_MODE_ALIASED
        );

        float itemHeight = 68.0f;
        float currentY = topHeaderH - scrollOffset;

        for (size_t i = 0; i < conversations.size(); ++i) {
            const auto& conv = conversations[i];
            D2D1_RECT_F itemRect = D2D1::RectF(0.0f, currentY, width, currentY + itemHeight);

            // Check visibility
            if (itemRect.bottom >= topHeaderH && itemRect.top <= height) {
                // Background highlight
                if (conv.threadId == selectedThreadId) {
                    m_renderTarget->FillRectangle(itemRect, m_brushSidebarSelected);
                    // Blue active marker on left
                    m_renderTarget->FillRectangle(D2D1::RectF(0.0f, currentY, 3.0f, currentY + itemHeight), m_brushAccent);
                } else if (static_cast<int>(i) == hoveredIndex) {
                    m_renderTarget->FillRectangle(itemRect, m_brushSidebarHover);
                }

                // Avatar circle
                float avatarX = 18.0f;
                float avatarY = currentY + itemHeight / 2.0f;
                float avatarRadius = 18.0f;
                D2D1_ELLIPSE avatarEllipse = D2D1::Ellipse(D2D1::Point2F(avatarX + avatarRadius, avatarY), avatarRadius, avatarRadius);
                m_renderTarget->FillEllipse(avatarEllipse, m_brushDivider);

                // Avatar initial letter
                std::wstring initial = conv.contactName.empty() ? L"#" : conv.contactName.substr(0, 1);
                std::transform(initial.begin(), initial.end(), initial.begin(), ::towupper);
                D2D1_RECT_F avatarTextRect = D2D1::RectF(avatarX + 11.0f, avatarY - 10.0f, avatarX + avatarRadius * 2.0f, avatarY + 10.0f);
                m_renderTarget->DrawText(initial.c_str(), static_cast<UINT32>(initial.length()), m_fontBodyBold, avatarTextRect, m_brushTextPrimary);

                // Contact Name / Number
                float textStartX = avatarX + avatarRadius * 2.0f + 12.0f;
                float textMaxWidth = width - textStartX - 58.0f;
                std::wstring displayName = conv.contactName.empty() ? conv.address : conv.contactName;
                D2D1_RECT_F nameRect = D2D1::RectF(textStartX, currentY + 13.0f, textStartX + std::max(40.0f, textMaxWidth), currentY + 33.0f);
                m_renderTarget->DrawText(displayName.c_str(), static_cast<UINT32>(displayName.length()), m_fontBodyBold, nameRect, m_brushTextPrimary);

                // Timestamp on top right
                if (!conv.messages.empty()) {
                    std::wstring timeStr = conv.messages.back().GetFormattedTime();
                    D2D1_RECT_F timeRect = D2D1::RectF(width - 54.0f, currentY + 14.0f, width - 10.0f, currentY + 32.0f);
                    m_renderTarget->DrawText(timeStr.c_str(), static_cast<UINT32>(timeStr.length()), m_fontTime, timeRect, m_brushTextMuted);
                }

                // Message snippet with color emoji support
                D2D1_RECT_F snippetRect = D2D1::RectF(textStartX, currentY + 36.0f, width - 38.0f, currentY + 54.0f);
                IDWriteTextLayout* snippetLayout = nullptr;
                if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(conv.snippet.c_str(), static_cast<UINT32>(conv.snippet.length()), m_fontCaption, std::max(40.0f, width - textStartX - 38.0f), 18.0f, &snippetLayout))) {
                    DWRITE_TRIMMING trimming = { DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
                    IDWriteInlineObject* ellipsis = nullptr;
                    m_dwriteFactory->CreateEllipsisTrimmingSign(m_fontCaption, &ellipsis);
                    snippetLayout->SetTrimming(&trimming, ellipsis);
                    SafeRelease(&ellipsis);

                    m_renderTarget->DrawTextLayout(
                        D2D1::Point2F(snippetRect.left, snippetRect.top),
                        snippetLayout,
                        m_brushTextSecondary,
                        D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT
                    );
                    snippetLayout->Release();
                }

                // Subtle delete button on hovered or selected real conversation row
                if (conv.threadId > 0 && (static_cast<int>(i) == hoveredIndex || conv.threadId == selectedThreadId)) {
                    D2D1_ROUNDED_RECT delBtn = D2D1::RoundedRect(
                        D2D1::RectF(width - 30.0f, currentY + 35.0f, width - 8.0f, currentY + 57.0f),
                        5.0f, 5.0f
                    );
                    if (static_cast<int>(i) == hoveredIndex) {
                        m_renderTarget->FillRoundedRectangle(delBtn, m_brushDivider);
                    }
                    D2D1_RECT_F xRect = D2D1::RectF(width - 25.0f, currentY + 36.0f, width - 8.0f, currentY + 57.0f);
                    m_renderTarget->DrawText(L"×", 1, m_fontBodyBold, xRect,
                        static_cast<int>(i) == hoveredIndex ? m_brushTextPrimary : m_brushTextMuted);
                }

                // Bottom hairline
                m_renderTarget->DrawLine(D2D1::Point2F(0.0f, currentY + itemHeight), D2D1::Point2F(width, currentY + itemHeight), m_brushDivider, 0.5f);
            }
            currentY += itemHeight;
        }

        m_renderTarget->PopAxisAlignedClip();

        // Top search bar background & divider
        m_renderTarget->FillRectangle(D2D1::RectF(0.0f, 0.0f, width, topHeaderH), m_brushSidebarBg);
        m_renderTarget->DrawLine(D2D1::Point2F(0.0f, topHeaderH), D2D1::Point2F(width, topHeaderH), m_brushDivider, 1.0f);

        // Right divider
        m_renderTarget->DrawLine(D2D1::Point2F(width, 0.0f), D2D1::Point2F(width, height), m_brushDivider, 1.0f);
    }

    void Direct2DRenderer::RenderThread(
        const Conversation& conversation,
        float startX,
        float width,
        float height,
        float scrollOffset,
        std::vector<BubbleLayout>& outBubbleLayouts
    ) {
        if (!m_renderTarget) return;

        outBubbleLayouts.clear();

        const float headerH = 54.0f;
        const float composerH = 54.0f;
        float messageAreaBottom = headerH + height;

        // Top Conversation Header
        D2D1_RECT_F headerRect = D2D1::RectF(startX, 0.0f, startX + width, headerH);
        m_renderTarget->FillRectangle(headerRect, m_brushSidebarBg);
        m_renderTarget->DrawLine(D2D1::Point2F(startX, headerH), D2D1::Point2F(startX + width, headerH), m_brushDivider, 1.0f);

        std::wstring title = conversation.contactName.empty() ? conversation.address : conversation.contactName;
        if (!conversation.address.empty() && conversation.contactName != conversation.address) {
            title += L"  (" + conversation.address + L")";
        }
        D2D1_RECT_F titleRect = D2D1::RectF(startX + 20.0f, 15.0f, startX + width - 190.0f, 40.0f);
        m_renderTarget->DrawText(title.c_str(), static_cast<UINT32>(title.length()), m_fontHeading, titleRect, m_brushTextPrimary);

        // Background of message panel
        D2D1_RECT_F threadRect = D2D1::RectF(startX, headerH, startX + width, messageAreaBottom);
        m_renderTarget->FillRectangle(threadRect, m_brushWindowBg);

        // Bottom composer backdrop
        D2D1_RECT_F composerRect = D2D1::RectF(startX, messageAreaBottom, startX + width, messageAreaBottom + composerH);
        m_renderTarget->FillRectangle(composerRect, m_brushSidebarBg);
        m_renderTarget->DrawLine(D2D1::Point2F(startX, messageAreaBottom), D2D1::Point2F(startX + width, messageAreaBottom), m_brushDivider, 1.0f);

        // Clip message bubbles strictly between header and composer
        m_renderTarget->PushAxisAlignedClip(threadRect, D2D1_ANTIALIAS_MODE_ALIASED);

        float padding = 20.0f;
        float maxBubbleWidth = std::min(width * 0.68f, 520.0f);
        float currentY = headerH + padding - scrollOffset;

        for (const auto& msg : conversation.messages) {
            bool isOut = msg.isOutgoing;
            float textWidth = maxBubbleWidth - 28.0f;

            // Measure Text with DirectWrite
            IDWriteTextLayout* textLayout = nullptr;
            DWRITE_TEXT_METRICS textMetrics{};
            if (!msg.body.empty()) {
                m_dwriteFactory->CreateTextLayout(msg.body.c_str(), static_cast<UINT32>(msg.body.length()), m_fontBody, textWidth, 5000.0f, &textLayout);
                if (textLayout) textLayout->GetMetrics(&textMetrics);
            }

            float bubbleContentWidth = std::max(textMetrics.widthIncludingTrailingWhitespace + 32.0f, 95.0f);
            float bubbleContentHeight = std::max(textMetrics.height + 30.0f, 46.0f);

            // Handle image attachment height
            ID2D1Bitmap* attachmentBitmap = nullptr;
            float imageDrawWidth = 0.0f;
            float imageDrawHeight = 0.0f;
            if (!msg.attachments.empty()) {
                attachmentBitmap = LoadImageFromFile(msg.attachments[0].localFilePath);
                if (attachmentBitmap) {
                    D2D1_SIZE_F imgSize = attachmentBitmap->GetSize();
                    imageDrawWidth = std::min(imgSize.width, maxBubbleWidth - 20.0f);
                    float ratio = imageDrawWidth / imgSize.width;
                    imageDrawHeight = std::min(imgSize.height * ratio, 280.0f);

                    bubbleContentWidth = std::max(bubbleContentWidth, imageDrawWidth + 20.0f);
                    bubbleContentHeight += imageDrawHeight + 10.0f;
                }
            }

            float bubbleLeft = isOut ? (startX + width - padding - bubbleContentWidth) : (startX + padding);
            float bubbleRight = bubbleLeft + bubbleContentWidth;
            float bubbleTop = currentY;
            float bubbleBottom = currentY + bubbleContentHeight;

            D2D1_ROUNDED_RECT roundedBubble = D2D1::RoundedRect(
                D2D1::RectF(bubbleLeft, bubbleTop, bubbleRight, bubbleBottom),
                14.0f, 14.0f
            );

            // Render bubble only if in visible message area
            if (bubbleBottom >= headerH && bubbleTop <= messageAreaBottom) {
                // Draw Bubble Background & Border
                if (isOut) {
                    m_renderTarget->FillRoundedRectangle(roundedBubble, m_brushBubbleOutbound);
                } else {
                    m_renderTarget->FillRoundedRectangle(roundedBubble, m_brushBubbleInbound);
                    m_renderTarget->DrawRoundedRectangle(roundedBubble, m_brushBubbleInboundBorder, 1.0f);
                }

                float contentY = bubbleTop + 10.0f;

                // Render Image Attachment if present
                if (attachmentBitmap) {
                    D2D1_RECT_F imgDest = D2D1::RectF(bubbleLeft + 10.0f, contentY, bubbleLeft + 10.0f + imageDrawWidth, contentY + imageDrawHeight);
                    m_renderTarget->DrawBitmap(attachmentBitmap, imgDest);
                    contentY += imageDrawHeight + 8.0f;
                }

                // Render SMS Text Layout with COLOR EMOJIS!
                if (textLayout) {
                    D2D1_POINT_2F textOrigin = D2D1::Point2F(bubbleLeft + 14.0f, contentY);
                    m_renderTarget->DrawTextLayout(
                        textOrigin,
                        textLayout,
                        isOut ? m_brushBubbleOutboundText : m_brushBubbleInboundText,
                        D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT
                    );
                }

                // Timestamp inside bubble lower-right
                std::wstring timeStr = msg.GetFormattedTime();
                D2D1_RECT_F timeRect = D2D1::RectF(bubbleRight - 55.0f, bubbleBottom - 18.0f, bubbleRight - 10.0f, bubbleBottom - 4.0f);
                m_renderTarget->DrawText(
                    timeStr.c_str(),
                    static_cast<UINT32>(timeStr.length()),
                    m_fontTime,
                    timeRect,
                    isOut ? m_brushBubbleOutboundText : m_brushTextMuted
                );

                // Save layout for interaction (click on attachment)
                BubbleLayout layout{};
                layout.messageId = msg.id;
                layout.bubbleRect = roundedBubble.rect;
                layout.isOutgoing = isOut;
                layout.hasAttachment = (attachmentBitmap != nullptr);
                if (!msg.attachments.empty()) layout.attachmentPath = msg.attachments[0].localFilePath;
                outBubbleLayouts.push_back(layout);
            }

            if (textLayout) textLayout->Release();
            currentY += bubbleContentHeight + 12.0f;
        }

        m_renderTarget->PopAxisAlignedClip();
    }

    void Direct2DRenderer::RenderPendingAttachmentBar(
        float startX,
        float width,
        float composerTopY,
        const std::wstring& attachmentName
    ) {
        if (!m_renderTarget || attachmentName.empty()) return;

        float barH = 30.0f;
        float topY = composerTopY - barH;
        D2D1_RECT_F barRect = D2D1::RectF(startX, topY, startX + width, composerTopY);
        m_renderTarget->FillRectangle(barRect, m_brushSidebarSelected);
        m_renderTarget->DrawLine(D2D1::Point2F(startX, topY), D2D1::Point2F(startX + width, topY), m_brushDivider, 1.0f);

        std::wstring label = L"📎 Image prete a envoyer : " + attachmentName;
        D2D1_RECT_F textRect = D2D1::RectF(startX + 16.0f, topY + 5.0f, startX + width - 44.0f, composerTopY - 4.0f);
        m_renderTarget->DrawText(
            label.c_str(),
            static_cast<UINT32>(label.length()),
            m_fontCaption,
            textRect,
            m_brushTextPrimary,
            D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT
        );

        D2D1_RECT_F clearRect = D2D1::RectF(startX + width - 34.0f, topY + 4.0f, startX + width - 10.0f, composerTopY - 4.0f);
        m_renderTarget->DrawText(L"×", 1, m_fontHeading, clearRect, m_brushTextSecondary);
    }

    void Direct2DRenderer::RenderEmptyState(float startX, float width, float height, const std::wstring& message) {
        if (!m_renderTarget) return;

        D2D1_RECT_F rect = D2D1::RectF(startX, 0.0f, startX + width, height);
        m_renderTarget->FillRectangle(rect, m_brushWindowBg);

        D2D1_RECT_F textRect = D2D1::RectF(startX + 40.0f, height / 2.0f - 45.0f, startX + width - 40.0f, height / 2.0f + 45.0f);
        m_renderTarget->DrawText(
            message.c_str(),
            static_cast<UINT32>(message.length()),
            m_fontBody,
            textRect,
            m_brushTextMuted
        );
    }

    float Direct2DRenderer::CalculateTotalThreadHeight(const Conversation& conversation, float contentWidth) {
        float padding = 20.0f;
        float maxBubbleWidth = std::min(contentWidth * 0.68f, 520.0f);
        float totalHeight = padding;

        for (const auto& msg : conversation.messages) {
            float textWidth = maxBubbleWidth - 28.0f;
            float textH = MeasureTextHeight(msg.body, textWidth);
            float bubbleH = std::max(textH + 30.0f, 46.0f);

            if (!msg.attachments.empty()) {
                ID2D1Bitmap* bmp = LoadImageFromFile(msg.attachments[0].localFilePath);
                if (bmp) {
                    D2D1_SIZE_F imgSize = bmp->GetSize();
                    float imgW = std::min(imgSize.width, maxBubbleWidth - 20.0f);
                    float imgH = std::min(imgSize.height * (imgW / std::max(1.0f, imgSize.width)), 280.0f);
                    bubbleH += imgH + 10.0f;
                } else {
                    bubbleH += 180.0f;
                }
            }

            totalHeight += bubbleH + 12.0f;
        }
        return totalHeight + padding;
    }

} // namespace KDEConnect
