#include "daemon.h"
#include <fstream>
#include <iostream>
#include <filesystem>
#include <iomanip>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <cwctype>

namespace KDEConnect {

    namespace {

        std::string EscapeJson(const std::string& input) {
            std::ostringstream ss;
            for (unsigned char c : input) {
                switch (c) {
                    case '"': ss << "\\\""; break;
                    case '\\': ss << "\\\\"; break;
                    case '\b': ss << "\\b"; break;
                    case '\f': ss << "\\f"; break;
                    case '\n': ss << "\\n"; break;
                    case '\r': ss << "\\r"; break;
                    case '\t': ss << "\\t"; break;
                    default:
                        if (c < 0x20) {
                            ss << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c);
                        } else {
                            ss << static_cast<char>(c);
                        }
                        break;
                }
            }
            return ss.str();
        }

        void AppendUtf8Codepoint(uint32_t cp, std::string& out) {
            if (cp <= 0x7F) {
                out.push_back(static_cast<char>(cp));
            } else if (cp <= 0x7FF) {
                out.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else if (cp <= 0xFFFF) {
                out.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else if (cp <= 0x10FFFF) {
                out.push_back(static_cast<char>(0xF0 | ((cp >> 18) & 0x07)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
        }

        bool ParseHex4(const std::string& s, size_t pos, uint32_t& outVal) {
            if (pos + 4 > s.size()) return false;
            uint32_t val = 0;
            for (size_t i = 0; i < 4; ++i) {
                char ch = s[pos + i];
                val <<= 4;
                if (ch >= '0' && ch <= '9') val |= static_cast<uint32_t>(ch - '0');
                else if (ch >= 'a' && ch <= 'f') val |= static_cast<uint32_t>(ch - 'a' + 10);
                else if (ch >= 'A' && ch <= 'F') val |= static_cast<uint32_t>(ch - 'A' + 10);
                else return false;
            }
            outVal = val;
            return true;
        }

        std::string UnescapeJsonString(const std::string& raw) {
            std::string out;
            out.reserve(raw.size());
            for (size_t i = 0; i < raw.size(); ++i) {
                if (raw[i] == '\\' && i + 1 < raw.size()) {
                    char next = raw[++i];
                    switch (next) {
                        case '"': out.push_back('"'); break;
                        case '\\': out.push_back('\\'); break;
                        case '/': out.push_back('/'); break;
                        case 'b': out.push_back('\b'); break;
                        case 'f': out.push_back('\f'); break;
                        case 'n': out.push_back('\n'); break;
                        case 'r': out.push_back('\r'); break;
                        case 't': out.push_back('\t'); break;
                        case 'u': {
                            uint32_t cp1 = 0;
                            if (ParseHex4(raw, i + 1, cp1)) {
                                i += 4;
                                if (cp1 >= 0xD800 && cp1 <= 0xDBFF &&
                                    i + 6 < raw.size() && raw[i + 1] == '\\' && raw[i + 2] == 'u') {
                                    uint32_t cp2 = 0;
                                    if (ParseHex4(raw, i + 3, cp2) && cp2 >= 0xDC00 && cp2 <= 0xDFFF) {
                                        uint32_t fullCp = 0x10000 + (((cp1 - 0xD800) << 10) | (cp2 - 0xDC00));
                                        AppendUtf8Codepoint(fullCp, out);
                                        i += 6;
                                        break;
                                    }
                                }
                                AppendUtf8Codepoint(cp1, out);
                            } else {
                                out.push_back('u');
                            }
                            break;
                        }
                        default:
                            out.push_back(next);
                            break;
                    }
                } else {
                    out.push_back(raw[i]);
                }
            }
            return out;
        }

        size_t SkipWs(const std::string& s, size_t pos) {
            while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\r' || s[pos] == '\n')) {
                ++pos;
            }
            return pos;
        }

        size_t FindStringEnd(const std::string& s, size_t openQuote) {
            size_t i = openQuote + 1;
            while (i < s.size()) {
                if (s[i] == '\\') {
                    if (i + 1 < s.size()) {
                        i += 2;
                        continue;
                    }
                    break;
                }
                if (s[i] == '"') return i;
                ++i;
            }
            return std::string::npos;
        }

        size_t FindCompoundEnd(const std::string& s, size_t startPos) {
            char openCh = s[startPos];
            char closeCh = (openCh == '{') ? '}' : ']';
            int depth = 0;
            for (size_t i = startPos; i < s.size(); ++i) {
                if (s[i] == '"') {
                    size_t qEnd = FindStringEnd(s, i);
                    if (qEnd == std::string::npos) return std::string::npos;
                    i = qEnd;
                    continue;
                }
                if (s[i] == openCh) ++depth;
                else if (s[i] == closeCh) {
                    --depth;
                    if (depth == 0) return i;
                }
            }
            return std::string::npos;
        }

        // Iterates top-level fields of a JSON object
        template<typename Fn>
        void ForEachJsonObjectField(const std::string& obj, Fn&& callback) {
            size_t pos = SkipWs(obj, 0);
            if (pos >= obj.size() || obj[pos] != '{') return;
            ++pos;

            while (pos < obj.size()) {
                pos = SkipWs(obj, pos);
                if (pos >= obj.size() || obj[pos] == '}') break;
                if (obj[pos] == ',') { ++pos; continue; }
                if (obj[pos] != '"') break;

                size_t keyEnd = FindStringEnd(obj, pos);
                if (keyEnd == std::string::npos) break;
                std::string key = obj.substr(pos + 1, keyEnd - pos - 1);

                pos = SkipWs(obj, keyEnd + 1);
                if (pos >= obj.size() || obj[pos] != ':') break;
                pos = SkipWs(obj, pos + 1);
                if (pos >= obj.size()) break;

                size_t valStart = pos;
                size_t nextPos = pos;

                if (obj[pos] == '"') {
                    size_t valEnd = FindStringEnd(obj, pos);
                    if (valEnd == std::string::npos) break;
                    nextPos = valEnd + 1;
                    std::string rawVal = obj.substr(pos + 1, valEnd - pos - 1);
                    callback(key, rawVal, true);
                } else if (obj[pos] == '{' || obj[pos] == '[') {
                    size_t valEnd = FindCompoundEnd(obj, pos);
                    if (valEnd == std::string::npos) break;
                    nextPos = valEnd + 1;
                    callback(key, obj.substr(valStart, nextPos - valStart), false);
                } else {
                    size_t valEnd = obj.find_first_of(",}\r\n", pos);
                    if (valEnd == std::string::npos) valEnd = obj.size();
                    nextPos = valEnd;
                    std::string rawVal = obj.substr(valStart, valEnd - valStart);
                    while (!rawVal.empty() && (rawVal.back() == ' ' || rawVal.back() == '\t')) rawVal.pop_back();
                    callback(key, rawVal, false);
                }
                pos = nextPos;
            }
        }

        // Iterates elements of a JSON array of objects
        template<typename Fn>
        void ForEachJsonArrayObject(const std::string& arr, Fn&& callback) {
            size_t pos = SkipWs(arr, 0);
            if (pos >= arr.size() || arr[pos] != '[') return;
            ++pos;

            while (pos < arr.size()) {
                pos = SkipWs(arr, pos);
                if (pos >= arr.size() || arr[pos] == ']') break;
                if (arr[pos] == ',') { ++pos; continue; }

                if (arr[pos] == '{') {
                    size_t objEnd = FindCompoundEnd(arr, pos);
                    if (objEnd == std::string::npos) break;
                    callback(arr.substr(pos, objEnd - pos + 1));
                    pos = objEnd + 1;
                } else if (arr[pos] == '[') {
                    size_t end = FindCompoundEnd(arr, pos);
                    if (end == std::string::npos) break;
                    pos = end + 1;
                } else if (arr[pos] == '"') {
                    size_t end = FindStringEnd(arr, pos);
                    if (end == std::string::npos) break;
                    pos = end + 1;
                } else {
                    size_t end = arr.find_first_of(",]\r\n", pos);
                    if (end == std::string::npos) break;
                    pos = end;
                }
            }
        }

        // Iterates string elements inside a JSON array
        template<typename Fn>
        void ForEachJsonArrayString(const std::string& arr, Fn&& callback) {
            size_t pos = SkipWs(arr, 0);
            if (pos >= arr.size() || arr[pos] != '[') return;
            ++pos;

            while (pos < arr.size()) {
                pos = SkipWs(arr, pos);
                if (pos >= arr.size() || arr[pos] == ']') break;
                if (arr[pos] == ',') { ++pos; continue; }

                if (arr[pos] == '"') {
                    size_t end = FindStringEnd(arr, pos);
                    if (end == std::string::npos) break;
                    callback(UnescapeJsonString(arr.substr(pos + 1, end - pos - 1)));
                    pos = end + 1;
                } else if (arr[pos] == '{' || arr[pos] == '[') {
                    size_t end = FindCompoundEnd(arr, pos);
                    if (end == std::string::npos) break;
                    pos = end + 1;
                } else {
                    size_t end = arr.find_first_of(",]\r\n", pos);
                    if (end == std::string::npos) break;
                    pos = end;
                }
            }
        }

        std::string HexEncode(const std::string& bytes) {
            static const char* hex = "0123456789abcdef";
            std::string out;
            out.reserve(bytes.size() * 2);
            for (unsigned char c : bytes) {
                out.push_back(hex[(c >> 4) & 0x0F]);
                out.push_back(hex[c & 0x0F]);
            }
            return out;
        }

        std::string HexDecode(const std::string& hexStr) {
            std::string out;
            out.reserve(hexStr.size() / 2);
            auto nibble = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            for (size_t i = 0; i + 1 < hexStr.size(); i += 2) {
                int h = nibble(hexStr[i]);
                int l = nibble(hexStr[i + 1]);
                if (h < 0 || l < 0) break;
                out.push_back(static_cast<char>((h << 4) | l));
            }
            return out;
        }

        std::wstring NormalizePhoneDigits(const std::wstring& phone) {
            std::wstring digits;
            for (wchar_t ch : phone) {
                if (ch >= L'0' && ch <= L'9') digits.push_back(ch);
            }
            return digits;
        }

        std::wstring NormalizePhoneSuffix(const std::wstring& phone) {
            std::wstring digits = NormalizePhoneDigits(phone);
            if (digits.size() > 9) {
                return digits.substr(digits.size() - 9);
            }
            return digits;
        }

        bool IsMultiRecipientAddress(const std::wstring& addr) {
            return addr.find_first_of(L",;") != std::wstring::npos;
        }

        std::wstring TrimWideCopy(std::wstring s) {
            while (!s.empty() && (s.front() == L' ' || s.front() == L'\t' || s.front() == L'\r' || s.front() == L'\n')) {
                s.erase(s.begin());
            }
            while (!s.empty() && (s.back() == L' ' || s.back() == L'\t' || s.back() == L'\r' || s.back() == L'\n')) {
                s.pop_back();
            }
            return s;
        }

        std::wstring ToLowerWideCopy(std::wstring s) {
            for (auto& ch : s) {
                ch = static_cast<wchar_t>(::towlower(ch));
            }
            return s;
        }

        std::wstring CanonicalSinglePhoneKey(const std::wstring& rawPhone) {
            std::wstring trimmed = TrimWideCopy(rawPhone);
            if (trimmed.empty() || IsMultiRecipientAddress(trimmed)) return L"";

            std::wstring check = trimmed;
            if (check.size() > 4) {
                std::wstring prefix = ToLowerWideCopy(check.substr(0, 4));
                if (prefix == L"tel:") {
                    check = TrimWideCopy(check.substr(4));
                }
            }

            bool hasAlpha = false;
            for (wchar_t ch : check) {
                if ((ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z') || ::iswalpha(ch)) {
                    hasAlpha = true;
                    break;
                }
            }

            if (!hasAlpha) {
                std::wstring digits = NormalizePhoneDigits(check);
                if (!digits.empty()) {
                    if (digits.size() >= 8) {
                        return L"suf:" + (digits.size() > 9 ? digits.substr(digits.size() - 9) : digits);
                    }
                    return L"exact:" + digits;
                }
            }

            return L"text:" + ToLowerWideCopy(check);
        }

        int64_t MakeFallbackThreadId(const std::wstring& phone) {
            std::wstring key = CanonicalSinglePhoneKey(phone);
            size_t h = std::hash<std::wstring>{}(key.empty() ? phone : key);
            return 1000000000LL + static_cast<int64_t>(h % 900000000LL);
        }

        bool IsSyntheticMessageId(int64_t id) {
            // Local/telephony IDs are 13-digit epoch ms (>= 1e12); Android SQLite _id values are much smaller
            return id == 0 || id >= 100000000000LL;
        }

        std::wstring NormalizeBodyForDedup(const std::wstring& body) {
            std::wstring out;
            out.reserve(body.size());
            for (wchar_t ch : body) {
                if (ch != L'\r') out.push_back(ch);
            }
            while (!out.empty() && (out.back() == L' ' || out.back() == L'\t' || out.back() == L'\n')) {
                out.pop_back();
            }
            return out;
        }

        bool AreLogicalDuplicateMessages(const Message& a, const Message& b) {
            if (a.id != 0 && b.id != 0 && a.id == b.id) {
                return true;
            }
            if (a.isOutgoing != b.isOutgoing) {
                return false;
            }
            std::wstring ba = NormalizeBodyForDedup(a.body);
            std::wstring bb = NormalizeBodyForDedup(b.body);
            if (ba != bb) {
                return false;
            }
            if (ba.empty() && a.attachments.size() != b.attachments.size()) {
                return false;
            }
            bool aSynth = IsSyntheticMessageId(a.id);
            bool bSynth = IsSyntheticMessageId(b.id);
            int64_t windowMs = (aSynth != bSynth) ? 7200000LL : 60000LL;
            return std::llabs(a.timestamp - b.timestamp) <= windowMs;
        }

        std::wstring GuessMimeTypeWide(const std::wstring& filePath) {
            std::wstring ext = std::filesystem::path(filePath).extension().wstring();
            for (auto& ch : ext) ch = static_cast<wchar_t>(::towlower(ch));
            if (ext == L".jpg" || ext == L".jpeg") return L"image/jpeg";
            if (ext == L".png") return L"image/png";
            if (ext == L".gif") return L"image/gif";
            if (ext == L".webp") return L"image/webp";
            if (ext == L".bmp") return L"image/bmp";
            if (ext == L".heic") return L"image/heic";
            return L"application/octet-stream";
        }

        std::wstring ExtensionFromMimeWide(const std::wstring& mime) {
            std::wstring m = mime;
            for (auto& ch : m) ch = static_cast<wchar_t>(::towlower(ch));
            if (m.find(L"png") != std::wstring::npos) return L".png";
            if (m.find(L"gif") != std::wstring::npos) return L".gif";
            if (m.find(L"webp") != std::wstring::npos) return L".webp";
            if (m.find(L"bmp") != std::wstring::npos) return L".bmp";
            return L".jpg";
        }

        std::vector<uint8_t> Base64DecodeBytes(const std::string& b64) {
            static const int kLookup[256] = {
                -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
                52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
                -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
                15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
                -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
                41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
                -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
            };
            std::vector<uint8_t> out;
            out.reserve((b64.size() * 3) / 4);
            uint32_t val = 0;
            int valb = -8;
            for (unsigned char c : b64) {
                if (c == '=') break;
                int d = kLookup[c];
                if (d == -1) continue;
                val = (val << 6) + static_cast<uint32_t>(d);
                valb += 6;
                if (valb >= 0) {
                    out.push_back(static_cast<uint8_t>((val >> valb) & 0xFF));
                    valb -= 8;
                }
            }
            return out;
        }

        std::string DecodeQuotedPrintable(const std::string& raw) {
            std::string out;
            out.reserve(raw.size());
            auto hexVal = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            for (size_t i = 0; i < raw.size(); ++i) {
                if (raw[i] == '=' && i + 2 < raw.size()) {
                    int h = hexVal(raw[i + 1]);
                    int l = hexVal(raw[i + 2]);
                    if (h >= 0 && l >= 0) {
                        out.push_back(static_cast<char>((h << 4) | l));
                        i += 2;
                        continue;
                    }
                }
                out.push_back(raw[i]);
            }
            return out;
        }

        std::string UnescapeVCardValue(const std::string& raw) {
            std::string out;
            out.reserve(raw.size());
            for (size_t i = 0; i < raw.size(); ++i) {
                if (raw[i] == '\\' && i + 1 < raw.size()) {
                    char next = raw[++i];
                    if (next == 'n' || next == 'N') out.push_back(' ');
                    else out.push_back(next);
                } else {
                    out.push_back(raw[i]);
                }
            }
            return out;
        }

        std::string TrimAscii(std::string s) {
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) s.erase(s.begin());
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.pop_back();
            return s;
        }

        std::wstring VCardBytesToWide(const std::string& raw, bool preferLatin1) {
            if (raw.empty()) return L"";
            if (!preferLatin1) {
                int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw.data(), static_cast<int>(raw.size()), nullptr, 0);
                if (len > 0) {
                    std::wstring out(static_cast<size_t>(len), L'\0');
                    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw.data(), static_cast<int>(raw.size()), &out[0], len);
                    return out;
                }
            }
            UINT cp = preferLatin1 ? 1252u : CP_ACP;
            int len = MultiByteToWideChar(cp, 0, raw.data(), static_cast<int>(raw.size()), nullptr, 0);
            if (len <= 0) return Utf8ToWide(raw);
            std::wstring out(static_cast<size_t>(len), L'\0');
            MultiByteToWideChar(cp, 0, raw.data(), static_cast<int>(raw.size()), &out[0], len);
            return out;
        }

        std::wstring DetectImageExtFromBytes(const std::vector<uint8_t>& bytes, const std::string& upperHeader) {
            if (bytes.size() >= 8 && bytes[0] == 0x89 && bytes[1] == 0x50 && bytes[2] == 0x4E && bytes[3] == 0x47) {
                return L".png";
            }
            if (bytes.size() >= 4 && bytes[0] == 'G' && bytes[1] == 'I' && bytes[2] == 'F') {
                return L".gif";
            }
            if (bytes.size() >= 12 && bytes[0] == 'R' && bytes[1] == 'I' && bytes[2] == 'F' && bytes[3] == 'F' &&
                bytes[8] == 'W' && bytes[9] == 'E' && bytes[10] == 'B' && bytes[11] == 'P') {
                return L".webp";
            }
            if (bytes.size() >= 2 && bytes[0] == 'B' && bytes[1] == 'M') {
                return L".bmp";
            }
            if (bytes.size() >= 3 && bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF) {
                return L".jpg";
            }
            if (upperHeader.find("PNG") != std::string::npos) return L".png";
            if (upperHeader.find("GIF") != std::string::npos) return L".gif";
            if (upperHeader.find("WEBP") != std::string::npos) return L".webp";
            return L".jpg";
        }

        std::wstring SaveContactAvatarToDisk(const std::string& uid, const std::vector<uint8_t>& bytes, const std::wstring& ext) {
            if (bytes.size() < 32) return L"";
            std::wstring avatarDir = GetAppDataDirectory() + L"\\ContactAvatars";
            SHCreateDirectoryExW(nullptr, avatarDir.c_str(), nullptr);

            std::wstring safeUid;
            for (unsigned char ch : uid) {
                if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-') {
                    safeUid.push_back(static_cast<wchar_t>(ch));
                }
            }
            if (safeUid.empty()) {
                safeUid = Utf8ToWide(HexEncode(uid));
            }
            if (safeUid.size() > 48) {
                safeUid.resize(48);
            }

            uint32_t hash = 2166136261u;
            for (uint8_t b : bytes) {
                hash ^= b;
                hash *= 16777619u;
            }
            wchar_t hexBuf[9]{};
            static const wchar_t* kHexDigits = L"0123456789abcdef";
            for (int shift = 28, idx = 0; shift >= 0; shift -= 4, ++idx) {
                hexBuf[idx] = kHexDigits[(hash >> shift) & 0x0Fu];
            }

            std::wstring prefix = L"avatar_" + safeUid + L"_";
            std::wstring fileName = prefix + hexBuf + (ext.empty() ? L".jpg" : ext);
            std::wstring destPath = avatarDir + L"\\" + fileName;

            try {
                std::error_code ec;
                if (!std::filesystem::exists(std::filesystem::path(destPath), ec)) {
                    for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::path(avatarDir), ec)) {
                        if (!entry.is_regular_file(ec)) continue;
                        std::wstring existingName = entry.path().filename().wstring();
                        if (existingName.rfind(prefix, 0) == 0 && existingName != fileName) {
                            std::filesystem::remove(entry.path(), ec);
                        }
                    }
                    std::ofstream out(std::filesystem::path(destPath), std::ios::out | std::ios::binary | std::ios::trunc);
                    if (out.is_open()) {
                        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                        out.close();
                    }
                }
                if (std::filesystem::exists(std::filesystem::path(destPath), ec)) {
                    return destPath;
                }
            } catch (...) {
                return L"";
            }
            return L"";
        }

        bool HeaderHasQuotedPrintable(const std::string& line) {
            size_t colonPos = line.find(':');
            if (colonPos == std::string::npos) return false;
            std::string upperHdr = line.substr(0, colonPos);
            for (char& ch : upperHdr) ch = static_cast<char>(::toupper(static_cast<unsigned char>(ch)));
            return upperHdr.find("QUOTED-PRINTABLE") != std::string::npos;
        }

        void ParseVCardFields(
            const std::string& vcardRaw,
            std::wstring& outName,
            std::vector<std::wstring>& outPhones,
            std::vector<uint8_t>* outPhotoBytes = nullptr,
            std::wstring* outPhotoExt = nullptr
        ) {
            outName.clear();
            outPhones.clear();
            if (outPhotoBytes) outPhotoBytes->clear();
            if (outPhotoExt) outPhotoExt->clear();

            // Normalize CRLF -> LF first
            std::string normalized;
            normalized.reserve(vcardRaw.size());
            for (size_t i = 0; i < vcardRaw.size(); ++i) {
                if (vcardRaw[i] == '\r') {
                    if (i + 1 < vcardRaw.size() && vcardRaw[i + 1] == '\n') ++i;
                    normalized.push_back('\n');
                } else {
                    normalized.push_back(vcardRaw[i]);
                }
            }

            // Unfold continuation lines (\n + space/tab, and quoted-printable soft breaks =\n ONLY on QP headers)
            std::vector<std::string> lines;
            std::istringstream ls(normalized);
            std::string rawLine;
            bool prevEndsSoftBreak = false;
            while (std::getline(ls, rawLine)) {
                if (!rawLine.empty() && (rawLine[0] == ' ' || rawLine[0] == '\t') && !lines.empty()) {
                    lines.back() += rawLine.substr(1);
                } else if (prevEndsSoftBreak && !lines.empty()) {
                    lines.back() += rawLine;
                } else {
                    lines.push_back(rawLine);
                }
                prevEndsSoftBreak = (!lines.empty() && !lines.back().empty() && lines.back().back() == '=' && HeaderHasQuotedPrintable(lines.back()));
                if (prevEndsSoftBreak) {
                    lines.back().pop_back();
                }
            }

            std::wstring fallbackStructuredName;
            std::wstring fallbackOrgName;
            for (const auto& line : lines) {
                size_t colonPos = line.find(':');
                if (colonPos == std::string::npos) continue;

                std::string header = line.substr(0, colonPos);
                std::string value = line.substr(colonPos + 1);

                std::string upperHeader = header;
                for (char& ch : upperHeader) ch = static_cast<char>(::toupper(static_cast<unsigned char>(ch)));

                // Strip optional "itemN." prefix
                size_t dotPos = upperHeader.find('.');
                std::string propAndParams = (dotPos != std::string::npos) ? upperHeader.substr(dotPos + 1) : upperHeader;
                size_t semiPos = propAndParams.find(';');
                std::string propName = (semiPos != std::string::npos) ? propAndParams.substr(0, semiPos) : propAndParams;

                bool isQuotedPrintable = (upperHeader.find("QUOTED-PRINTABLE") != std::string::npos);
                bool preferLatin1 = (upperHeader.find("ISO-8859-1") != std::string::npos ||
                                     upperHeader.find("WINDOWS-1252") != std::string::npos ||
                                     upperHeader.find("LATIN1") != std::string::npos);
                if (isQuotedPrintable) {
                    value = DecodeQuotedPrintable(value);
                }

                if (propName == "FN") {
                    std::string clean = TrimAscii(UnescapeVCardValue(value));
                    if (!clean.empty()) {
                        outName = VCardBytesToWide(clean, preferLatin1);
                    }
                } else if (propName == "N" && fallbackStructuredName.empty()) {
                    // Format: Family;Given;Middle;Prefix;Suffix
                    std::vector<std::string> parts;
                    std::stringstream ss(value);
                    std::string item;
                    while (std::getline(ss, item, ';')) {
                        parts.push_back(TrimAscii(UnescapeVCardValue(item)));
                    }
                    std::string family = parts.size() > 0 ? parts[0] : "";
                    std::string given = parts.size() > 1 ? parts[1] : "";
                    std::string assembled = TrimAscii((!given.empty() && !family.empty()) ? (given + " " + family) : (given + family));
                    if (!assembled.empty()) {
                        fallbackStructuredName = VCardBytesToWide(assembled, preferLatin1);
                    }
                } else if (propName == "ORG" && fallbackOrgName.empty()) {
                    std::string org = value;
                    for (char& ch : org) {
                        if (ch == ';') ch = ' ';
                    }
                    org = TrimAscii(UnescapeVCardValue(org));
                    if (!org.empty()) {
                        fallbackOrgName = VCardBytesToWide(org, preferLatin1);
                    }
                } else if (propName == "TEL") {
                    std::string cleanTel = TrimAscii(UnescapeVCardValue(value));
                    if (cleanTel.size() > 4) {
                        std::string prefix = cleanTel.substr(0, 4);
                        for (char& ch : prefix) ch = static_cast<char>(::tolower(static_cast<unsigned char>(ch)));
                        if (prefix == "tel:") cleanTel = TrimAscii(cleanTel.substr(4));
                    }
                    if (!cleanTel.empty()) {
                        outPhones.push_back(VCardBytesToWide(cleanTel, preferLatin1));
                    }
                } else if (propName == "PHOTO" && outPhotoBytes && outPhotoBytes->empty()) {
                    std::string lowerVal = value;
                    for (char& ch : lowerVal) ch = static_cast<char>(::tolower(static_cast<unsigned char>(ch)));
                    size_t b64Pos = lowerVal.find("base64,");
                    bool hasBase64Hdr = (upperHeader.find("BASE64") != std::string::npos ||
                                         upperHeader.find("ENCODING=B") != std::string::npos);
                    if (b64Pos != std::string::npos || hasBase64Hdr) {
                        std::string b64Slice = (b64Pos != std::string::npos) ? value.substr(b64Pos + 7) : value;
                        std::vector<uint8_t> decoded = Base64DecodeBytes(b64Slice);
                        if (decoded.size() >= 32) {
                            if (outPhotoExt) {
                                *outPhotoExt = DetectImageExtFromBytes(decoded, upperHeader);
                            }
                            *outPhotoBytes = std::move(decoded);
                        }
                    }
                }
            }

            if (outName.empty()) {
                if (!fallbackStructuredName.empty()) {
                    outName = fallbackStructuredName;
                } else if (!fallbackOrgName.empty()) {
                    outName = fallbackOrgName;
                }
            }
        }

    } // namespace

    SmsDaemon& SmsDaemon::Instance() {
        static SmsDaemon instance;
        return instance;
    }

    SmsDaemon::SmsDaemon() {
    }

    SmsDaemon::~SmsDaemon() {
        Stop();
    }

    void SmsDaemon::SetOnConversationsUpdated(ConversationsUpdatedCallback cb) {
        std::lock_guard<std::mutex> lock(m_callbackMutex);
        m_onConversationsUpdated = std::move(cb);
    }

    void SmsDaemon::NotifyConversationsUpdated() {
        ConversationsUpdatedCallback cb;
        {
            std::lock_guard<std::mutex> lock(m_callbackMutex);
            cb = m_onConversationsUpdated;
        }
        if (cb) cb();
    }

    void SmsDaemon::ReloadStorageDirectory() {
        std::wstring newDir = GetMessagesDirectory();
        bool dirChanged = false;
        {
            std::lock_guard<std::mutex> lock(m_storageMutex);
            dirChanged = (newDir != m_watchDir);
            if (dirChanged) {
                m_watchDir = newDir;
                m_requestedThreads.clear();
                LoadContactsCacheLocked();
                LoadDeletedThreadsLocked();
                LoadAllConversationsFromDiskLocked();
                ApplyContactsToConversationsLocked();
            }
        }
        if (dirChanged && m_running) {
            if (m_dirHandle != INVALID_HANDLE_VALUE) {
                CancelIoEx(m_dirHandle, nullptr);
                CloseHandle(m_dirHandle);
                m_dirHandle = INVALID_HANDLE_VALUE;
            }
            if (m_watcherThread.joinable()) {
                m_watcherThread.join();
            }
            m_dirHandle = CreateFileW(
                newDir.c_str(),
                FILE_LIST_DIRECTORY,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr,
                OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS,
                nullptr
            );
            m_watcherThread = std::thread(&SmsDaemon::DirectoryWatcherThread, this);
        }
        if (dirChanged) {
            NotifyConversationsUpdated();
        }
    }

    static std::wstring NormalizeBindIpForCompare(const std::wstring& ip) {
        std::wstring clean = TrimWideCopy(ip);
        if (clean == L"0.0.0.0") clean.clear();
        return clean;
    }

    static std::string ExtractSimpleJsonStringField(const std::string& json, const std::string& field) {
        std::string needle = "\"" + field + "\":\"";
        size_t pos = json.find(needle);
        if (pos == std::string::npos) return "";
        size_t start = pos + needle.size();
        size_t end = json.find('"', start);
        if (end == std::string::npos) return "";
        return json.substr(start, end - start);
    }

    bool SmsDaemon::IsDaemonIpcAvailable() const {
        if (WaitNamedPipeW(IPC_PIPE_NAME, 150)) {
            return true;
        }
        DWORD err = GetLastError();
        return (err == ERROR_PIPE_BUSY);
    }

    void SmsDaemon::RefreshClientStatusFromDaemon() {
        std::string resp;
        if (!SendIpcCommand("STATUS\n", &resp)) return;
        std::string bound = ExtractSimpleJsonStringField(resp, "bound");
        std::string errHex = ExtractSimpleJsonStringField(resp, "error");
        std::lock_guard<std::mutex> lock(m_netStatusMutex);
        if (!bound.empty()) {
            m_clientBoundEndpoint = Utf8ToWide(bound);
        }
        if (!errHex.empty()) {
            m_lastBindError = Utf8ToWide(HexDecode(errHex));
        } else if (!bound.empty()) {
            m_lastBindError.clear();
        }
    }

    std::wstring SmsDaemon::GetBoundEndpointSummary() const {
        if (m_isClientMode) {
            std::lock_guard<std::mutex> lock(m_netStatusMutex);
            return m_clientBoundEndpoint;
        }
        return m_protocol.GetBoundEndpointSummary();
    }

    std::wstring SmsDaemon::GetLastBindError() const {
        std::lock_guard<std::mutex> lock(m_netStatusMutex);
        if (!m_lastBindError.empty()) {
            return m_lastBindError;
        }
        return m_protocol.GetLastBindError();
    }

    bool SmsDaemon::ApplySettingsReload(const std::wstring& newBindIp, const std::wstring& newDeviceName) {
        GetReceivedFilesDirectory();
        ReloadStorageDirectory();

        std::wstring cleanBindIp = NormalizeBindIpForCompare(newBindIp);
        if (m_isClientMode) {
            std::string cmd = "RELOAD_CONFIG|" + HexEncode(WideToUtf8(cleanBindIp)) + "|" +
                              HexEncode(WideToUtf8(newDeviceName)) + "\n";
            std::string resp;
            if (!SendIpcCommand(cmd, &resp)) {
                std::lock_guard<std::mutex> lock(m_netStatusMutex);
                m_lastBindError = L"Impossible de joindre KDEConnectDaemon via IPC.";
                return false;
            }
            std::string status = ExtractSimpleJsonStringField(resp, "status");
            std::string bound = ExtractSimpleJsonStringField(resp, "bound");
            std::string errHex = ExtractSimpleJsonStringField(resp, "error");
            {
                std::lock_guard<std::mutex> lock(m_netStatusMutex);
                m_clientBoundEndpoint = Utf8ToWide(bound);
                if (status == "error") {
                    m_lastBindError = errHex.empty()
                        ? L"Echec de liaison reseau sur le service Daemon."
                        : Utf8ToWide(HexDecode(errHex));
                } else {
                    m_lastBindError.clear();
                }
            }
            return status != "error";
        }

        m_protocol.UpdateDeviceName(newDeviceName);
        std::wstring currentEffective = NormalizeBindIpForCompare(m_protocol.GetEffectiveBindIp());
        if (m_running && (!m_protocol.IsListening() || cleanBindIp != currentEffective)) {
            m_protocol.Stop();
            bool started = m_protocol.Start(cleanBindIp);
            if (!started) {
                std::lock_guard<std::mutex> lock(m_netStatusMutex);
                m_lastBindError = m_protocol.GetLastBindError();
                return false;
            }
            {
                std::lock_guard<std::mutex> lock(m_netStatusMutex);
                m_lastBindError.clear();
            }
            return true;
        }
        return m_protocol.IsListening();
    }

    bool SmsDaemon::Start(const std::wstring& bindIp) {
        if (m_running) return true;

        m_running = true;
        m_startupEpochMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        {
            std::lock_guard<std::mutex> lock(m_storageMutex);
            m_watchDir = GetMessagesDirectory();
        }

        // Open directory handle for ReadDirectoryChangesW
        m_dirHandle = CreateFileW(
            m_watchDir.c_str(),
            FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS,
            nullptr
        );

        if (m_dirHandle == INVALID_HANDLE_VALUE) {
            std::wcerr << L"[Daemon] Avertissement: impossible d'ouvrir le dossier " << m_watchDir << std::endl;
        }

        // Preload cached contacts, deleted threads & synced threads from disk immediately
        {
            std::lock_guard<std::mutex> lock(m_storageMutex);
            LoadContactsCacheLocked();
            LoadDeletedThreadsLocked();
            LoadAllConversationsFromDiskLocked();
            ApplyContactsToConversationsLocked();
        }

        // Hook protocol packet and ready callbacks
        m_protocol.SetOnPacketReceived([this](const DeviceInfo& dev, const KDEPacket& pkt) {
            HandleIncomingKdePacket(dev, pkt);
        });

        m_protocol.SetOnDeviceReady([this](const DeviceInfo& dev) {
            std::wcout << L"[SMS Sync] Appareil pret (" << (dev.deviceName.empty() ? dev.ipAddress : dev.deviceName)
                       << L") -> Demande des contacts et conversations SMS..." << std::endl;
            m_protocol.RequestAllContacts(dev);
            m_protocol.RequestAllConversations(dev);
        });

        // Start protocol handler on UDP 1716 and TCP 1716.
        // Only enter IPC Client Mode if KDEConnectDaemon's Named Pipe is actually present.
        bool protocolStarted = m_protocol.Start(bindIp);
        if (!protocolStarted && IsDaemonIpcAvailable()) {
            m_isClientMode = true;
            {
                std::lock_guard<std::mutex> lock(m_netStatusMutex);
                m_lastBindError.clear();
            }
            std::wcout << L"[IPC Client] Port 1716 occupe par KDEConnectDaemon. Mode client IPC active." << std::endl;
        } else {
            m_isClientMode = false;
            if (!protocolStarted) {
                std::lock_guard<std::mutex> lock(m_netStatusMutex);
                m_lastBindError = m_protocol.GetLastBindError();
            } else {
                std::lock_guard<std::mutex> lock(m_netStatusMutex);
                m_lastBindError.clear();
            }
            m_ipcThread = std::thread(&SmsDaemon::NamedPipeIpcThread, this);
        }

        m_watcherThread = std::thread(&SmsDaemon::DirectoryWatcherThread, this);

        // If in client mode, query daemon status and kick off a SYNC command
        if (m_isClientMode) {
            std::thread([this]() {
                if (m_running) {
                    RefreshClientStatusFromDaemon();
                    SendIpcCommand("SYNC\n");
                    NotifyConversationsUpdated();
                }
            }).detach();
        }

        return true;
    }

    void SmsDaemon::Stop() {
        if (!m_running) return;

        m_running = false;

        if (m_dirHandle != INVALID_HANDLE_VALUE) {
            CancelIoEx(m_dirHandle, nullptr);
            CloseHandle(m_dirHandle);
            m_dirHandle = INVALID_HANDLE_VALUE;
        }

        if (!m_isClientMode) {
            m_protocol.Stop();

            // Wake up blocking ConnectNamedPipe if waiting
            HANDLE wakePipe = CreateFileW(
                IPC_PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (wakePipe != INVALID_HANDLE_VALUE) {
                CloseHandle(wakePipe);
            }
        }

        if (m_watcherThread.joinable()) m_watcherThread.join();
        if (m_ipcThread.joinable()) m_ipcThread.join();
    }

    void SmsDaemon::DirectoryWatcherThread() {
        alignas(DWORD) char buffer[4096];
        DWORD bytesReturned = 0;

        while (m_running && m_dirHandle != INVALID_HANDLE_VALUE) {
            BOOL success = ReadDirectoryChangesW(
                m_dirHandle,
                buffer,
                sizeof(buffer),
                FALSE,
                FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE,
                &bytesReturned,
                nullptr,
                nullptr
            );

            if (!success || bytesReturned == 0 || !m_running) {
                break;
            }

            bool hasJsonChange = false;
            FILE_NOTIFY_INFORMATION* fni = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buffer);
            while (fni) {
                std::wstring fileName(fni->FileName, fni->FileNameLength / sizeof(wchar_t));
                if (fileName.ends_with(L".json")) {
                    hasJsonChange = true;
                }

                if (fni->NextEntryOffset == 0) break;
                fni = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(reinterpret_cast<char*>(fni) + fni->NextEntryOffset);
            }

            if (hasJsonChange && m_running) {
                int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                if (!m_isClientMode && nowMs < m_ignoreWatcherUntilMs.load()) {
                    // Skip self-triggered write notifications in Host mode (already notified directly)
                    continue;
                }
                // Debounce bursts so disk atomic renames settle
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                ProcessFolderChangeNotification();
            }
        }
    }

    void SmsDaemon::MaybeNotifyIncomingMessage(const Message& msg, const std::wstring& resolvedName) {
        if (msg.isOutgoing) return;
        if (!ToastNotifier::Instance().HasTrayWindow()) return;

        int64_t nowSteadyMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        std::wstring sig = NormalizePhoneSuffix(msg.address) + L"|" + msg.body;
        if (sig == L"|") {
            sig = std::to_wstring(msg.id);
        }

        {
            std::lock_guard<std::mutex> lock(m_notifyMutex);
            for (auto it = m_recentNotifiedSignatures.begin(); it != m_recentNotifiedSignatures.end();) {
                if (nowSteadyMs - it->second > 60000) {
                    it = m_recentNotifiedSignatures.erase(it);
                } else {
                    ++it;
                }
            }
            if (msg.id != 0 && m_notifiedMsgIds.find(msg.id) != m_notifiedMsgIds.end()) {
                return;
            }
            if (m_recentNotifiedSignatures.find(sig) != m_recentNotifiedSignatures.end()) {
                return;
            }
            m_recentNotifiedSignatures[sig] = nowSteadyMs;
            if (msg.id != 0) {
                m_notifiedMsgIds.insert(msg.id);
            }
        }

        std::wstring title = resolvedName.empty() ? msg.address : resolvedName;
        if (title.empty()) title = L"Nouveau SMS";
        std::wstring body = msg.body;
        if (body.empty()) {
            body = msg.attachments.empty() ? L"Nouveau message recu" : L"[Piece jointe MMS]";
        }
        ToastNotifier::Instance().ShowSmsNotification(title, body, msg.threadId);
    }

    void SmsDaemon::ProcessFolderChangeNotification() {
        std::vector<std::pair<Message, std::wstring>> pendingToNotify;
        int64_t nowEpochMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        {
            std::lock_guard<std::mutex> lock(m_storageMutex);
            std::set<std::pair<int64_t, int64_t>> knownMsgIds;
            if (m_isClientMode) {
                for (const auto& kv : m_conversations) {
                    for (const auto& m : kv.second.messages) {
                        knownMsgIds.emplace(kv.first, m.id);
                    }
                }
            }

            LoadContactsCacheLocked();
            LoadDeletedThreadsLocked();
            LoadAllConversationsFromDiskLocked();
            ApplyContactsToConversationsLocked();

            if (m_isClientMode) {
                for (const auto& kv : m_conversations) {
                    for (const auto& m : kv.second.messages) {
                        if (m.isOutgoing) continue;
                        if (knownMsgIds.find(std::make_pair(kv.first, m.id)) != knownMsgIds.end()) continue;
                        bool recent = (m.timestamp >= m_startupEpochMs - 60000) &&
                                      (std::llabs(nowEpochMs - m.timestamp) <= 600000);
                        if (recent) {
                            pendingToNotify.emplace_back(m, kv.second.contactName);
                        }
                    }
                }
            }
        }

        NotifyConversationsUpdated();

        size_t notifiedCount = 0;
        for (const auto& item : pendingToNotify) {
            if (notifiedCount >= 3) break;
            MaybeNotifyIncomingMessage(item.first, item.second);
            ++notifiedCount;
        }
    }

    bool SmsDaemon::SendIpcCommand(const std::string& command, std::string* outResponse) {
        if (!WaitNamedPipeW(IPC_PIPE_NAME, 1200)) {
            return false;
        }
        HANDLE pipe = CreateFileW(
            IPC_PIPE_NAME,
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr
        );
        if (pipe == INVALID_HANDLE_VALUE) {
            return false;
        }

        DWORD mode = PIPE_READMODE_MESSAGE;
        SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);

        DWORD bytesWritten = 0;
        BOOL wrote = WriteFile(pipe, command.c_str(), static_cast<DWORD>(command.size()), &bytesWritten, nullptr);

        if (wrote && outResponse) {
            char buf[2048];
            DWORD bytesRead = 0;
            if (ReadFile(pipe, buf, sizeof(buf) - 1, &bytesRead, nullptr)) {
                buf[bytesRead] = '\0';
                *outResponse = buf;
            }
        }

        CloseHandle(pipe);
        return wrote == TRUE;
    }

    void SmsDaemon::NamedPipeIpcThread() {
        while (m_running) {
            HANDLE pipe = CreateNamedPipeW(
                IPC_PIPE_NAME,
                PIPE_ACCESS_DUPLEX,
                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                PIPE_UNLIMITED_INSTANCES,
                65536,
                65536,
                0,
                nullptr
            );

            if (pipe == INVALID_HANDLE_VALUE) break;

            BOOL connected = ConnectNamedPipe(pipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
            if (connected && m_running) {
                std::vector<char> readBuffer(65536, 0);
                DWORD bytesRead = 0;
                if (ReadFile(pipe, readBuffer.data(), static_cast<DWORD>(readBuffer.size() - 1), &bytesRead, nullptr)) {
                    readBuffer[bytesRead] = '\0';
                    std::string cmd(readBuffer.data(), bytesRead);
                    while (!cmd.empty() && (cmd.back() == '\r' || cmd.back() == '\n')) cmd.pop_back();

                    std::string response = "{\"status\":\"ok\"}\n";

                    bool triggerReload = false;
                    std::wstring reloadBindIp;
                    std::wstring reloadDeviceName;
                    bool hasReloadArgs = false;
                    if (cmd == "SYNC") {
                        bool ok = RequestConversationsSync();
                        response = ok ? "{\"status\":\"ok\"}\n" : "{\"status\":\"no_device\"}\n";
                    } else if (cmd == "RELOAD_CONFIG" || cmd.rfind("RELOAD_CONFIG|", 0) == 0) {
                        triggerReload = true;
                        if (cmd.rfind("RELOAD_CONFIG|", 0) == 0) {
                            size_t sep1 = cmd.find('|', 14);
                            if (sep1 != std::string::npos) {
                                reloadBindIp = Utf8ToWide(HexDecode(cmd.substr(14, sep1 - 14)));
                                reloadDeviceName = Utf8ToWide(HexDecode(cmd.substr(sep1 + 1)));
                                hasReloadArgs = true;
                            }
                        }
                    } else if (cmd.rfind("SYNC_THREAD:", 0) == 0) {
                        int64_t tid = std::strtoll(cmd.substr(12).c_str(), nullptr, 10);
                        bool ok = RequestThreadSync(tid);
                        response = ok ? "{\"status\":\"ok\"}\n" : "{\"status\":\"no_device\"}\n";
                    } else if (cmd.rfind("DELETE_THREAD:", 0) == 0) {
                        int64_t tid = std::strtoll(cmd.substr(14).c_str(), nullptr, 10);
                        DeleteConversation(tid);
                        response = "{\"status\":\"ok\"}\n";
                    } else if (cmd.rfind("SEND_SMS|", 0) == 0) {
                        size_t sep1 = cmd.find('|', 9);
                        if (sep1 != std::string::npos) {
                            std::string phoneHex = cmd.substr(9, sep1 - 9);
                            std::string bodyHex = cmd.substr(sep1 + 1);
                            std::wstring phone = Utf8ToWide(HexDecode(phoneHex));
                            std::wstring body = Utf8ToWide(HexDecode(bodyHex));
                            bool sentOk = (!phone.empty() && !body.empty()) && m_protocol.SendSms(DeviceInfo{}, phone, body);
                            response = sentOk ? "{\"status\":\"ok\"}\n" : "{\"status\":\"error\"}\n";
                        } else {
                            response = "{\"status\":\"error\"}\n";
                        }
                    } else if (cmd.rfind("SEND_MMS|", 0) == 0) {
                        size_t sep1 = cmd.find('|', 9);
                        size_t sep2 = (sep1 != std::string::npos) ? cmd.find('|', sep1 + 1) : std::string::npos;
                        if (sep1 != std::string::npos && sep2 != std::string::npos) {
                            std::wstring phone = Utf8ToWide(HexDecode(cmd.substr(9, sep1 - 9)));
                            std::wstring body = Utf8ToWide(HexDecode(cmd.substr(sep1 + 1, sep2 - sep1 - 1)));
                            std::wstring filePath = Utf8ToWide(HexDecode(cmd.substr(sep2 + 1)));
                            std::vector<Attachment> atts;
                            if (!filePath.empty()) {
                                Attachment att;
                                att.localFilePath = filePath;
                                att.fileName = std::filesystem::path(filePath).filename().wstring();
                                att.mimeType = GuessMimeTypeWide(filePath);
                                atts.push_back(std::move(att));
                            }
                            bool sentOk = (!phone.empty() && (!body.empty() || !atts.empty())) &&
                                          m_protocol.SendSms(DeviceInfo{}, phone, body, atts);
                            response = sentOk ? "{\"status\":\"ok\"}\n" : "{\"status\":\"error\"}\n";
                        } else {
                            response = "{\"status\":\"error\"}\n";
                        }
                    } else if (cmd == "STATUS") {
                        auto connectedDevs = m_protocol.GetConnectedDevices();
                        std::string bound = WideToUtf8(m_protocol.GetBoundEndpointSummary());
                        std::string errHex = HexEncode(WideToUtf8(m_protocol.GetLastBindError()));
                        std::ostringstream ss;
                        ss << "{\"status\":\"ok\",\"connected\":" << connectedDevs.size()
                           << ",\"bound\":\"" << bound << "\""
                           << ",\"error\":\"" << errHex << "\"}\n";
                        response = ss.str();
                    }

                    if (triggerReload) {
                        bool reloadOk = ApplySettingsReload(
                            hasReloadArgs ? reloadBindIp : ReadIniString(L"Network", L"BindIp", L""),
                            (hasReloadArgs && !reloadDeviceName.empty()) ? reloadDeviceName : GetConfiguredDeviceName()
                        );
                        std::string bound = WideToUtf8(m_protocol.GetBoundEndpointSummary());
                        std::string errHex = HexEncode(WideToUtf8(GetLastBindError()));
                        std::ostringstream ss;
                        ss << "{\"status\":\"" << (reloadOk ? "ok" : "error") << "\""
                           << ",\"bound\":\"" << bound << "\""
                           << ",\"error\":\"" << errHex << "\"}\n";
                        response = ss.str();
                    }

                    DWORD bytesWritten = 0;
                    WriteFile(pipe, response.c_str(), static_cast<DWORD>(response.length()), &bytesWritten, nullptr);
                    DisconnectNamedPipe(pipe);
                    CloseHandle(pipe);
                    pipe = INVALID_HANDLE_VALUE;
                    continue;
                }
                DisconnectNamedPipe(pipe);
            }
            if (pipe != INVALID_HANDLE_VALUE) {
                CloseHandle(pipe);
            }
        }
    }

    bool SmsDaemon::RequestConversationsSync() {
        if (m_isClientMode) {
            return SendIpcCommand("SYNC\n");
        }
        auto devs = m_protocol.GetConnectedDevices();
        if (devs.empty()) return false;
        m_protocol.RequestAllContacts(devs.front());
        return m_protocol.RequestAllConversations(devs.front());
    }

    bool SmsDaemon::RequestThreadSync(int64_t threadId) {
        if (threadId <= 0) return false;
        if (m_isClientMode) {
            return SendIpcCommand("SYNC_THREAD:" + std::to_string(threadId) + "\n");
        }
        auto devs = m_protocol.GetConnectedDevices();
        if (devs.empty()) return false;
        return m_protocol.RequestConversationMessages(devs.front(), threadId, -1);
    }

    std::wstring SmsDaemon::GetConnectionStatusText() const {
        if (m_isClientMode) {
            std::wstring bound = GetBoundEndpointSummary();
            std::wstring err = GetLastBindError();
            if (!err.empty()) {
                return L"Service Daemon (IPC) - " + err;
            }
            if (!bound.empty()) {
                return L"Connecte au service Daemon (IPC actif - " + bound + L")";
            }
            return L"Connecte au service Daemon (IPC actif)";
        }

        if (!m_protocol.IsListening()) {
            std::wstring err = GetLastBindError();
            return err.empty() ? L"Ecoute reseau inactive (verifiez l'IP dans Reglages)" : err;
        }

        std::wstring bound = m_protocol.GetBoundEndpointSummary();
        std::wstring bindSuffix = bound.empty() ? L"" : (L" [" + bound + L"]");
        auto devs = m_protocol.GetConnectedDevices();
        if (devs.empty()) {
            return L"En attente de connexion du telephone sur " + (bound.empty() ? L"0.0.0.0:1716" : bound) + L"...";
        }
        const auto& d = devs.front();
        std::wstring name = d.deviceName.empty() ? d.ipAddress : d.deviceName;
        return L"Connecte a " + name + (d.isPaired ? L" (Appaire)" : L" (En attente d'appairage)") + bindSuffix;
    }

    void SmsDaemon::HandleIncomingKdePacket(const DeviceInfo& device, const KDEPacket& packet) {
        if (packet.type == "kdeconnect.sms.messages") {
            std::string messagesArrayJson;
            ForEachJsonObjectField(packet.bodyJson, [&](const std::string& key, const std::string& val, bool) {
                if (key == "messages") {
                    messagesArrayJson = val;
                }
            });

            if (messagesArrayJson.empty()) {
                std::wcout << L"[SMS Sync] Paquet kdeconnect.sms.messages recu mais liste 'messages' vide." << std::endl;
                return;
            }

            std::vector<Message> parsedMessages;
            std::set<int64_t> threadsInPacket;

            ForEachJsonArrayObject(messagesArrayJson, [&](const std::string& msgObj) {
                Message msg;
                int smsType = 1;
                bool hasAttachments = false;
                std::string rawAttachmentsJson;

                ForEachJsonObjectField(msgObj, [&](const std::string& key, const std::string& val, bool) {
                    if (key == "_id") {
                        msg.id = std::strtoll(val.c_str(), nullptr, 10);
                    } else if (key == "thread_id") {
                        msg.threadId = std::strtoll(val.c_str(), nullptr, 10);
                    } else if (key == "body") {
                        msg.body = Utf8ToWide(UnescapeJsonString(val));
                    } else if (key == "date") {
                        msg.timestamp = std::strtoll(val.c_str(), nullptr, 10);
                    } else if (key == "type") {
                        smsType = std::atoi(val.c_str());
                    } else if (key == "read") {
                        msg.isRead = (val == "1" || val == "true");
                    } else if (key == "address" && msg.address.empty()) {
                        msg.address = Utf8ToWide(UnescapeJsonString(val));
                    } else if (key == "addresses") {
                        std::wstring joinedAddresses;
                        std::set<std::wstring> seenAddrKeys;
                        ForEachJsonArrayObject(val, [&](const std::string& addrObj) {
                            ForEachJsonObjectField(addrObj, [&](const std::string& aKey, const std::string& aVal, bool) {
                                if (aKey == "address" && !aVal.empty()) {
                                    std::wstring wAddr = TrimWideCopy(Utf8ToWide(UnescapeJsonString(aVal)));
                                    if (wAddr.empty()) return;
                                    std::wstring cKey = CanonicalSinglePhoneKey(wAddr);
                                    std::wstring dedupKey = cKey.empty() ? ToLowerWideCopy(wAddr) : cKey;
                                    if (!seenAddrKeys.insert(dedupKey).second) return;
                                    if (!joinedAddresses.empty()) joinedAddresses += L", ";
                                    joinedAddresses += wAddr;
                                }
                            });
                        });
                        if (!joinedAddresses.empty()) {
                            msg.address = joinedAddresses;
                        }
                    } else if (key == "attachments") {
                        if (val.size() > 2 && val.find('{') != std::string::npos) {
                            hasAttachments = true;
                            rawAttachmentsJson = val;
                        }
                    }
                });

                msg.isOutgoing = (smsType == 2 || smsType == 4 || smsType == 5 || smsType == 6);
                if (msg.threadId <= 0 && !msg.address.empty()) {
                    msg.threadId = MakeFallbackThreadId(msg.address);
                }
                if (msg.id == 0) {
                    msg.id = msg.timestamp ^ (msg.threadId << 16);
                }

                if (hasAttachments && !rawAttachmentsJson.empty()) {
                    std::wstring recvDir = GetReceivedFilesDirectory();
                    int attIndex = 0;
                    ForEachJsonArrayObject(rawAttachmentsJson, [&](const std::string& attObj) {
                        std::wstring mimeType = L"image/jpeg";
                        std::string encodedThumb;
                        ForEachJsonObjectField(attObj, [&](const std::string& aKey, const std::string& aVal, bool) {
                            if (aKey == "mime_type" || aKey == "mimeType") {
                                mimeType = Utf8ToWide(UnescapeJsonString(aVal));
                            } else if (aKey == "encoded_thumbnail" && !aVal.empty()) {
                                encodedThumb = UnescapeJsonString(aVal);
                            }
                        });

                        if (!encodedThumb.empty()) {
                            std::vector<uint8_t> bytes = Base64DecodeBytes(encodedThumb);
                            if (!bytes.empty()) {
                                std::wstring ext = ExtensionFromMimeWide(mimeType);
                                std::wstring fileName = L"mms_" + std::to_wstring(msg.id) + L"_" + std::to_wstring(attIndex) + ext;
                                std::wstring destPath = recvDir + L"\\" + fileName;
                                std::filesystem::path fsDest(destPath);
                                if (!std::filesystem::exists(fsDest)) {
                                    std::ofstream outFile(fsDest, std::ios::out | std::ios::binary | std::ios::trunc);
                                    if (outFile.is_open()) {
                                        outFile.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                                    }
                                }
                                Attachment att;
                                att.fileName = fileName;
                                att.mimeType = mimeType;
                                att.localFilePath = destPath;
                                att.fileSize = bytes.size();
                                msg.attachments.push_back(std::move(att));
                            }
                        }
                        ++attIndex;
                    });
                }

                if (msg.body.empty() && hasAttachments && msg.attachments.empty()) {
                    msg.body = L"[Piece jointe MMS]";
                }

                if (msg.timestamp > 0 && msg.timestamp < 100000000000LL) {
                    msg.timestamp *= 1000;
                }

                if (msg.threadId > 0 && (!msg.body.empty() || !msg.address.empty() || !msg.attachments.empty())) {
                    threadsInPacket.insert(msg.threadId);
                    parsedMessages.push_back(std::move(msg));
                }
            });

            std::wcout << L"[SMS Sync] " << parsedMessages.size() << L" message(s) recu(s) dans "
                       << threadsInPacket.size() << L" conversation(s)." << std::endl;

            if (!parsedMessages.empty()) {
                std::vector<Message> newlyInserted;
                SaveMessagesBatchToDisk(parsedMessages, &newlyInserted);
                NotifyConversationsUpdated();

                if (!newlyInserted.empty()) {
                    int64_t nowEpochMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    std::vector<std::pair<Message, std::wstring>> toNotify;
                    {
                        std::lock_guard<std::mutex> lock(m_storageMutex);
                        for (const auto& nm : newlyInserted) {
                            if (nm.isOutgoing) continue;
                            bool isRecent = (nm.timestamp >= m_startupEpochMs - 60000) &&
                                            (std::llabs(nowEpochMs - nm.timestamp) <= 600000);
                            bool isFreshUnread = (!nm.isRead && std::llabs(nowEpochMs - nm.timestamp) <= 600000);
                            if (!isRecent && !isFreshUnread) continue;

                            std::wstring displayName;
                            auto it = m_conversations.find(nm.threadId);
                            if (it != m_conversations.end() && !it->second.contactName.empty()) {
                                displayName = it->second.contactName;
                            } else {
                                displayName = ResolveContactNameForAddressLocked(nm.address);
                            }
                            toNotify.emplace_back(nm, displayName);
                        }
                    }
                    size_t notifiedCount = 0;
                    for (const auto& item : toNotify) {
                        if (notifiedCount >= 3) break;
                        MaybeNotifyIncomingMessage(item.first, item.second);
                        ++notifiedCount;
                    }
                }

                // When request_conversations returns multiple threads (1 latest message each),
                // automatically fetch full message history for the most recent threads!
                std::vector<int64_t> threadsToFetch;
                {
                    std::lock_guard<std::mutex> lock(m_storageMutex);
                    std::vector<std::pair<int64_t, int64_t>> candidates; // (lastTimestamp, threadId)
                    for (int64_t tid : threadsInPacket) {
                        if (m_requestedThreads.find(tid) == m_requestedThreads.end()) {
                            auto it = m_conversations.find(tid);
                            if (it != m_conversations.end() && it->second.messages.size() <= 1) {
                                candidates.emplace_back(it->second.lastTimestamp, tid);
                            }
                        }
                    }
                    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
                        return a.first > b.first;
                    });
                    size_t limit = std::min<size_t>(candidates.size(), 15);
                    for (size_t i = 0; i < limit; ++i) {
                        m_requestedThreads.insert(candidates[i].second);
                        threadsToFetch.push_back(candidates[i].second);
                    }
                }

                if (!threadsToFetch.empty()) {
                    std::thread([this, device, threadsToFetch = std::move(threadsToFetch)]() {
                        for (int64_t tid : threadsToFetch) {
                            if (!m_running) break;
                            m_protocol.RequestConversationMessages(device, tid, -1);
                            std::this_thread::sleep_for(std::chrono::milliseconds(50));
                        }
                    }).detach();
                }
            }
        } else if (packet.type == "kdeconnect.contacts.response_uids_timestamps") {
            std::vector<std::string> allUids;
            std::set<std::string> allUidSet;
            std::map<std::string, int64_t> remoteTs;
            ForEachJsonObjectField(packet.bodyJson, [&](const std::string& key, const std::string& val, bool) {
                if (key == "uids") {
                    ForEachJsonArrayString(val, [&](const std::string& uid) {
                        if (!uid.empty() && allUidSet.insert(uid).second) {
                            allUids.push_back(uid);
                        }
                    });
                } else if (!key.empty()) {
                    int64_t ts = std::strtoll(val.c_str(), nullptr, 10);
                    remoteTs[key] = ts;
                    if (allUidSet.insert(key).second) {
                        allUids.push_back(key);
                    }
                }
            });

            std::vector<std::string> uidsToFetch;
            {
                std::lock_guard<std::mutex> lock(m_storageMutex);
                for (const auto& uid : allUids) {
                    int64_t rTs = 0;
                    auto rtIt = remoteTs.find(uid);
                    if (rtIt != remoteTs.end()) rTs = rtIt->second;

                    auto cacheIt = m_contactUidTimestamps.find(uid);
                    bool missingContact = (m_contactsCache.find(uid) == m_contactsCache.end());
                    if (missingContact || cacheIt == m_contactUidTimestamps.end() || (rTs > 0 && rTs > cacheIt->second)) {
                        uidsToFetch.push_back(uid);
                        if (rTs > 0) m_contactUidTimestamps[uid] = rTs;
                    }
                }
            }

            std::wcout << L"[Contacts Sync] " << allUids.size() << L" UID(s) sur telephone, "
                       << uidsToFetch.size() << L" fiche(s) a recuperer." << std::endl;

            if (!uidsToFetch.empty()) {
                std::thread([this, device, uidsToFetch = std::move(uidsToFetch)]() {
                    const size_t kBatch = 50;
                    for (size_t i = 0; i < uidsToFetch.size() && m_running; i += kBatch) {
                        size_t end = std::min(i + kBatch, uidsToFetch.size());
                        std::vector<std::string> batch(uidsToFetch.begin() + i, uidsToFetch.begin() + end);
                        m_protocol.RequestContactsVcards(device, batch);
                        std::this_thread::sleep_for(std::chrono::milliseconds(40));
                    }
                }).detach();
            }
        } else if (packet.type == "kdeconnect.contacts.response_vcards") {
            size_t addedCount = 0;
            size_t avatarCount = 0;
            bool anyConversationUpdated = false;
            {
                std::lock_guard<std::mutex> lock(m_storageMutex);
                ForEachJsonObjectField(packet.bodyJson, [&](const std::string& key, const std::string& val, bool isStr) {
                    if (!isStr || key == "uids" || key.empty()) return;
                    std::string rawVCard = UnescapeJsonString(val);

                    CachedContact contact;
                    contact.uid = key;
                    auto tsIt = m_contactUidTimestamps.find(key);
                    contact.timestamp = (tsIt != m_contactUidTimestamps.end()) ? tsIt->second : 0;

                    std::vector<uint8_t> photoBytes;
                    std::wstring photoExt;
                    ParseVCardFields(rawVCard, contact.name, contact.phones, &photoBytes, &photoExt);
                    if (!contact.name.empty() && !contact.phones.empty()) {
                        if (!photoBytes.empty()) {
                            contact.avatarPath = SaveContactAvatarToDisk(contact.uid, photoBytes, photoExt);
                            if (!contact.avatarPath.empty()) avatarCount++;
                        }
                        m_contactsCache[contact.uid] = contact;
                        IndexContactLocked(contact);
                        addedCount++;
                    }
                });

                if (addedCount > 0) {
                    SaveContactsCacheLocked();
                    anyConversationUpdated = ApplyContactsToConversationsLocked();
                }
            }

            std::wcout << L"[Contacts Sync] " << addedCount << L" vCard(s) contact(s) importee(s), "
                       << avatarCount << L" photo(s) avatar." << std::endl;
            if (addedCount > 0 || anyConversationUpdated) {
                NotifyConversationsUpdated();
            }
        } else if (packet.type == "kdeconnect.telephony") {
            std::string eventType;
            std::string phone;
            std::string body;
            std::string contactName;
            bool isCancel = false;

            ForEachJsonObjectField(packet.bodyJson, [&](const std::string& key, const std::string& val, bool) {
                if (key == "event") eventType = UnescapeJsonString(val);
                else if (key == "phoneNumber") phone = UnescapeJsonString(val);
                else if (key == "messageBody") body = UnescapeJsonString(val);
                else if (key == "contactName") contactName = UnescapeJsonString(val);
                else if (key == "isCancel") isCancel = (val == "true" || val == "1");
            });

            if (isCancel || (!eventType.empty() && eventType != "sms")) {
                return;
            }

            std::wstring wPhone = TrimWideCopy(Utf8ToWide(phone));
            std::wstring wName = TrimWideCopy(Utf8ToWide(contactName));
            if (wPhone.empty()) {
                wPhone = wName;
            }

            if (!wPhone.empty() && !body.empty()) {
                int64_t matchedThreadId = -1;
                {
                    std::lock_guard<std::mutex> lock(m_storageMutex);
                    if (wName.empty()) {
                        wName = ResolveContactNameForAddressLocked(wPhone);
                    }
                    matchedThreadId = FindConversationByPhoneLocked(wPhone);

                    // Fallback: if telephony provided an alternate contact number or sender name instead of digits,
                    // match an existing open conversation by contact name (if unambiguous) or other phones of the same contact.
                    if (matchedThreadId <= 0) {
                        std::wstring nameLookup = !wName.empty() ? ToLowerWideCopy(wName) : ToLowerWideCopy(wPhone);
                        if (!nameLookup.empty()) {
                            int64_t uniqueByName = -1;
                            int nameMatches = 0;
                            for (const auto& kv : m_conversations) {
                                std::wstring cName = ToLowerWideCopy(TrimWideCopy(kv.second.contactName));
                                if (!cName.empty() && cName == nameLookup) {
                                    uniqueByName = kv.first;
                                    nameMatches++;
                                }
                            }
                            if (nameMatches == 1) {
                                matchedThreadId = uniqueByName;
                            }
                        }
                    }

                    if (matchedThreadId > 0) {
                        auto it = m_conversations.find(matchedThreadId);
                        if (it != m_conversations.end() && !wName.empty()) {
                            it->second.contactName = wName;
                        }
                    }
                }
                if (matchedThreadId <= 0) {
                    matchedThreadId = MakeFallbackThreadId(wPhone);
                }

                Message msg;
                msg.id = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                msg.threadId = matchedThreadId;
                msg.address = wPhone;
                msg.body = Utf8ToWide(body);
                msg.timestamp = msg.id;
                msg.isOutgoing = false;
                msg.isRead = false;

                SaveMessageToDisk(msg);
                NotifyConversationsUpdated();

                // Fire non-intrusive bubble notification (deduplicated with sms.messages)
                MaybeNotifyIncomingMessage(msg, wName.empty() ? msg.address : wName);

                // Refresh synced conversations from phone to bind real Android _id and thread_id
                m_protocol.RequestAllConversations(device);
                if (matchedThreadId > 0 && matchedThreadId < 1000000000LL) {
                    m_protocol.RequestConversationMessages(device, matchedThreadId, -1);
                }
            }
        }
    }

    void SmsDaemon::IndexContactLocked(const CachedContact& contact) {
        if (contact.name.empty()) return;
        for (const auto& phone : contact.phones) {
            std::wstring digits = NormalizePhoneDigits(phone);
            if (digits.empty()) continue;
            m_phoneToContactName[L"exact:" + digits] = contact.name;
            if (!contact.avatarPath.empty()) {
                m_phoneToAvatarPath[L"exact:" + digits] = contact.avatarPath;
            }
            if (digits.size() >= 8) {
                std::wstring suf = digits.size() > 9 ? digits.substr(digits.size() - 9) : digits;
                m_phoneToContactName[L"suf:" + suf] = contact.name;
                if (!contact.avatarPath.empty()) {
                    m_phoneToAvatarPath[L"suf:" + suf] = contact.avatarPath;
                }
            }
        }
    }

    std::wstring SmsDaemon::ResolveSingleContactLocked(const std::wstring& rawAddress) const {
        std::wstring digits = NormalizePhoneDigits(rawAddress);
        if (digits.empty()) return L"";
        auto itExact = m_phoneToContactName.find(L"exact:" + digits);
        if (itExact != m_phoneToContactName.end()) return itExact->second;
        if (digits.size() >= 8) {
            std::wstring suf = digits.size() > 9 ? digits.substr(digits.size() - 9) : digits;
            auto itSuf = m_phoneToContactName.find(L"suf:" + suf);
            if (itSuf != m_phoneToContactName.end()) return itSuf->second;
        }
        return L"";
    }

    std::wstring SmsDaemon::ResolveContactNameForAddressLocked(const std::wstring& rawAddress) const {
        if (rawAddress.empty()) return L"";
        if (rawAddress.find_first_of(L",;") == std::wstring::npos) {
            return ResolveSingleContactLocked(rawAddress);
        }
        std::wstring combined;
        bool anyResolved = false;
        size_t start = 0;
        while (start < rawAddress.size()) {
            size_t sep = rawAddress.find_first_of(L",;", start);
            std::wstring part = (sep == std::wstring::npos) ? rawAddress.substr(start) : rawAddress.substr(start, sep - start);
            while (!part.empty() && part.front() == L' ') part.erase(part.begin());
            while (!part.empty() && part.back() == L' ') part.pop_back();
            std::wstring resolved = ResolveSingleContactLocked(part);
            if (!resolved.empty()) anyResolved = true;
            if (!combined.empty()) combined += L", ";
            combined += resolved.empty() ? part : resolved;
            if (sep == std::wstring::npos) break;
            start = sep + 1;
        }
        return anyResolved ? combined : L"";
    }

    std::wstring SmsDaemon::ResolveSingleAvatarLocked(const std::wstring& rawAddress) const {
        std::wstring digits = NormalizePhoneDigits(rawAddress);
        if (digits.empty()) return L"";
        auto itExact = m_phoneToAvatarPath.find(L"exact:" + digits);
        if (itExact != m_phoneToAvatarPath.end()) return itExact->second;
        if (digits.size() >= 8) {
            std::wstring suf = digits.size() > 9 ? digits.substr(digits.size() - 9) : digits;
            auto itSuf = m_phoneToAvatarPath.find(L"suf:" + suf);
            if (itSuf != m_phoneToAvatarPath.end()) return itSuf->second;
        }
        return L"";
    }

    std::wstring SmsDaemon::ResolveAvatarForAddressLocked(const std::wstring& rawAddress) const {
        if (rawAddress.empty()) return L"";
        if (rawAddress.find_first_of(L",;") == std::wstring::npos) {
            return ResolveSingleAvatarLocked(rawAddress);
        }
        size_t start = 0;
        while (start < rawAddress.size()) {
            size_t sep = rawAddress.find_first_of(L",;", start);
            std::wstring part = (sep == std::wstring::npos) ? rawAddress.substr(start) : rawAddress.substr(start, sep - start);
            while (!part.empty() && part.front() == L' ') part.erase(part.begin());
            while (!part.empty() && part.back() == L' ') part.pop_back();
            std::wstring resolved = ResolveSingleAvatarLocked(part);
            if (!resolved.empty()) return resolved;
            if (sep == std::wstring::npos) break;
            start = sep + 1;
        }
        return L"";
    }

    int64_t SmsDaemon::FindConversationByPhoneLocked(const std::wstring& rawAddress) const {
        if (rawAddress.empty()) return -1;
        std::wstring targetKey = CanonicalSinglePhoneKey(rawAddress);
        std::wstring targetLower = ToLowerWideCopy(TrimWideCopy(rawAddress));

        // Prefer returning real Android threads (< 1000000000LL) over synthetic fallback threads
        int64_t fallbackMatch = -1;
        for (const auto& kv : m_conversations) {
            bool matched = false;
            if (!targetKey.empty()) {
                if (CanonicalSinglePhoneKey(kv.second.address) == targetKey) {
                    matched = true;
                } else {
                    for (const auto& m : kv.second.messages) {
                        if (CanonicalSinglePhoneKey(m.address) == targetKey) {
                            matched = true;
                            break;
                        }
                    }
                }
            } else if (!targetLower.empty() && ToLowerWideCopy(TrimWideCopy(kv.second.address)) == targetLower) {
                matched = true;
            }

            if (matched) {
                if (kv.first < 1000000000LL) {
                    return kv.first;
                }
                if (fallbackMatch < 0) {
                    fallbackMatch = kv.first;
                }
            }
        }
        return fallbackMatch;
    }

    int64_t SmsDaemon::ResolveMergedThreadId(int64_t threadId) const {
        std::lock_guard<std::mutex> lock(m_storageMutex);
        int64_t cur = threadId;
        for (int hops = 0; hops < 8; ++hops) {
            auto it = m_threadRedirects.find(cur);
            if (it == m_threadRedirects.end() || it->second == cur) break;
            cur = it->second;
        }
        return cur;
    }

    void SmsDaemon::LoadContactsCacheLocked() {
        m_contactsCache.clear();
        m_contactUidTimestamps.clear();
        m_phoneToContactName.clear();
        m_phoneToAvatarPath.clear();

        std::wstring path = GetAppDataDirectory() + L"\\contacts_cache.txt";
        std::filesystem::path fsPath(path);
        std::ifstream file(fsPath);
        if (!file.is_open()) return;

        std::string line;
        while (std::getline(file, line)) {
            if (line.empty()) continue;
            // Format: uid|timestamp|nameHex|phonesCommaSeparatedHex|avatarPathHex
            size_t p1 = line.find('|');
            size_t p2 = (p1 != std::string::npos) ? line.find('|', p1 + 1) : std::string::npos;
            size_t p3 = (p2 != std::string::npos) ? line.find('|', p2 + 1) : std::string::npos;
            if (p1 == std::string::npos || p2 == std::string::npos || p3 == std::string::npos) continue;

            size_t p4 = line.find('|', p3 + 1);
            bool hasAvatarCol = (p4 != std::string::npos);

            CachedContact c;
            c.uid = line.substr(0, p1);
            c.timestamp = std::strtoll(line.substr(p1 + 1, p2 - p1 - 1).c_str(), nullptr, 10);
            c.name = Utf8ToWide(HexDecode(line.substr(p2 + 1, p3 - p2 - 1)));
            std::string phonesField = hasAvatarCol ? line.substr(p3 + 1, p4 - p3 - 1) : line.substr(p3 + 1);
            size_t pos = 0;
            while (pos < phonesField.size()) {
                size_t comma = phonesField.find(',', pos);
                std::string pHex = (comma == std::string::npos) ? phonesField.substr(pos) : phonesField.substr(pos, comma - pos);
                if (!pHex.empty()) c.phones.push_back(Utf8ToWide(HexDecode(pHex)));
                if (comma == std::string::npos) break;
                pos = comma + 1;
            }
            if (hasAvatarCol) {
                std::wstring av = Utf8ToWide(HexDecode(line.substr(p4 + 1)));
                if (!av.empty() && GetFileAttributesW(av.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    c.avatarPath = std::move(av);
                }
            }

            if (!c.uid.empty() && !c.name.empty()) {
                // Force one-time vCard re-fetch for legacy cache rows lacking the avatar column
                m_contactUidTimestamps[c.uid] = hasAvatarCol ? c.timestamp : 0;
                m_contactsCache[c.uid] = c;
                IndexContactLocked(c);
            }
        }
    }

    void SmsDaemon::SaveContactsCacheLocked() {
        std::wstring path = GetAppDataDirectory() + L"\\contacts_cache.txt";
        std::filesystem::path fsPath(path);
        std::ofstream file(fsPath, std::ios::out | std::ios::trunc);
        if (!file.is_open()) return;
        for (const auto& kv : m_contactsCache) {
            const auto& c = kv.second;
            file << c.uid << "|" << c.timestamp << "|" << HexEncode(WideToUtf8(c.name)) << "|";
            for (size_t i = 0; i < c.phones.size(); ++i) {
                if (i > 0) file << ",";
                file << HexEncode(WideToUtf8(c.phones[i]));
            }
            file << "|" << HexEncode(WideToUtf8(c.avatarPath)) << "\n";
        }
    }

    bool SmsDaemon::ApplyContactsToConversationsLocked() {
        bool changed = false;
        int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();

        for (auto& kv : m_conversations) {
            Conversation& conv = kv.second;
            std::wstring resolved = ResolveContactNameForAddressLocked(conv.address);
            std::wstring resolvedAvatar = ResolveAvatarForAddressLocked(conv.address);
            if (resolved.empty() || resolvedAvatar.empty()) {
                for (auto it = conv.messages.rbegin(); it != conv.messages.rend(); ++it) {
                    if (!it->address.empty()) {
                        if (resolved.empty()) {
                            resolved = ResolveContactNameForAddressLocked(it->address);
                            if (!resolved.empty() && conv.address.empty()) {
                                conv.address = it->address;
                            }
                        }
                        if (resolvedAvatar.empty()) {
                            resolvedAvatar = ResolveAvatarForAddressLocked(it->address);
                        }
                        if (!resolved.empty() && !resolvedAvatar.empty()) break;
                    }
                }
            }

            bool dirty = false;
            if (!resolved.empty() && conv.contactName != resolved) {
                conv.contactName = resolved;
                dirty = true;
            }
            if (!resolvedAvatar.empty() && conv.avatarPath != resolvedAvatar) {
                conv.avatarPath = resolvedAvatar;
                dirty = true;
            }
            if (dirty) {
                m_ignoreWatcherUntilMs = nowMs + 350;
                WriteConversationJsonLocked(conv);
                changed = true;
            }
        }
        return changed;
    }

    void SmsDaemon::LoadDeletedThreadsLocked() {
        m_deletedThreads.clear();
        std::wstring path = GetAppDataDirectory() + L"\\deleted_threads.txt";
        std::filesystem::path fsPath(path);
        std::ifstream file(fsPath);
        if (!file.is_open()) return;
        std::string line;
        while (std::getline(file, line)) {
            if (line.empty()) continue;
            size_t sp = line.find(' ');
            if (sp != std::string::npos) {
                int64_t tid = std::strtoll(line.substr(0, sp).c_str(), nullptr, 10);
                int64_t ts = std::strtoll(line.substr(sp + 1).c_str(), nullptr, 10);
                if (tid > 0) m_deletedThreads[tid] = ts;
            }
        }
    }

    void SmsDaemon::SaveDeletedThreadsLocked() {
        std::wstring path = GetAppDataDirectory() + L"\\deleted_threads.txt";
        std::filesystem::path fsPath(path);
        std::ofstream file(fsPath, std::ios::out | std::ios::trunc);
        if (file.is_open()) {
            for (const auto& kv : m_deletedThreads) {
                file << kv.first << " " << kv.second << "\n";
            }
        }
    }

    bool SmsDaemon::DeleteConversation(int64_t threadId) {
        if (threadId <= 0) return false;

        {
            std::lock_guard<std::mutex> lock(m_storageMutex);
            int64_t deletedWatermark = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            auto it = m_conversations.find(threadId);
            if (it != m_conversations.end()) {
                deletedWatermark = std::max(deletedWatermark, it->second.lastTimestamp);
                m_conversations.erase(it);
            }
            m_deletedThreads[threadId] = deletedWatermark;
            SaveDeletedThreadsLocked();

            int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            m_ignoreWatcherUntilMs = nowMs + 350;

            std::wstring filePath = m_watchDir + L"\\thread_" + std::to_wstring(threadId) + L".json";
            DeleteFileW(filePath.c_str());
        }

        NotifyConversationsUpdated();

        if (m_isClientMode) {
            SendIpcCommand("DELETE_THREAD:" + std::to_string(threadId) + "\n");
        } else {
            m_protocol.RequestDeleteConversation(DeviceInfo{}, threadId);
        }
        return true;
    }

    void SmsDaemon::SaveMessageToDisk(const Message& msg) {
        SaveMessagesBatchToDisk(std::vector<Message>{ msg });
    }

    void SmsDaemon::WriteConversationJsonLocked(const Conversation& conv) {
        std::wstring filePath = m_watchDir + L"\\thread_" + std::to_wstring(conv.threadId) + L".json";
        std::wstring tmpPath = filePath + L".tmp";

        {
            std::filesystem::path fsTmpPath(tmpPath);
            std::ofstream file(fsTmpPath, std::ios::out | std::ios::trunc);
            if (file.is_open()) {
                file << "{\n"
                     << "  \"threadId\": " << conv.threadId << ",\n"
                     << "  \"address\": \"" << EscapeJson(WideToUtf8(conv.address)) << "\",\n"
                     << "  \"contactName\": \"" << EscapeJson(WideToUtf8(conv.contactName)) << "\",\n"
                     << "  \"avatarPath\": \"" << EscapeJson(WideToUtf8(conv.avatarPath)) << "\",\n"
                     << "  \"messages\": [\n";

                for (size_t i = 0; i < conv.messages.size(); ++i) {
                    const auto& m = conv.messages[i];
                    file << "    {\n"
                         << "      \"id\": " << m.id << ",\n"
                         << "      \"address\": \"" << EscapeJson(WideToUtf8(m.address)) << "\",\n"
                         << "      \"body\": \"" << EscapeJson(WideToUtf8(m.body)) << "\",\n"
                         << "      \"timestamp\": " << m.timestamp << ",\n"
                         << "      \"isOutgoing\": " << (m.isOutgoing ? "true" : "false") << ",\n"
                         << "      \"isRead\": " << (m.isRead ? "true" : "false");

                    if (!m.attachments.empty()) {
                        file << ",\n      \"attachments\": [\n";
                        for (size_t j = 0; j < m.attachments.size(); ++j) {
                            const auto& a = m.attachments[j];
                            file << "        {"
                                 << "\"fileName\": \"" << EscapeJson(WideToUtf8(a.fileName)) << "\", "
                                 << "\"mimeType\": \"" << EscapeJson(WideToUtf8(a.mimeType)) << "\", "
                                 << "\"localFilePath\": \"" << EscapeJson(WideToUtf8(a.localFilePath)) << "\", "
                                 << "\"fileSize\": " << a.fileSize
                                 << "}" << (j + 1 < m.attachments.size() ? "," : "") << "\n";
                        }
                        file << "      ]\n";
                    } else {
                        file << "\n";
                    }

                    file << "    }" << (i + 1 < conv.messages.size() ? "," : "") << "\n";
                }
                file << "  ]\n}\n";
                file.close();
            }
        }

        MoveFileExW(tmpPath.c_str(), filePath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    }

    void SmsDaemon::SaveMessagesBatchToDisk(const std::vector<Message>& messages, std::vector<Message>* outNewlyInserted) {
        if (messages.empty()) return;

        std::lock_guard<std::mutex> lock(m_storageMutex);
        std::set<int64_t> dirtyThreads;
        bool deletedChanged = false;

        for (const auto& rawMsg : messages) {
            Message msg = rawMsg;

            // Follow any previously recorded thread redirects (e.g. synthetic fallback -> real Android thread)
            for (int hops = 0; hops < 8; ++hops) {
                auto redIt = m_threadRedirects.find(msg.threadId);
                if (redIt == m_threadRedirects.end() || redIt->second == msg.threadId) break;
                msg.threadId = redIt->second;
            }

            auto delIt = m_deletedThreads.find(msg.threadId);
            if (delIt != m_deletedThreads.end()) {
                if (msg.timestamp <= delIt->second) {
                    continue; // Ignore already-deleted history during resync
                }
                // New message arrived after deletion -> unhide thread cleanly
                m_deletedThreads.erase(delIt);
                deletedChanged = true;
            }

            // Prevent duplicate conversations for the same single phone number / sender
            if (!msg.address.empty()) {
                int64_t dupTid = FindConversationByPhoneLocked(msg.address);
                if (dupTid > 0 && dupTid != msg.threadId) {
                    bool incomingIsSynth = (msg.threadId >= 1000000000LL);
                    bool existingIsSynth = (dupTid >= 1000000000LL);

                    if (incomingIsSynth && !existingIsSynth) {
                        // Redirect synthetic thread into existing real Android thread
                        if (m_conversations.find(msg.threadId) != m_conversations.end()) {
                            Conversation oldConv = std::move(m_conversations[msg.threadId]);
                            m_conversations.erase(msg.threadId);
                            dirtyThreads.erase(msg.threadId);
                            m_threadRedirects[msg.threadId] = dupTid;
                            std::wstring oldFilePath = m_watchDir + L"\\thread_" + std::to_wstring(msg.threadId) + L".json";
                            DeleteFileW(oldFilePath.c_str());

                            auto& destConv = m_conversations[dupTid];
                            for (auto& oldM : oldConv.messages) {
                                oldM.threadId = dupTid;
                                bool dupMsg = false;
                                for (auto& km : destConv.messages) {
                                    if (AreLogicalDuplicateMessages(km, oldM)) {
                                        dupMsg = true;
                                        break;
                                    }
                                }
                                if (!dupMsg) destConv.messages.push_back(std::move(oldM));
                            }
                        } else {
                            m_threadRedirects[msg.threadId] = dupTid;
                        }
                        msg.threadId = dupTid;
                    } else if (m_conversations.find(msg.threadId) == m_conversations.end() && !(incomingIsSynth && existingIsSynth)) {
                        // Incoming is a real Android thread -> absorb existing duplicate thread into it
                        Conversation oldConv = std::move(m_conversations[dupTid]);
                        m_conversations.erase(dupTid);
                        dirtyThreads.erase(dupTid);
                        m_threadRedirects[dupTid] = msg.threadId;

                        std::wstring oldFilePath = m_watchDir + L"\\thread_" + std::to_wstring(dupTid) + L".json";
                        DeleteFileW(oldFilePath.c_str());

                        auto& destConv = m_conversations[msg.threadId];
                        destConv.threadId = msg.threadId;
                        if (destConv.address.empty()) destConv.address = oldConv.address;
                        if (destConv.contactName.empty()) destConv.contactName = oldConv.contactName;
                        if (destConv.avatarPath.empty()) destConv.avatarPath = oldConv.avatarPath;
                        for (auto& oldM : oldConv.messages) {
                            oldM.threadId = msg.threadId;
                            bool dupMsg = false;
                            for (auto& km : destConv.messages) {
                                if (AreLogicalDuplicateMessages(km, oldM)) {
                                    dupMsg = true;
                                    break;
                                }
                            }
                            if (!dupMsg) destConv.messages.push_back(std::move(oldM));
                        }
                    } else if (incomingIsSynth && existingIsSynth) {
                        // Reuse existing synthetic thread instead of creating a second one
                        msg.threadId = dupTid;
                    } else if (!incomingIsSynth && existingIsSynth) {
                        // Both threads exist in m_conversations: absorb the stray synthetic thread into real thread
                        Conversation oldConv = std::move(m_conversations[dupTid]);
                        m_conversations.erase(dupTid);
                        dirtyThreads.erase(dupTid);
                        m_threadRedirects[dupTid] = msg.threadId;

                        std::wstring oldFilePath = m_watchDir + L"\\thread_" + std::to_wstring(dupTid) + L".json";
                        DeleteFileW(oldFilePath.c_str());

                        auto& destConv = m_conversations[msg.threadId];
                        destConv.threadId = msg.threadId;
                        if (destConv.address.empty()) destConv.address = oldConv.address;
                        if (destConv.contactName.empty()) destConv.contactName = oldConv.contactName;
                        if (destConv.avatarPath.empty()) destConv.avatarPath = oldConv.avatarPath;
                        for (auto& oldM : oldConv.messages) {
                            oldM.threadId = msg.threadId;
                            bool dupMsg = false;
                            for (auto& km : destConv.messages) {
                                if (AreLogicalDuplicateMessages(km, oldM)) {
                                    dupMsg = true;
                                    break;
                                }
                            }
                            if (!dupMsg) destConv.messages.push_back(std::move(oldM));
                        }
                    }
                }
            }

            auto& conv = m_conversations[msg.threadId];
            conv.threadId = msg.threadId;
            if (!msg.address.empty() && conv.address.empty()) {
                conv.address = msg.address;
            } else if (!msg.address.empty() && msg.address.size() > conv.address.size() && conv.contactName == conv.address) {
                conv.address = msg.address;
            }

            std::wstring resolvedName = ResolveContactNameForAddressLocked(conv.address);
            if (!resolvedName.empty()) {
                conv.contactName = resolvedName;
            } else if (conv.contactName.empty()) {
                conv.contactName = conv.address.empty() ? (L"Fil #" + std::to_wstring(msg.threadId)) : conv.address;
            }
            std::wstring resolvedAvatar = ResolveAvatarForAddressLocked(conv.address);
            if (!resolvedAvatar.empty()) {
                conv.avatarPath = resolvedAvatar;
            }

            // Deduplicate by real Android _id or matching logical message, always preserving real Android IDs over synthetic PC epoch IDs
            bool updated = false;
            for (auto& existing : conv.messages) {
                if (existing.id == msg.id && msg.id != 0) {
                    existing.body = msg.body;
                    existing.timestamp = msg.timestamp;
                    existing.isOutgoing = msg.isOutgoing;
                    existing.isRead = msg.isRead;
                    if (!msg.address.empty()) existing.address = msg.address;
                    if (!msg.attachments.empty()) existing.attachments = msg.attachments;
                    updated = true;
                    break;
                }
                if (AreLogicalDuplicateMessages(existing, msg)) {
                    bool existingIsSynth = IsSyntheticMessageId(existing.id);
                    bool incomingIsSynth = IsSyntheticMessageId(msg.id);
                    if (existingIsSynth && !incomingIsSynth) {
                        // Upgrade synthetic local placeholder with authoritative Android ID & timestamp
                        existing.id = msg.id;
                        existing.timestamp = msg.timestamp;
                        existing.isRead = msg.isRead;
                        if (!msg.body.empty()) existing.body = msg.body;
                        if (!msg.address.empty()) existing.address = msg.address;
                    }
                    if (existing.attachments.empty() && !msg.attachments.empty()) {
                        existing.attachments = msg.attachments;
                    }
                    updated = true;
                    break;
                }
            }
            if (!updated) {
                conv.messages.push_back(msg);
                if (outNewlyInserted) {
                    outNewlyInserted->push_back(msg);
                }
            }
            dirtyThreads.insert(msg.threadId);
        }

        if (deletedChanged) {
            SaveDeletedThreadsLocked();
        }

        if (dirtyThreads.empty()) return;

        int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        m_ignoreWatcherUntilMs = nowMs + 350;

        for (int64_t tid : dirtyThreads) {
            auto& conv = m_conversations[tid];
            std::sort(conv.messages.begin(), conv.messages.end(), [](const Message& a, const Message& b) {
                if (a.timestamp != b.timestamp) return a.timestamp < b.timestamp;
                return a.id < b.id;
            });

            conv.unreadCount = 0;
            for (const auto& m : conv.messages) {
                if (!m.isOutgoing && !m.isRead) {
                    conv.unreadCount++;
                }
            }
            if (!conv.messages.empty()) {
                conv.lastTimestamp = conv.messages.back().timestamp;
                conv.snippet = conv.messages.back().body.empty() && !conv.messages.back().attachments.empty()
                    ? L"[Image]"
                    : conv.messages.back().body;
            }

            WriteConversationJsonLocked(conv);
        }
    }

    void SmsDaemon::LoadAllConversationsFromDiskLocked() {
        std::map<int64_t, Conversation> loadedFromDisk;

        try {
            std::filesystem::path dirPath(m_watchDir);
            if (!std::filesystem::exists(dirPath)) return;

            for (const auto& entry : std::filesystem::directory_iterator(dirPath)) {
                if (!entry.is_regular_file()) continue;
                std::wstring filename = entry.path().filename().wstring();
                if (filename.rfind(L"thread_", 0) != 0 || !filename.ends_with(L".json")) continue;

                std::ifstream file(entry.path(), std::ios::in | std::ios::binary);
                if (!file.is_open()) continue;

                std::string jsonContent((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
                if (jsonContent.empty()) continue;

                Conversation conv;
                std::string messagesArr;

                ForEachJsonObjectField(jsonContent, [&](const std::string& key, const std::string& val, bool) {
                    if (key == "threadId") conv.threadId = std::strtoll(val.c_str(), nullptr, 10);
                    else if (key == "address") conv.address = Utf8ToWide(UnescapeJsonString(val));
                    else if (key == "contactName") conv.contactName = Utf8ToWide(UnescapeJsonString(val));
                    else if (key == "avatarPath") conv.avatarPath = Utf8ToWide(UnescapeJsonString(val));
                    else if (key == "messages") messagesArr = val;
                });

                if (conv.threadId <= 0) continue;
                if (m_deletedThreads.find(conv.threadId) != m_deletedThreads.end()) continue;

                std::wstring resolved = ResolveContactNameForAddressLocked(conv.address);
                if (!resolved.empty()) {
                    conv.contactName = resolved;
                } else if (conv.contactName.empty()) {
                    conv.contactName = conv.address.empty() ? (L"Fil #" + std::to_wstring(conv.threadId)) : conv.address;
                }
                std::wstring resolvedAvatar = ResolveAvatarForAddressLocked(conv.address);
                if (!resolvedAvatar.empty()) {
                    conv.avatarPath = resolvedAvatar;
                } else if (!conv.avatarPath.empty() && GetFileAttributesW(conv.avatarPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
                    conv.avatarPath.clear();
                }

                if (!messagesArr.empty()) {
                    ForEachJsonArrayObject(messagesArr, [&](const std::string& msgObj) {
                        Message m;
                        m.threadId = conv.threadId;
                        m.address = conv.address;
                        std::string attsArr;
                        ForEachJsonObjectField(msgObj, [&](const std::string& key, const std::string& val, bool) {
                            if (key == "id") m.id = std::strtoll(val.c_str(), nullptr, 10);
                            else if (key == "address") m.address = Utf8ToWide(UnescapeJsonString(val));
                            else if (key == "body") m.body = Utf8ToWide(UnescapeJsonString(val));
                            else if (key == "timestamp") m.timestamp = std::strtoll(val.c_str(), nullptr, 10);
                            else if (key == "isOutgoing") m.isOutgoing = (val == "true" || val == "1");
                            else if (key == "isRead") m.isRead = (val == "true" || val == "1");
                            else if (key == "attachments") attsArr = val;
                        });
                        if (!attsArr.empty()) {
                            ForEachJsonArrayObject(attsArr, [&](const std::string& attObj) {
                                Attachment att;
                                ForEachJsonObjectField(attObj, [&](const std::string& aKey, const std::string& aVal, bool) {
                                    if (aKey == "fileName") att.fileName = Utf8ToWide(UnescapeJsonString(aVal));
                                    else if (aKey == "mimeType") att.mimeType = Utf8ToWide(UnescapeJsonString(aVal));
                                    else if (aKey == "localFilePath") att.localFilePath = Utf8ToWide(UnescapeJsonString(aVal));
                                    else if (aKey == "fileSize") att.fileSize = std::strtoull(aVal.c_str(), nullptr, 10);
                                });
                                if (!att.localFilePath.empty() || !att.fileName.empty()) {
                                    m.attachments.push_back(std::move(att));
                                }
                            });
                        }
                        if (m.timestamp > 0 || !m.body.empty() || !m.attachments.empty()) {
                            conv.messages.push_back(std::move(m));
                        }
                    });
                }

                if (conv.address.empty() && !conv.messages.empty()) {
                    for (const auto& cm : conv.messages) {
                        if (!cm.address.empty()) {
                            conv.address = cm.address;
                            break;
                        }
                    }
                }

                std::sort(conv.messages.begin(), conv.messages.end(), [](const Message& a, const Message& b) {
                    if (a.timestamp != b.timestamp) return a.timestamp < b.timestamp;
                    return a.id < b.id;
                });

                // Strip any pre-existing duplicate messages within this thread (preferring real Android IDs)
                std::vector<Message> dedupedMsgs;
                dedupedMsgs.reserve(conv.messages.size());
                for (auto& m : conv.messages) {
                    bool merged = false;
                    for (auto& kept : dedupedMsgs) {
                        if (AreLogicalDuplicateMessages(kept, m)) {
                            if (IsSyntheticMessageId(kept.id) && !IsSyntheticMessageId(m.id)) {
                                kept.id = m.id;
                                kept.timestamp = m.timestamp;
                                kept.isRead = m.isRead;
                                if (!m.body.empty()) kept.body = m.body;
                                if (!m.address.empty()) kept.address = m.address;
                            }
                            if (kept.attachments.empty() && !m.attachments.empty()) {
                                kept.attachments = std::move(m.attachments);
                            }
                            merged = true;
                            break;
                        }
                    }
                    if (!merged) {
                        dedupedMsgs.push_back(std::move(m));
                    }
                }
                conv.messages = std::move(dedupedMsgs);

                if (!conv.messages.empty()) {
                    conv.lastTimestamp = conv.messages.back().timestamp;
                    conv.snippet = conv.messages.back().body.empty() && !conv.messages.back().attachments.empty()
                        ? L"[Image]"
                        : conv.messages.back().body;
                }
                for (const auto& m : conv.messages) {
                    if (!m.isOutgoing && !m.isRead) conv.unreadCount++;
                }

                loadedFromDisk[conv.threadId] = std::move(conv);
            }
        } catch (...) {
            // Keep existing in-memory cache if filesystem scan races
            return;
        }

        // Deduplicate any pre-existing duplicate thread files on disk for the same single phone number / sender
        std::map<std::wstring, int64_t> canonicalByPhone;
        std::vector<int64_t> dupIds;
        for (auto& kv : loadedFromDisk) {
            std::wstring pKey = CanonicalSinglePhoneKey(kv.second.address);
            if (pKey.empty()) {
                for (const auto& m : kv.second.messages) {
                    pKey = CanonicalSinglePhoneKey(m.address);
                    if (!pKey.empty()) break;
                }
            }
            if (pKey.empty()) continue;
            auto it = canonicalByPhone.find(pKey);
            if (it == canonicalByPhone.end()) {
                canonicalByPhone[pKey] = kv.first;
                continue;
            }

            int64_t keepId = it->second;
            int64_t dropId = kv.first;
            // Prefer keeping non-synthetic Android threadId (< 1000000000LL)
            if (keepId >= 1000000000LL && dropId < 1000000000LL) {
                std::swap(keepId, dropId);
                canonicalByPhone[pKey] = keepId;
            }

            Conversation& keepConv = loadedFromDisk[keepId];
            Conversation& dropConv = loadedFromDisk[dropId];
            if (keepConv.contactName.empty() || keepConv.contactName == keepConv.address) {
                if (!dropConv.contactName.empty()) keepConv.contactName = dropConv.contactName;
            }
            if (keepConv.avatarPath.empty()) keepConv.avatarPath = dropConv.avatarPath;

            for (auto& dm : dropConv.messages) {
                bool dupMsg = false;
                for (auto& km : keepConv.messages) {
                    if (AreLogicalDuplicateMessages(km, dm)) {
                        if (IsSyntheticMessageId(km.id) && !IsSyntheticMessageId(dm.id)) {
                            km.id = dm.id;
                            km.timestamp = dm.timestamp;
                            km.isRead = dm.isRead;
                            if (!dm.body.empty()) km.body = dm.body;
                        }
                        if (km.attachments.empty() && !dm.attachments.empty()) {
                            km.attachments = std::move(dm.attachments);
                        }
                        dupMsg = true;
                        break;
                    }
                }
                if (!dupMsg) {
                    dm.threadId = keepId;
                    keepConv.messages.push_back(std::move(dm));
                }
            }
            std::sort(keepConv.messages.begin(), keepConv.messages.end(), [](const Message& a, const Message& b) {
                if (a.timestamp != b.timestamp) return a.timestamp < b.timestamp;
                return a.id < b.id;
            });
            if (!keepConv.messages.empty()) {
                keepConv.lastTimestamp = keepConv.messages.back().timestamp;
                keepConv.snippet = keepConv.messages.back().body.empty() && !keepConv.messages.back().attachments.empty()
                    ? L"[Image]"
                    : keepConv.messages.back().body;
            }
            WriteConversationJsonLocked(keepConv);
            m_threadRedirects[dropId] = keepId;
            dupIds.push_back(dropId);
        }
        for (int64_t did : dupIds) {
            loadedFromDisk.erase(did);
            std::wstring oldFile = m_watchDir + L"\\thread_" + std::to_wstring(did) + L".json";
            DeleteFileW(oldFile.c_str());
        }

        m_conversations = std::move(loadedFromDisk);
    }

    std::vector<Conversation> SmsDaemon::LoadAllConversations() {
        std::lock_guard<std::mutex> lock(m_storageMutex);

        if (m_conversations.empty()) {
            LoadAllConversationsFromDiskLocked();
        } else if (m_isClientMode && m_contactsCache.empty()) {
            LoadContactsCacheLocked();
            if (!m_contactsCache.empty()) {
                ApplyContactsToConversationsLocked();
            }
        }

        std::vector<Conversation> result;
        result.reserve(m_conversations.size());
        for (const auto& pair : m_conversations) {
            result.push_back(pair.second);
        }

        // Sort descending by last timestamp
        std::sort(result.begin(), result.end(), [](const Conversation& a, const Conversation& b) {
            return a.lastTimestamp > b.lastTimestamp;
        });

        return result;
    }

    std::vector<ContactSummary> SmsDaemon::GetSyncedContacts() const {
        std::vector<ContactSummary> result;
        std::set<std::wstring> seen;
        {
            std::lock_guard<std::mutex> lock(m_storageMutex);
            for (const auto& kv : m_contactsCache) {
                const auto& c = kv.second;
                if (c.name.empty()) continue;
                for (const auto& phone : c.phones) {
                    if (phone.empty()) continue;
                    std::wstring digits = NormalizePhoneDigits(phone);
                    std::wstring key = c.name + L"|" + (digits.empty() ? phone : digits);
                    if (seen.insert(key).second) {
                        ContactSummary item;
                        item.name = c.name;
                        item.phone = phone;
                        item.avatarPath = c.avatarPath;
                        result.push_back(std::move(item));
                    }
                }
            }
        }

        auto toLower = [](const std::wstring& s) {
            std::wstring out = s;
            for (auto& ch : out) ch = static_cast<wchar_t>(::towlower(ch));
            return out;
        };
        std::sort(result.begin(), result.end(), [&](const ContactSummary& a, const ContactSummary& b) {
            std::wstring la = toLower(a.name);
            std::wstring lb = toLower(b.name);
            if (la != lb) return la < lb;
            return a.phone < b.phone;
        });
        return result;
    }

    Conversation SmsDaemon::LoadConversation(int64_t threadId) {
        std::lock_guard<std::mutex> lock(m_storageMutex);
        auto it = m_conversations.find(threadId);
        if (it != m_conversations.end()) {
            return it->second;
        }
        return {};
    }

    bool SmsDaemon::SendOutgoingSms(
        const std::wstring& phoneNumber,
        const std::wstring& text,
        int64_t preferredThreadId,
        const std::wstring& attachmentPath
    ) {
        if (phoneNumber.empty() || (text.empty() && attachmentPath.empty())) return false;

        int64_t threadId = preferredThreadId;
        {
            std::lock_guard<std::mutex> lock(m_storageMutex);
            if (threadId > 0) {
                int64_t cur = threadId;
                for (int hops = 0; hops < 8; ++hops) {
                    auto it = m_threadRedirects.find(cur);
                    if (it == m_threadRedirects.end() || it->second == cur) break;
                    cur = it->second;
                }
                threadId = cur;
            }
            if (threadId <= 0) {
                threadId = FindConversationByPhoneLocked(phoneNumber);
            }
        }
        if (threadId <= 0) {
            threadId = MakeFallbackThreadId(phoneNumber);
        }

        std::vector<Attachment> atts;
        if (!attachmentPath.empty()) {
            std::filesystem::path fsAtt(attachmentPath);
            std::error_code ec;
            if (!std::filesystem::exists(fsAtt, ec) || !std::filesystem::is_regular_file(fsAtt, ec)) {
                return false;
            }
            Attachment att;
            att.localFilePath = attachmentPath;
            att.fileName = fsAtt.filename().wstring();
            att.mimeType = GuessMimeTypeWide(attachmentPath);
            att.fileSize = std::filesystem::file_size(fsAtt, ec);
            atts.push_back(std::move(att));
        }

        bool sentOk = false;
        if (m_isClientMode) {
            std::string resp;
            if (!atts.empty()) {
                std::string cmd = "SEND_MMS|" + HexEncode(WideToUtf8(phoneNumber)) + "|" +
                                  HexEncode(WideToUtf8(text)) + "|" + HexEncode(WideToUtf8(attachmentPath)) + "\n";
                sentOk = SendIpcCommand(cmd, &resp) && (resp.find("\"ok\"") != std::string::npos);
            } else {
                std::string cmd = "SEND_SMS|" + HexEncode(WideToUtf8(phoneNumber)) + "|" + HexEncode(WideToUtf8(text)) + "\n";
                sentOk = SendIpcCommand(cmd, &resp) && (resp.find("\"ok\"") != std::string::npos);
            }
        } else {
            sentOk = m_protocol.SendSms(DeviceInfo{}, phoneNumber, text, atts);
        }

        if (!sentOk) {
            return false;
        }

        Message msg;
        msg.id = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        msg.threadId = threadId;
        msg.address = phoneNumber;
        msg.body = text;
        msg.timestamp = msg.id;
        msg.isOutgoing = true;
        msg.isRead = true;
        msg.attachments = std::move(atts);

        SaveMessageToDisk(msg);
        NotifyConversationsUpdated();
        return true;
    }

} // namespace KDEConnect
