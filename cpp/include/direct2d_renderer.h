#pragma once
#include "common.h"
#include "theme_manager.h"
#include <unordered_map>

namespace KDEConnect {

    struct BubbleLayout {
        int64_t messageId;
        D2D1_RECT_F bubbleRect;
        D2D1_RECT_F textRect;
        D2D1_RECT_F timeRect;
        D2D1_RECT_F imageRect;
        bool isOutgoing;
        bool hasAttachment;
        std::wstring attachmentPath;
    };

    class Direct2DRenderer {
    public:
        Direct2DRenderer();
        ~Direct2DRenderer();

        bool Initialize(HWND hwnd);
        void Resize(UINT width, UINT height);
        void DiscardDeviceResources();

        // Rendering functions
        void BeginDraw();
        void EndDraw();

        void RenderSidebar(
            const std::vector<Conversation>& conversations,
            int64_t selectedThreadId,
            int hoveredIndex,
            float width,
            float height,
            float scrollOffset
        );

        void RenderThread(
            const Conversation& conversation,
            float startX,
            float width,
            float height,
            float scrollOffset,
            std::vector<BubbleLayout>& outBubbleLayouts
        );

        void RenderPendingAttachmentBar(
            float startX,
            float width,
            float composerTopY,
            const std::wstring& attachmentName
        );

        void RenderEmptyState(float startX, float width, float height, const std::wstring& message);

        // Attachment image management
        ID2D1Bitmap* LoadImageFromFile(const std::wstring& filePath);

        // Emoji & Text metrics
        float MeasureTextHeight(const std::wstring& text, float maxWidth);

        // Scroll calculations
        float CalculateTotalThreadHeight(const Conversation& conversation, float contentWidth);

    private:
        HRESULT CreateDeviceIndependentResources();
        HRESULT CreateDeviceResources();
        void UpdateBrushes(const ThemeColors& colors);
        void DrawAvatarCircle(
            float centerX,
            float centerY,
            float radius,
            const std::wstring& avatarPath,
            const std::wstring& fallbackName
        );

        HWND m_hwnd = nullptr;
        ID2D1Factory1* m_d2dFactory = nullptr;
        IDWriteFactory2* m_dwriteFactory = nullptr;
        IWICImagingFactory* m_wicFactory = nullptr;

        ID2D1HwndRenderTarget* m_renderTarget = nullptr;

        // Fonts
        IDWriteTextFormat* m_fontHeading = nullptr;
        IDWriteTextFormat* m_fontBody = nullptr;
        IDWriteTextFormat* m_fontBodyBold = nullptr;
        IDWriteTextFormat* m_fontCaption = nullptr;
        IDWriteTextFormat* m_fontTime = nullptr;

        // Theme brushes
        ID2D1SolidColorBrush* m_brushWindowBg = nullptr;
        ID2D1SolidColorBrush* m_brushSidebarBg = nullptr;
        ID2D1SolidColorBrush* m_brushSidebarHover = nullptr;
        ID2D1SolidColorBrush* m_brushSidebarSelected = nullptr;
        ID2D1SolidColorBrush* m_brushDivider = nullptr;
        ID2D1SolidColorBrush* m_brushTextPrimary = nullptr;
        ID2D1SolidColorBrush* m_brushTextSecondary = nullptr;
        ID2D1SolidColorBrush* m_brushTextMuted = nullptr;
        ID2D1SolidColorBrush* m_brushBubbleInbound = nullptr;
        ID2D1SolidColorBrush* m_brushBubbleInboundText = nullptr;
        ID2D1SolidColorBrush* m_brushBubbleInboundBorder = nullptr;
        ID2D1SolidColorBrush* m_brushBubbleOutbound = nullptr;
        ID2D1SolidColorBrush* m_brushBubbleOutboundText = nullptr;
        ID2D1SolidColorBrush* m_brushAccent = nullptr;

        // Image Cache
        std::unordered_map<std::wstring, ID2D1Bitmap*> m_bitmapCache;
    };

} // namespace KDEConnect
