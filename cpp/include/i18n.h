#pragma once
#include "common.h"

namespace KDEConnect {

    enum class AppLanguage {
        Auto = 0,
        French = 1,
        English = 2,
        Spanish = 3,
        German = 4,
        Italian = 5,
        Portuguese = 6
    };

    class I18n {
    public:
        static I18n& Instance();

        void Initialize();
        AppLanguage GetConfiguredLanguage() const;
        AppLanguage GetEffectiveLanguage() const;
        void SetLanguage(AppLanguage lang);

        std::wstring Translate(const char* key) const;
        std::wstring TranslateFmt(const char* key, const std::wstring& arg0, const std::wstring& arg1 = L"") const;

        static std::wstring GetLanguageDisplayName(AppLanguage lang);
        static AppLanguage LanguageFromCode(const std::wstring& code);
        static std::wstring CodeFromLanguage(AppLanguage lang);

    private:
        I18n();
        AppLanguage DetectSystemLanguage() const;

        mutable std::mutex m_mutex;
        AppLanguage m_configured = AppLanguage::Auto;
        AppLanguage m_effective = AppLanguage::English;
        bool m_initialized = false;
    };

    inline std::wstring Tr(const char* key) {
        return I18n::Instance().Translate(key);
    }

    inline std::wstring TrFmt(const char* key, const std::wstring& arg0, const std::wstring& arg1 = L"") {
        return I18n::Instance().TranslateFmt(key, arg0, arg1);
    }

} // namespace KDEConnect
