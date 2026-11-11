#pragma once
#include "common.h"

namespace KDEConnect {

    class ThemeManager {
    public:
        static ThemeManager& Instance();

        void Initialize(HWND mainHwnd);
        void SetMode(ThemeMode mode);
        ThemeMode GetMode() const { return m_currentMode; }

        bool IsDarkActive() const;
        const ThemeColors& GetColors() const;

        void ApplyToWindow(HWND hwnd);
        void HandleSettingChange(HWND hwnd, LPARAM lParam);

        // Callbacks for UI updates when theme changes
        using ThemeChangedCallback = std::function<void(bool isDark, const ThemeColors& colors)>;
        void RegisterCallback(const std::string& key, ThemeChangedCallback callback);
        void UnregisterCallback(const std::string& key);

    private:
        ThemeManager();
        ~ThemeManager() = default;

        bool QuerySystemUsesLightTheme() const;
        void UpdateColors();
        void NotifyCallbacks();

        HWND m_mainHwnd = nullptr;
        ThemeMode m_currentMode = ThemeMode::System;
        bool m_isDarkActive = true;
        ThemeColors m_colors;
        std::vector<std::pair<std::string, ThemeChangedCallback>> m_callbacks;
    };

} // namespace KDEConnect
