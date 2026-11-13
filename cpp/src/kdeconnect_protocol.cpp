#include "kdeconnect_protocol.h"
#include <iostream>
#include <iomanip>
#include <cstring>
#include <cstdio>

namespace KDEConnect {

    // Simple JSON helpers for KDE Connect packet framing
    static std::string EscapeJsonString(const std::string& input) {
        std::ostringstream ss;
        for (char c : input) {
            switch (c) {
                case '"': ss << "\\\""; break;
                case '\\': ss << "\\\\"; break;
                case '\b': ss << "\\b"; break;
                case '\f': ss << "\\f"; break;
                case '\n': ss << "\\n"; break;
                case '\r': ss << "\\r"; break;
                case '\t': ss << "\\t"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        ss << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(static_cast<unsigned char>(c));
                    } else {
                        ss << c;
                    }
            }
        }
        return ss.str();
    }

    static bool SendAllBytes(SOCKET sock, const char* data, int len) {
        int totalSent = 0;
        while (totalSent < len) {
            int sent = send(sock, data + totalSent, len - totalSent, 0);
            if (sent <= 0) return false;
            totalSent += sent;
        }
        return true;
    }

    static bool ReadPlaintextLine(SOCKET sock, std::string& outLine) {
        outLine.clear();
        char ch = 0;
        for (int i = 0; i < 8192; ++i) {
            int r = recv(sock, &ch, 1, 0);
            if (r <= 0) return false;
            if (ch == '\n') return true;
            if (ch != '\r') outLine.push_back(ch);
        }
        return false;
    }

    std::string KDEPacket::Serialize() const {
        std::ostringstream oss;
        oss << "{\"id\":" << id
            << ",\"type\":\"" << EscapeJsonString(type) << "\""
            << ",\"body\":" << (bodyJson.empty() ? "{}" : bodyJson)
            << "}\n";
        return oss.str();
    }

    bool KDEPacket::Deserialize(const std::string& raw, KDEPacket& outPacket) {
        if (raw.empty()) return false;

        size_t idPos = raw.find("\"id\":");
        if (idPos != std::string::npos) {
            size_t idEnd = raw.find_first_of(",}\r\n", idPos + 5);
            if (idEnd != std::string::npos) {
                std::string idStr = raw.substr(idPos + 5, idEnd - (idPos + 5));
                outPacket.id = std::strtoll(idStr.c_str(), nullptr, 10);
            }
        }

        size_t typePos = raw.find("\"type\":");
        if (typePos != std::string::npos) {
            size_t quoteStart = raw.find('"', typePos + 7);
            if (quoteStart != std::string::npos) {
                size_t quoteEnd = raw.find('"', quoteStart + 1);
                if (quoteEnd != std::string::npos) {
                    outPacket.type = raw.substr(quoteStart + 1, quoteEnd - quoteStart - 1);
                }
            }
        }

        size_t bodyPos = raw.find("\"body\":");
        if (bodyPos != std::string::npos) {
            size_t startBrace = raw.find('{', bodyPos);
            if (startBrace != std::string::npos) {
                int braceCount = 0;
                size_t endBrace = startBrace;
                for (size_t i = startBrace; i < raw.size(); ++i) {
                    if (raw[i] == '{') braceCount++;
                    else if (raw[i] == '}') {
                        braceCount--;
                        if (braceCount == 0) {
                            endBrace = i;
                            break;
                        }
                    }
                }
                outPacket.bodyJson = raw.substr(startBrace, endBrace - startBrace + 1);
            }
        }

        return !outPacket.type.empty();
    }

    static DeviceInfo ParseIdentityBody(const KDEPacket& pkt, const std::wstring& fallbackIp, uint16_t fallbackPort) {
        DeviceInfo dev;
        dev.ipAddress = fallbackIp;
        dev.port = fallbackPort;

        size_t dId = pkt.bodyJson.find("\"deviceId\":");
        if (dId != std::string::npos) {
            size_t q1 = pkt.bodyJson.find('"', dId + 11);
            size_t q2 = pkt.bodyJson.find('"', q1 + 1);
            if (q1 != std::string::npos && q2 != std::string::npos) {
                dev.deviceId = Utf8ToWide(pkt.bodyJson.substr(q1 + 1, q2 - q1 - 1));
            }
        }

        size_t dName = pkt.bodyJson.find("\"deviceName\":");
        if (dName != std::string::npos) {
            size_t q1 = pkt.bodyJson.find('"', dName + 13);
            size_t q2 = pkt.bodyJson.find('"', q1 + 1);
            if (q1 != std::string::npos && q2 != std::string::npos) {
                dev.deviceName = Utf8ToWide(pkt.bodyJson.substr(q1 + 1, q2 - q1 - 1));
            }
        }

        size_t dType = pkt.bodyJson.find("\"deviceType\":");
        if (dType != std::string::npos) {
            size_t q1 = pkt.bodyJson.find('"', dType + 13);
            size_t q2 = pkt.bodyJson.find('"', q1 + 1);
            if (q1 != std::string::npos && q2 != std::string::npos) {
                dev.deviceType = Utf8ToWide(pkt.bodyJson.substr(q1 + 1, q2 - q1 - 1));
            }
        }

        size_t pPort = pkt.bodyJson.find("\"tcpPort\":");
        if (pPort != std::string::npos) {
            size_t portEnd = pkt.bodyJson.find_first_of(",}\r\n", pPort + 10);
            std::string portStr = pkt.bodyJson.substr(pPort + 10, portEnd - (pPort + 10));
            int parsedPort = std::atoi(portStr.c_str());
            if (parsedPort > 0 && parsedPort <= 65535) dev.port = static_cast<uint16_t>(parsedPort);
        }

        return dev;
    }

    ProtocolHandler::ProtocolHandler() {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);

        // Generate local identity
        wchar_t computerName[MAX_COMPUTERNAME_LENGTH + 1];
        DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
        if (!GetComputerNameW(computerName, &size)) {
            wcscpy_s(computerName, L"WindowsPC");
        }

        wchar_t userName[256];
        DWORD userSize = 256;
        if (!GetUserNameW(userName, &userSize)) {
            wcscpy_s(userName, L"User");
        }

        std::string seed = WideToUtf8(computerName) + "_" + WideToUtf8(userName);
        uint64_t h1 = 0xcbf29ce484222325ULL;
        uint64_t h2 = 0x100000001b3ULL;
        for (char c : seed) {
            h1 = (h1 ^ static_cast<uint8_t>(c)) * 0x100000001b3ULL;
            h2 = (h2 + static_cast<uint8_t>(c)) * 0xcbf29ce484222325ULL;
        }

        // Strict 32-character lowercase hexadecimal string matching ^[a-zA-Z0-9_]{32,38}$
        char hexBuf[33];
        std::snprintf(hexBuf, sizeof(hexBuf), "%016llx%016llx",
                      static_cast<unsigned long long>(h1),
                      static_cast<unsigned long long>(h2));

        m_localIdentity.deviceId = Utf8ToWide(hexBuf);
        m_localIdentity.deviceName = computerName;
        m_localIdentity.deviceType = L"desktop";
        m_localIdentity.isPaired = true;
        m_localIdentity.isReachable = true;
    }

    ProtocolHandler::~ProtocolHandler() {
        Stop();
        if (m_localCert) {
            CertFreeCertificateContext(m_localCert);
            m_localCert = nullptr;
        }
        WSACleanup();
    }

    bool ProtocolHandler::EnsureLocalCertificate() {
        if (m_localCert) return true;

        std::wstring containerName = L"KDEConnectWin32SmsKey_" + m_localIdentity.deviceId;
        std::wstring subjectName = L"CN=" + m_localIdentity.deviceId;

        HCERTSTORE hStore = CertOpenSystemStoreW(0, L"MY");
        if (hStore) {
            PCCERT_CONTEXT pFound = CertFindCertificateInStore(
                hStore,
                X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
                0,
                CERT_FIND_SUBJECT_STR_W,
                m_localIdentity.deviceId.c_str(),
                nullptr
            );
            if (pFound) {
                HCRYPTPROV_OR_NCRYPT_KEY_HANDLE hTestProv = 0;
                DWORD dwKeySpec = 0;
                BOOL fCallerFree = FALSE;
                if (CryptAcquireCertificatePrivateKey(pFound, CRYPT_ACQUIRE_SILENT_FLAG, nullptr, &hTestProv, &dwKeySpec, &fCallerFree)) {
                    if (fCallerFree && dwKeySpec != CERT_NCRYPT_KEY_SPEC) {
                        CryptReleaseContext(static_cast<HCRYPTPROV>(hTestProv), 0);
                    }
                    m_localCert = pFound;
                    CertCloseStore(hStore, 0);
                    std::wcout << L"[TLS Certificat] Certificat X.509 existant charge (CN=" << m_localIdentity.deviceId << L")" << std::endl;
                    return true;
                }
                CertFreeCertificateContext(pFound);
            }
        }

        // Create RSA-2048 key pair in Windows CSP
        HCRYPTPROV hProv = 0;
        if (!CryptAcquireContextW(&hProv, containerName.c_str(), MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES, CRYPT_SILENT)) {
            if (GetLastError() == static_cast<DWORD>(NTE_BAD_KEYSET)) {
                CryptAcquireContextW(&hProv, containerName.c_str(), MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES, CRYPT_NEWKEYSET | CRYPT_SILENT);
            } else {
                CryptAcquireContextW(&hProv, containerName.c_str(), MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES, CRYPT_DELETEKEYSET | CRYPT_SILENT);
                CryptAcquireContextW(&hProv, containerName.c_str(), MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES, CRYPT_NEWKEYSET | CRYPT_SILENT);
            }
        }

        if (!hProv) {
            std::wcerr << L"[ERREUR TLS] CryptAcquireContextW echoue (code " << GetLastError() << L")" << std::endl;
            if (hStore) CertCloseStore(hStore, 0);
            return false;
        }

        HCRYPTKEY hKey = 0;
        if (!CryptGetUserKey(hProv, AT_KEYEXCHANGE, &hKey)) {
            if (!CryptGenKey(hProv, AT_KEYEXCHANGE, (2048 << 16) | CRYPT_EXPORTABLE, &hKey)) {
                std::wcerr << L"[ERREUR TLS] CryptGenKey 2048-bit echoue (code " << GetLastError() << L")" << std::endl;
                CryptReleaseContext(hProv, 0);
                if (hStore) CertCloseStore(hStore, 0);
                return false;
            }
        }
        if (hKey) CryptDestroyKey(hKey);

        // Encode Subject CN=<deviceId>
        CERT_NAME_BLOB subjectBlob{};
        if (!CertStrToNameW(X509_ASN_ENCODING, subjectName.c_str(), CERT_X500_NAME_STR, nullptr, nullptr, &subjectBlob.cbData, nullptr)) {
            CryptReleaseContext(hProv, 0);
            if (hStore) CertCloseStore(hStore, 0);
            return false;
        }

        std::vector<BYTE> subjectBytes(subjectBlob.cbData);
        subjectBlob.pbData = subjectBytes.data();
        if (!CertStrToNameW(X509_ASN_ENCODING, subjectName.c_str(), CERT_X500_NAME_STR, nullptr, subjectBlob.pbData, &subjectBlob.cbData, nullptr)) {
            CryptReleaseContext(hProv, 0);
            if (hStore) CertCloseStore(hStore, 0);
            return false;
        }

        CRYPT_KEY_PROV_INFO kpi{};
        kpi.pwszContainerName = const_cast<LPWSTR>(containerName.c_str());
        kpi.pwszProvName = const_cast<LPWSTR>(MS_ENH_RSA_AES_PROV_W);
        kpi.dwProvType = PROV_RSA_AES;
        kpi.dwFlags = 0;
        kpi.dwKeySpec = AT_KEYEXCHANGE;

        CRYPT_ALGORITHM_IDENTIFIER sigAlg{};
        sigAlg.pszObjId = const_cast<LPSTR>(szOID_RSA_SHA256RSA);

        // Start 1 day in the past and expire in 10 years to avoid phone/PC clock skew
        FILETIME ftNow{};
        GetSystemTimeAsFileTime(&ftNow);
        ULARGE_INTEGER uli{};
        uli.LowPart = ftNow.dwLowDateTime;
        uli.HighPart = ftNow.dwHighDateTime;

        ULARGE_INTEGER uliStart = uli;
        uliStart.QuadPart -= 86400ULL * 10000000ULL;
        FILETIME ftStart{ uliStart.LowPart, uliStart.HighPart };
        SYSTEMTIME stStart{};
        FileTimeToSystemTime(&ftStart, &stStart);

        ULARGE_INTEGER uliEnd = uli;
        uliEnd.QuadPart += 3650ULL * 86400ULL * 10000000ULL;
        FILETIME ftEnd{ uliEnd.LowPart, uliEnd.HighPart };
        SYSTEMTIME stEnd{};
        FileTimeToSystemTime(&ftEnd, &stEnd);

        PCCERT_CONTEXT pCert = CertCreateSelfSignCertificate(
            hProv,
            &subjectBlob,
            0,
            &kpi,
            &sigAlg,
            &stStart,
            &stEnd,
            nullptr
        );

        CryptReleaseContext(hProv, 0);

        if (!pCert) {
            std::wcerr << L"[ERREUR TLS] CertCreateSelfSignCertificate echoue (code " << GetLastError() << L")" << std::endl;
            if (hStore) CertCloseStore(hStore, 0);
            return false;
        }

        if (hStore) {
            CertAddCertificateContextToStore(hStore, pCert, CERT_STORE_ADD_REPLACE_EXISTING, nullptr);
            CertCloseStore(hStore, 0);
        }

        m_localCert = pCert;
        std::wcout << L"[TLS Certificat] Nouveau certificat X.509 genere (CN=" << m_localIdentity.deviceId << L")" << std::endl;
        return true;
    }

    void ProtocolHandler::CleanupTlsSession(TlsSession& session) {
        if (session.hasCtx) {
            DeleteSecurityContext(&session.hCtx);
            session.hasCtx = false;
        }
        if (session.hasCred) {
            FreeCredentialsHandle(&session.hCred);
            session.hasCred = false;
        }
        if (session.sock != INVALID_SOCKET) {
            closesocket(session.sock);
            session.sock = INVALID_SOCKET;
        }
    }

    bool ProtocolHandler::PerformTlsHandshakeClient(SOCKET sock, TlsSession& session) {
        session.sock = sock;

        SCHANNEL_CRED schCred{};
        schCred.dwVersion = SCHANNEL_CRED_VERSION;
        if (m_localCert) {
            schCred.cCreds = 1;
            schCred.paCred = &m_localCert;
        }
        schCred.grbitEnabledProtocols = 0; // Let OS negotiate TLS 1.2 / 1.3
        schCred.dwFlags = SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS;

        TimeStamp tsExpiry{};
        SECURITY_STATUS status = AcquireCredentialsHandleW(
            nullptr,
            const_cast<LPWSTR>(UNISP_NAME_W),
            SECPKG_CRED_OUTBOUND,
            nullptr,
            &schCred,
            nullptr,
            nullptr,
            &session.hCred,
            &tsExpiry
        );

        if (status != SEC_E_OK) {
            std::wcerr << L"[ERREUR TLS Client] AcquireCredentialsHandleW = 0x" << std::hex << status << std::dec << std::endl;
            return false;
        }
        session.hasCred = true;

        DWORD iscFlags = ISC_REQ_SEQUENCE_DETECT |
                         ISC_REQ_REPLAY_DETECT |
                         ISC_REQ_CONFIDENTIALITY |
                         ISC_REQ_EXTENDED_ERROR |
                         ISC_REQ_ALLOCATE_MEMORY |
                         ISC_REQ_STREAM |
                         ISC_REQ_MANUAL_CRED_VALIDATION |
                         ISC_REQ_USE_SUPPLIED_CREDS;

        std::vector<char> inBuf;
        bool firstCall = true;
        bool needRecv = false;

        while (m_running) {
            if (needRecv) {
                char chunk[8192];
                int r = recv(sock, chunk, sizeof(chunk), 0);
                if (r <= 0) {
                    return false;
                }
                inBuf.insert(inBuf.end(), chunk, chunk + r);
                needRecv = false;
            }

            SecBuffer inBuffers[2]{};
            inBuffers[0].BufferType = SECBUFFER_TOKEN;
            inBuffers[0].cbBuffer = static_cast<unsigned long>(inBuf.size());
            inBuffers[0].pvBuffer = inBuf.empty() ? nullptr : inBuf.data();
            inBuffers[1].BufferType = SECBUFFER_EMPTY;
            inBuffers[1].cbBuffer = 0;
            inBuffers[1].pvBuffer = nullptr;

            SecBufferDesc inDesc{ SECBUFFER_VERSION, 2, inBuffers };

            SecBuffer outBuffers[1]{};
            outBuffers[0].BufferType = SECBUFFER_TOKEN;
            outBuffers[0].cbBuffer = 0;
            outBuffers[0].pvBuffer = nullptr;

            SecBufferDesc outDesc{ SECBUFFER_VERSION, 1, outBuffers };
            DWORD outFlags = 0;

            status = InitializeSecurityContextW(
                &session.hCred,
                firstCall ? nullptr : &session.hCtx,
                nullptr,
                iscFlags,
                0,
                SECURITY_NATIVE_DREP,
                firstCall ? nullptr : &inDesc,
                0,
                &session.hCtx,
                &outDesc,
                &outFlags,
                &tsExpiry
            );

            if (status == SEC_E_OK || status == SEC_I_CONTINUE_NEEDED || status == SEC_I_INCOMPLETE_CREDENTIALS) {
                session.hasCtx = true;
                firstCall = false;
            }

            if (outBuffers[0].cbBuffer > 0 && outBuffers[0].pvBuffer != nullptr) {
                bool ok = SendAllBytes(sock, static_cast<const char*>(outBuffers[0].pvBuffer), static_cast<int>(outBuffers[0].cbBuffer));
                FreeContextBuffer(outBuffers[0].pvBuffer);
                if (!ok) return false;
            }

            if (status == SEC_E_INCOMPLETE_MESSAGE) {
                needRecv = true;
                continue;
            }

            if (inBuffers[1].BufferType == SECBUFFER_EXTRA && inBuffers[1].cbBuffer > 0) {
                size_t keep = inBuffers[1].cbBuffer;
                if (keep <= inBuf.size()) {
                    size_t offset = inBuf.size() - keep;
                    std::memmove(inBuf.data(), inBuf.data() + offset, keep);
                    inBuf.resize(keep);
                }
            } else if (!firstCall) {
                inBuf.clear();
            }

            if (status == SEC_E_OK) {
                if (!inBuf.empty()) {
                    session.pendingCipher.insert(session.pendingCipher.end(), inBuf.begin(), inBuf.end());
                }
                status = QueryContextAttributesW(&session.hCtx, SECPKG_ATTR_STREAM_SIZES, &session.streamSizes);
                return (status == SEC_E_OK);
            }

            if (status == SEC_I_CONTINUE_NEEDED) {
                if (inBuf.empty()) needRecv = true;
                continue;
            }

            if (status == SEC_I_INCOMPLETE_CREDENTIALS) {
                // Retry with supplied credentials without blocking on recv
                continue;
            }

            std::wcerr << L"[ERREUR TLS Client] InitializeSecurityContextW = 0x" << std::hex << status << std::dec << std::endl;
            return false;
        }

        return false;
    }

    bool ProtocolHandler::PerformTlsHandshakeServer(SOCKET sock, TlsSession& session) {
        session.sock = sock;

        SCHANNEL_CRED schCred{};
        schCred.dwVersion = SCHANNEL_CRED_VERSION;
        if (m_localCert) {
            schCred.cCreds = 1;
            schCred.paCred = &m_localCert;
        }
        schCred.grbitEnabledProtocols = 0; // Let OS negotiate TLS 1.2 / 1.3
        schCred.dwFlags = SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS;

        TimeStamp tsExpiry{};
        SECURITY_STATUS status = AcquireCredentialsHandleW(
            nullptr,
            const_cast<LPWSTR>(UNISP_NAME_W),
            SECPKG_CRED_INBOUND,
            nullptr,
            &schCred,
            nullptr,
            nullptr,
            &session.hCred,
            &tsExpiry
        );

        if (status != SEC_E_OK) {
            std::wcerr << L"[ERREUR TLS Serveur] AcquireCredentialsHandleW = 0x" << std::hex << status << std::dec << std::endl;
            return false;
        }
        session.hasCred = true;

        DWORD ascFlags = ASC_REQ_SEQUENCE_DETECT |
                         ASC_REQ_REPLAY_DETECT |
                         ASC_REQ_CONFIDENTIALITY |
                         ASC_REQ_EXTENDED_ERROR |
                         ASC_REQ_ALLOCATE_MEMORY |
                         ASC_REQ_STREAM;

        std::vector<char> inBuf;
        bool needRecv = true;

        while (m_running) {
            if (needRecv) {
                char chunk[8192];
                int r = recv(sock, chunk, sizeof(chunk), 0);
                if (r <= 0) {
                    return false;
                }
                inBuf.insert(inBuf.end(), chunk, chunk + r);
                needRecv = false;
            }

            SecBuffer inBuffers[2]{};
            inBuffers[0].BufferType = SECBUFFER_TOKEN;
            inBuffers[0].cbBuffer = static_cast<unsigned long>(inBuf.size());
            inBuffers[0].pvBuffer = inBuf.data();
            inBuffers[1].BufferType = SECBUFFER_EMPTY;
            inBuffers[1].cbBuffer = 0;
            inBuffers[1].pvBuffer = nullptr;

            SecBufferDesc inDesc{ SECBUFFER_VERSION, 2, inBuffers };

            SecBuffer outBuffers[1]{};
            outBuffers[0].BufferType = SECBUFFER_TOKEN;
            outBuffers[0].cbBuffer = 0;
            outBuffers[0].pvBuffer = nullptr;

            SecBufferDesc outDesc{ SECBUFFER_VERSION, 1, outBuffers };
            DWORD outFlags = 0;

            status = AcceptSecurityContext(
                &session.hCred,
                session.hasCtx ? &session.hCtx : nullptr,
                &inDesc,
                ascFlags,
                SECURITY_NATIVE_DREP,
                &session.hCtx,
                &outDesc,
                &outFlags,
                &tsExpiry
            );

            if (status == SEC_E_OK || status == SEC_I_CONTINUE_NEEDED) {
                session.hasCtx = true;
            }

            if (outBuffers[0].cbBuffer > 0 && outBuffers[0].pvBuffer != nullptr) {
                bool ok = SendAllBytes(sock, static_cast<const char*>(outBuffers[0].pvBuffer), static_cast<int>(outBuffers[0].cbBuffer));
                FreeContextBuffer(outBuffers[0].pvBuffer);
                if (!ok) return false;
            }

            if (status == SEC_E_INCOMPLETE_MESSAGE) {
                needRecv = true;
                continue;
            }

            if (inBuffers[1].BufferType == SECBUFFER_EXTRA && inBuffers[1].cbBuffer > 0) {
                size_t keep = inBuffers[1].cbBuffer;
                if (keep <= inBuf.size()) {
                    size_t offset = inBuf.size() - keep;
                    std::memmove(inBuf.data(), inBuf.data() + offset, keep);
                    inBuf.resize(keep);
                }
            } else {
                inBuf.clear();
            }

            if (status == SEC_E_OK) {
                if (!inBuf.empty()) {
                    session.pendingCipher.insert(session.pendingCipher.end(), inBuf.begin(), inBuf.end());
                }
                status = QueryContextAttributesW(&session.hCtx, SECPKG_ATTR_STREAM_SIZES, &session.streamSizes);
                return (status == SEC_E_OK);
            }

            if (status == SEC_I_CONTINUE_NEEDED) {
                if (inBuf.empty()) needRecv = true;
                continue;
            }

            std::wcerr << L"[ERREUR TLS Serveur] AcceptSecurityContext = 0x" << std::hex << status << std::dec << std::endl;
            return false;
        }

        return false;
    }

    bool ProtocolHandler::TlsSendAll(TlsSession& session, const std::string& plaintext) {
        std::lock_guard<std::mutex> lock(session.sendMutex);
        if (!session.hasCtx || session.sock == INVALID_SOCKET) return false;

        size_t offset = 0;
        size_t maxChunk = session.streamSizes.cbMaximumMessage > 0 ? session.streamSizes.cbMaximumMessage : 16384;

        while (offset < plaintext.size()) {
            size_t chunkLen = std::min(maxChunk, plaintext.size() - offset);
            size_t totalAlloc = session.streamSizes.cbHeader + chunkLen + session.streamSizes.cbTrailer;
            std::vector<BYTE> ioBuffer(totalAlloc);

            std::memcpy(ioBuffer.data() + session.streamSizes.cbHeader, plaintext.data() + offset, chunkLen);

            SecBuffer buffers[4]{};
            buffers[0].BufferType = SECBUFFER_STREAM_HEADER;
            buffers[0].cbBuffer = session.streamSizes.cbHeader;
            buffers[0].pvBuffer = ioBuffer.data();

            buffers[1].BufferType = SECBUFFER_DATA;
            buffers[1].cbBuffer = static_cast<unsigned long>(chunkLen);
            buffers[1].pvBuffer = ioBuffer.data() + session.streamSizes.cbHeader;

            buffers[2].BufferType = SECBUFFER_STREAM_TRAILER;
            buffers[2].cbBuffer = session.streamSizes.cbTrailer;
            buffers[2].pvBuffer = ioBuffer.data() + session.streamSizes.cbHeader + chunkLen;

            buffers[3].BufferType = SECBUFFER_EMPTY;

            SecBufferDesc msgDesc{ SECBUFFER_VERSION, 4, buffers };
            SECURITY_STATUS status = EncryptMessage(&session.hCtx, 0, &msgDesc, 0);
            if (status != SEC_E_OK) {
                return false;
            }

            int sendLen = static_cast<int>(buffers[0].cbBuffer + buffers[1].cbBuffer + buffers[2].cbBuffer);
            if (!SendAllBytes(session.sock, reinterpret_cast<const char*>(ioBuffer.data()), sendLen)) {
                return false;
            }

            offset += chunkLen;
        }

        return true;
    }

    bool ProtocolHandler::TlsRecvChunk(TlsSession& session, std::string& outPlaintext) {
        outPlaintext.clear();

        while (m_running) {
            if (!session.pendingCipher.empty()) {
                SecBuffer buffers[4]{};
                buffers[0].BufferType = SECBUFFER_DATA;
                buffers[0].cbBuffer = static_cast<unsigned long>(session.pendingCipher.size());
                buffers[0].pvBuffer = session.pendingCipher.data();
                buffers[1].BufferType = SECBUFFER_EMPTY;
                buffers[2].BufferType = SECBUFFER_EMPTY;
                buffers[3].BufferType = SECBUFFER_EMPTY;

                SecBufferDesc msgDesc{ SECBUFFER_VERSION, 4, buffers };
                SECURITY_STATUS status = DecryptMessage(&session.hCtx, &msgDesc, 0, nullptr);

                if (status == SEC_E_OK || status == SEC_I_CONTEXT_EXPIRED) {
                    const char* plainPtr = nullptr;
                    unsigned long plainLen = 0;
                    const char* extraPtr = nullptr;
                    unsigned long extraLen = 0;

                    for (int i = 0; i < 4; ++i) {
                        if (buffers[i].BufferType == SECBUFFER_DATA) {
                            plainPtr = static_cast<const char*>(buffers[i].pvBuffer);
                            plainLen = buffers[i].cbBuffer;
                        } else if (buffers[i].BufferType == SECBUFFER_EXTRA) {
                            extraPtr = static_cast<const char*>(buffers[i].pvBuffer);
                            extraLen = buffers[i].cbBuffer;
                        }
                    }

                    if (plainPtr && plainLen > 0) {
                        outPlaintext.append(plainPtr, plainLen);
                    }

                    if (extraPtr && extraLen > 0) {
                        std::memmove(session.pendingCipher.data(), extraPtr, extraLen);
                        session.pendingCipher.resize(extraLen);
                    } else {
                        session.pendingCipher.clear();
                    }

                    if (status == SEC_I_CONTEXT_EXPIRED && outPlaintext.empty()) {
                        return false;
                    }
                    if (!outPlaintext.empty()) {
                        return true;
                    }
                    continue;
                }

                if (status == SEC_E_INCOMPLETE_MESSAGE) {
                    // Fall through to recv more bytes
                } else {
                    return false;
                }
            }

            char chunk[8192];
            int r = recv(session.sock, chunk, sizeof(chunk), 0);
            if (r <= 0) return false;
            session.pendingCipher.insert(session.pendingCipher.end(), chunk, chunk + r);
        }

        return false;
    }

    std::string ProtocolHandler::BuildIdentityPacket() const {
        KDEPacket pkt;
        pkt.id = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        pkt.type = "kdeconnect.identity";

        std::ostringstream ss;
        ss << "{"
           << "\"deviceId\":\"" << EscapeJsonString(WideToUtf8(m_localIdentity.deviceId)) << "\","
           << "\"deviceName\":\"" << EscapeJsonString(WideToUtf8(m_localIdentity.deviceName)) << "\","
           << "\"deviceType\":\"desktop\","
           << "\"protocolVersion\":8,"
           << "\"tcpPort\":" << m_port << ","
           << "\"incomingCapabilities\":["
           << "\"kdeconnect.ping\","
           << "\"kdeconnect.telephony\","
           << "\"kdeconnect.sms\","
           << "\"kdeconnect.sms.messages\","
           << "\"kdeconnect.battery\""
           << "],"
           << "\"outgoingCapabilities\":["
           << "\"kdeconnect.ping\","
           << "\"kdeconnect.sms.request\","
           << "\"kdeconnect.sms.request_conversations\","
           << "\"kdeconnect.sms.request_conversation\""
           << "]"
           << "}";
        pkt.bodyJson = ss.str();
        return pkt.Serialize();
    }

    std::vector<std::wstring> ProtocolHandler::GetLocalIpAddresses() {
        std::vector<std::wstring> ips;
        char hostname[256];
        if (gethostname(hostname, sizeof(hostname)) == 0) {
            addrinfo hints{};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            addrinfo* res = nullptr;
            if (getaddrinfo(hostname, nullptr, &hints, &res) == 0) {
                for (addrinfo* ptr = res; ptr != nullptr; ptr = ptr->ai_next) {
                    sockaddr_in* s = reinterpret_cast<sockaddr_in*>(ptr->ai_addr);
                    char ipStr[INET_ADDRSTRLEN];
                    if (inet_ntop(AF_INET, &s->sin_addr, ipStr, sizeof(ipStr))) {
                        std::string str(ipStr);
                        if (str != "127.0.0.1") {
                            ips.push_back(Utf8ToWide(str));
                        }
                    }
                }
                freeaddrinfo(res);
            }
        }
        return ips;
    }

    bool ProtocolHandler::Start(const std::wstring& bindIp, uint16_t port) {
        m_bindIp = bindIp;
        m_port = port;
        m_running = true;

        if (!EnsureLocalCertificate()) {
            std::wcerr << L"[AVERTISSEMENT] Impossible de generer le certificat TLS local !" << std::endl;
        }

        m_udpSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (m_udpSocket == INVALID_SOCKET) {
            std::wcerr << L"[ERREUR UDP] Impossible de creer le socket UDP (code " << WSAGetLastError() << L")" << std::endl;
            return false;
        }

        BOOL opt = TRUE;
        setsockopt(m_udpSocket, SOL_SOCKET, SO_BROADCAST, (char*)&opt, sizeof(opt));
        setsockopt(m_udpSocket, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

        // On Windows, the UDP listening socket MUST bind to INADDR_ANY (0.0.0.0) to receive broadcasts
        sockaddr_in bindAddr{};
        bindAddr.sin_family = AF_INET;
        bindAddr.sin_port = htons(m_port);
        bindAddr.sin_addr.s_addr = INADDR_ANY;

        if (bind(m_udpSocket, (sockaddr*)&bindAddr, sizeof(bindAddr)) == SOCKET_ERROR) {
            int err = WSAGetLastError();
            std::wcerr << L"[ERREUR UDP] Echec bind sur 0.0.0.0:" << m_port << L" (code " << err << L")" << std::endl;
            if (err == 10048) {
                std::wcerr << L"             -> Le port 1716 est deja utilise ! Fermez l'application KDE Connect officielle." << std::endl;
            }
            closesocket(m_udpSocket);
            m_udpSocket = INVALID_SOCKET;
            return false;
        } else {
            std::wcout << L"[UDP Serveur] Ecoute active sur 0.0.0.0:" << m_port << L" (Reception diffusions OK)" << std::endl;
        }

        m_tcpListenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (m_tcpListenSocket != INVALID_SOCKET) {
            setsockopt(m_tcpListenSocket, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
            sockaddr_in tcpBindAddr{};
            tcpBindAddr.sin_family = AF_INET;
            tcpBindAddr.sin_port = htons(m_port);
            tcpBindAddr.sin_addr.s_addr = INADDR_ANY;

            if (bind(m_tcpListenSocket, (sockaddr*)&tcpBindAddr, sizeof(tcpBindAddr)) == SOCKET_ERROR) {
                std::wcerr << L"[ERREUR TCP] Echec bind sur port " << m_port << L" (code " << WSAGetLastError() << L")" << std::endl;
            } else {
                listen(m_tcpListenSocket, SOMAXCONN);
                std::wcout << L"[TCP Serveur] Ecoute active sur 0.0.0.0:" << m_port << L" (Pret pour TLS & appairage)" << std::endl;
            }
        }

        m_udpThread = std::thread(&ProtocolHandler::UdpDiscoveryThread, this);
        m_tcpThread = std::thread(&ProtocolHandler::TcpServerThread, this);
        m_broadcastThread = std::thread(&ProtocolHandler::PeriodicBroadcastThread, this);

        BroadcastIdentity();
        return true;
    }

    void ProtocolHandler::Stop() {
        m_running = false;

        if (m_udpSocket != INVALID_SOCKET) {
            closesocket(m_udpSocket);
            m_udpSocket = INVALID_SOCKET;
        }

        if (m_tcpListenSocket != INVALID_SOCKET) {
            closesocket(m_tcpListenSocket);
            m_tcpListenSocket = INVALID_SOCKET;
        }

        {
            std::lock_guard<std::mutex> lock(m_connMutex);
            for (auto& kv : m_activeSessions) {
                if (kv.second && kv.second->sock != INVALID_SOCKET) {
                    closesocket(kv.second->sock);
                }
            }
            m_activeSessions.clear();
        }

        if (m_broadcastThread.joinable()) m_broadcastThread.join();
        if (m_udpThread.joinable()) m_udpThread.join();
        if (m_tcpThread.joinable()) m_tcpThread.join();
    }

    void ProtocolHandler::PeriodicBroadcastThread() {
        while (m_running) {
            BroadcastIdentity();
            for (int i = 0; i < 8 && m_running; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        }
    }

    void ProtocolHandler::BroadcastIdentity() {
        if (m_udpSocket == INVALID_SOCKET) return;

        std::string raw = BuildIdentityPacket();

        std::vector<std::string> targetIps;
        if (!m_bindIp.empty()) {
            targetIps.push_back(WideToUtf8(m_bindIp));
        } else {
            auto ips = GetLocalIpAddresses();
            for (const auto& w : ips) targetIps.push_back(WideToUtf8(w));
        }

        for (const auto& ipUtf8 : targetIps) {
            in_addr localInAddr{};
            if (inet_pton(AF_INET, ipUtf8.c_str(), &localInAddr) == 1) {
                uint32_t hostOrderIp = ntohl(localInAddr.s_addr);
                uint32_t subnetBcast = (hostOrderIp & 0xFFFFFF00) | 0x000000FF;

                in_addr bcastAddrIn{};
                bcastAddrIn.s_addr = htonl(subnetBcast);
                char bcastIpStr[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &bcastAddrIn, bcastIpStr, sizeof(bcastIpStr));

                SOCKET senderSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
                if (senderSock != INVALID_SOCKET) {
                    BOOL opt = TRUE;
                    setsockopt(senderSock, SOL_SOCKET, SO_BROADCAST, (char*)&opt, sizeof(opt));

                    sockaddr_in ifAddr{};
                    ifAddr.sin_family = AF_INET;
                    ifAddr.sin_port = 0;
                    ifAddr.sin_addr = localInAddr;

                    if (bind(senderSock, (sockaddr*)&ifAddr, sizeof(ifAddr)) != SOCKET_ERROR) {
                        sockaddr_in destSubnet{};
                        destSubnet.sin_family = AF_INET;
                        destSubnet.sin_port = htons(m_port);
                        destSubnet.sin_addr.s_addr = htonl(subnetBcast);

                        int res = sendto(senderSock, raw.c_str(), static_cast<int>(raw.length()), 0,
                                         (sockaddr*)&destSubnet, sizeof(destSubnet));
                        if (res != SOCKET_ERROR) {
                            std::wcout << L"[UDP Annonce] Envoyee sur " << Utf8ToWide(bcastIpStr)
                                       << L":" << m_port << L" (via " << Utf8ToWide(ipUtf8) << L") [OK]" << std::endl;
                        } else {
                            std::wcout << L"[UDP Annonce] Echec sur " << Utf8ToWide(bcastIpStr)
                                       << L" (code " << WSAGetLastError() << L")" << std::endl;
                        }

                        sockaddr_in destGlobal{};
                        destGlobal.sin_family = AF_INET;
                        destGlobal.sin_port = htons(m_port);
                        destGlobal.sin_addr.s_addr = INADDR_BROADCAST;
                        sendto(senderSock, raw.c_str(), static_cast<int>(raw.length()), 0,
                               (sockaddr*)&destGlobal, sizeof(destGlobal));
                    }
                    closesocket(senderSock);
                }
            }
        }

        sockaddr_in globalAddr{};
        globalAddr.sin_family = AF_INET;
        globalAddr.sin_port = htons(m_port);
        globalAddr.sin_addr.s_addr = INADDR_BROADCAST;
        sendto(m_udpSocket, raw.c_str(), static_cast<int>(raw.length()), 0,
               (sockaddr*)&globalAddr, sizeof(globalAddr));
    }

    void ProtocolHandler::UdpDiscoveryThread() {
        char buffer[4096];
        sockaddr_in senderAddr{};
        int senderLen = sizeof(senderAddr);

        while (m_running) {
            senderLen = sizeof(senderAddr);
            int received = recvfrom(m_udpSocket, buffer, sizeof(buffer) - 1, 0, (sockaddr*)&senderAddr, &senderLen);
            if (received <= 0) {
                if (!m_running) break;
                continue;
            }

            buffer[received] = '\0';
            KDEPacket pkt;
            if (KDEPacket::Deserialize(buffer, pkt)) {
                char senderIp[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &senderAddr.sin_addr, senderIp, sizeof(senderIp));
                std::wstring senderIpW = Utf8ToWide(senderIp);

                if (pkt.type == "kdeconnect.identity") {
                    DeviceInfo dev = ParseIdentityBody(pkt, senderIpW, 1716);

                    if (!dev.deviceId.empty() && dev.deviceId != m_localIdentity.deviceId) {
                        std::wcout << L"\n[UDP Recu] Paquet decouvert de " << dev.ipAddress
                                   << L":" << ntohs(senderAddr.sin_port)
                                   << L" | Appareil: " << (dev.deviceName.empty() ? L"Telephone" : dev.deviceName)
                                   << L" (ID: " << dev.deviceId << L", Port TCP: " << dev.port << L")" << std::endl;

                        // Direct UDP reply so phone immediately sees our PC
                        std::string rawResp = BuildIdentityPacket();
                        sendto(m_udpSocket, rawResp.c_str(), static_cast<int>(rawResp.length()), 0,
                               (sockaddr*)&senderAddr, sizeof(senderAddr));

                        sockaddr_in target1716 = senderAddr;
                        target1716.sin_port = htons(m_port);
                        sendto(m_udpSocket, rawResp.c_str(), static_cast<int>(rawResp.length()), 0,
                               (sockaddr*)&target1716, sizeof(target1716));

                        if (dev.port != m_port) {
                            sockaddr_in targetPhoneTcp = senderAddr;
                            targetPhoneTcp.sin_port = htons(dev.port);
                            sendto(m_udpSocket, rawResp.c_str(), static_cast<int>(rawResp.length()), 0,
                                   (sockaddr*)&targetPhoneTcp, sizeof(targetPhoneTcp));
                        }

                        // Throttle outbound TCP connection attempts per deviceId
                        bool shouldConnectTcp = false;
                        {
                            std::lock_guard<std::mutex> lock(m_connMutex);
                            bool alreadyConnected = (m_activeSessions.find(dev.deviceId) != m_activeSessions.end());
                            if (!alreadyConnected) {
                                auto now = std::chrono::steady_clock::now();
                                auto it = m_lastTcpAttempt.find(dev.deviceId);
                                if (it == m_lastTcpAttempt.end() ||
                                    std::chrono::duration_cast<std::chrono::seconds>(now - it->second).count() >= 5) {
                                    m_lastTcpAttempt[dev.deviceId] = now;
                                    shouldConnectTcp = true;
                                }
                            }
                        }

                        if (shouldConnectTcp) {
                            std::thread(&ProtocolHandler::ConnectAndSendIdentityTcp, this, dev).detach();
                        }
                    }
                }
            }
        }
    }

    void ProtocolHandler::RunEncryptedPacketLoop(std::shared_ptr<TlsSession> session, DeviceInfo dev) {
        if (!dev.deviceId.empty()) {
            std::lock_guard<std::mutex> lock(m_connMutex);
            m_activeSessions[dev.deviceId] = session;
        }
        {
            std::lock_guard<std::mutex> lock(m_devicesMutex);
            auto it = std::find_if(m_knownDevices.begin(), m_knownDevices.end(), [&](const DeviceInfo& d) {
                return d.deviceId == dev.deviceId;
            });
            if (it == m_knownDevices.end()) {
                m_knownDevices.push_back(dev);
                std::wcout << L"[+] Appareil pret et visible sur le reseau: "
                           << (dev.deviceName.empty() ? dev.ipAddress : dev.deviceName)
                           << L" (" << dev.ipAddress << L")" << std::endl;
                if (m_onDeviceDiscovered) m_onDeviceDiscovered(dev);
            } else {
                it->ipAddress = dev.ipAddress;
                it->port = dev.port;
                it->isReachable = true;
            }
        }

        std::string bufferAccumulator;
        std::string chunk;
        bool pairReplySent = false;

        while (m_running && TlsRecvChunk(*session, chunk)) {
            bufferAccumulator += chunk;

            size_t newlinePos;
            while ((newlinePos = bufferAccumulator.find('\n')) != std::string::npos) {
                std::string packetLine = bufferAccumulator.substr(0, newlinePos);
                bufferAccumulator.erase(0, newlinePos + 1);

                KDEPacket pkt;
                if (KDEPacket::Deserialize(packetLine, pkt)) {
                    std::wcout << L"[TLS Paquet] Type: " << Utf8ToWide(pkt.type)
                               << L" recu de " << (dev.deviceName.empty() ? dev.ipAddress : dev.deviceName) << std::endl;

                    if (pkt.type == "kdeconnect.identity") {
                        std::wcout << L"[TLS Identite] Identite chiffree confirmee (Protocole v8 OK) !" << std::endl;
                    } else if (pkt.type == "kdeconnect.pair") {
                        bool wantsPair = (pkt.bodyJson.find("true") != std::string::npos);
                        if (wantsPair && !pairReplySent) {
                            pairReplySent = true;
                            std::wcout << L"[Appairage] Demande d'appairage TLS recue ! Validation automatique..." << std::endl;
                            KDEPacket pairReply;
                            pairReply.id = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch()).count();
                            pairReply.type = "kdeconnect.pair";
                            pairReply.bodyJson = "{\"pair\":true}";
                            TlsSendAll(*session, pairReply.Serialize());
                            std::wcout << L"[Appairage] Confirmation chiffree envoyee au telephone !" << std::endl;
                        }
                    } else if (pkt.type == "kdeconnect.ping") {
                        std::wcout << L"[Ping] Ping recu du telephone, reponse..." << std::endl;
                        KDEPacket pingReply;
                        pingReply.id = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();
                        pingReply.type = "kdeconnect.ping";
                        pingReply.bodyJson = "{}";
                        TlsSendAll(*session, pingReply.Serialize());
                    }

                    if (m_onPacketReceived) {
                        m_onPacketReceived(dev, pkt);
                    }
                }
            }
        }

        if (!dev.deviceId.empty()) {
            std::lock_guard<std::mutex> lock(m_connMutex);
            auto it = m_activeSessions.find(dev.deviceId);
            if (it != m_activeSessions.end() && it->second == session) {
                m_activeSessions.erase(it);
            }
        }
        CleanupTlsSession(*session);
        std::wcout << L"[TLS Session] Connexion fermee avec " << dev.ipAddress << std::endl;
    }

    void ProtocolHandler::ConnectAndSendIdentityTcp(const DeviceInfo& device) {
        // Slight delay to let phone initiate incoming TCP first if it wants
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        if (!m_running) return;

        {
            std::lock_guard<std::mutex> lock(m_connMutex);
            if (!device.deviceId.empty() && m_activeSessions.find(device.deviceId) != m_activeSessions.end()) {
                return;
            }
        }

        SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock == INVALID_SOCKET) return;

        DWORD timeoutMs = 15000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));

        sockaddr_in destAddr{};
        destAddr.sin_family = AF_INET;
        destAddr.sin_port = htons(device.port > 0 ? device.port : 1716);
        std::string ipUtf8 = WideToUtf8(device.ipAddress);
        inet_pton(AF_INET, ipUtf8.c_str(), &destAddr.sin_addr);

        std::wcout << L"[TCP Sortant] Connexion vers " << device.ipAddress
                   << L":" << ntohs(destAddr.sin_port) << L"..." << std::endl;

        if (connect(sock, reinterpret_cast<sockaddr*>(&destAddr), sizeof(destAddr)) == SOCKET_ERROR) {
            std::wcout << L"[TCP Sortant] Connexion impossible vers " << device.ipAddress
                       << L":" << ntohs(destAddr.sin_port) << L" (code " << WSAGetLastError() << L")" << std::endl;
            closesocket(sock);
            return;
        }

        // KDE Connect rule: TCP client writes unencrypted identity FIRST, then acts as TLS SERVER
        std::string rawIdentity = BuildIdentityPacket();
        if (!SendAllBytes(sock, rawIdentity.c_str(), static_cast<int>(rawIdentity.length()))) {
            closesocket(sock);
            return;
        }
        std::wcout << L"[TCP Sortant] Identite (en clair) envoyee. Passage en mode TLS Serveur..." << std::endl;

        auto session = std::make_shared<TlsSession>();
        if (!PerformTlsHandshakeServer(sock, *session)) {
            std::wcerr << L"[TLS Serveur] Echec handshake TLS avec " << device.ipAddress << std::endl;
            CleanupTlsSession(*session);
            return;
        }

        std::wcout << L"[TLS Serveur] Tunnel chiffre etabli avec " << device.ipAddress << L" ! Envoi identite TLS..." << std::endl;

        // Disable short receive timeout for long-lived idle TLS session
        DWORD noTimeout = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&noTimeout), sizeof(noTimeout));

        // Protocol v8: immediately send encrypted identity packet over TLS
        if (!TlsSendAll(*session, BuildIdentityPacket())) {
            CleanupTlsSession(*session);
            return;
        }

        RunEncryptedPacketLoop(session, device);
    }

    void ProtocolHandler::TcpServerThread() {
        while (m_running && m_tcpListenSocket != INVALID_SOCKET) {
            sockaddr_in clientAddr{};
            int clientLen = sizeof(clientAddr);
            SOCKET clientSocket = accept(m_tcpListenSocket, reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);
            if (clientSocket == INVALID_SOCKET) {
                if (!m_running) break;
                continue;
            }

            std::thread(&ProtocolHandler::HandleClientConnection, this, clientSocket).detach();
        }
    }

    void ProtocolHandler::HandleClientConnection(SOCKET clientSocket) {
        sockaddr_in clientAddr{};
        int addrLen = sizeof(clientAddr);
        getpeername(clientSocket, reinterpret_cast<sockaddr*>(&clientAddr), &addrLen);
        char peerIp[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &clientAddr.sin_addr, peerIp, sizeof(peerIp));
        std::wstring peerIpW = Utf8ToWide(peerIp);

        DWORD timeoutMs = 15000;
        setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
        setsockopt(clientSocket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));

        std::wcout << L"[TCP Entrant] Connexion acceptee depuis " << peerIpW
                   << L":" << ntohs(clientAddr.sin_port) << std::endl;

        // KDE Connect rule: TCP server reads 1 unencrypted identity line FIRST, then acts as TLS CLIENT
        std::string firstLine;
        if (!ReadPlaintextLine(clientSocket, firstLine)) {
            std::wcout << L"[TCP Entrant] Aucune identite recue avant fermeture depuis " << peerIpW << std::endl;
            closesocket(clientSocket);
            return;
        }

        KDEPacket initPkt;
        if (!KDEPacket::Deserialize(firstLine, initPkt) || initPkt.type != "kdeconnect.identity") {
            std::wcout << L"[TCP Entrant] Premier paquet invalide depuis " << peerIpW << std::endl;
            closesocket(clientSocket);
            return;
        }

        DeviceInfo dev = ParseIdentityBody(initPkt, peerIpW, 1716);
        if (dev.deviceId == m_localIdentity.deviceId) {
            closesocket(clientSocket);
            return;
        }

        std::wcout << L"[TCP Entrant] Identite recue: " << (dev.deviceName.empty() ? peerIpW : dev.deviceName)
                   << L" (ID: " << dev.deviceId << L"). Passage en mode TLS Client..." << std::endl;

        auto session = std::make_shared<TlsSession>();
        if (!PerformTlsHandshakeClient(clientSocket, *session)) {
            std::wcerr << L"[TLS Client] Echec handshake TLS avec " << peerIpW << std::endl;
            CleanupTlsSession(*session);
            return;
        }

        std::wcout << L"[TLS Client] Tunnel chiffre etabli avec " << peerIpW << L" ! Envoi identite TLS..." << std::endl;

        DWORD noTimeout = 0;
        setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&noTimeout), sizeof(noTimeout));

        // Protocol v8: immediately send encrypted identity packet over TLS
        if (!TlsSendAll(*session, BuildIdentityPacket())) {
            CleanupTlsSession(*session);
            return;
        }

        RunEncryptedPacketLoop(session, dev);
    }

    bool ProtocolHandler::SendPacket(const DeviceInfo& device, const KDEPacket& packet) {
        std::shared_ptr<TlsSession> targetSession;
        {
            std::lock_guard<std::mutex> lock(m_connMutex);
            if (!device.deviceId.empty()) {
                auto it = m_activeSessions.find(device.deviceId);
                if (it != m_activeSessions.end()) targetSession = it->second;
            }
            if (!targetSession && !m_activeSessions.empty()) {
                targetSession = m_activeSessions.begin()->second;
            }
        }

        if (targetSession) {
            return TlsSendAll(*targetSession, packet.Serialize());
        }

        return false;
    }

    bool ProtocolHandler::RequestAllConversations(const DeviceInfo& device) {
        KDEPacket pkt;
        pkt.id = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        pkt.type = "kdeconnect.sms.request_conversations";
        pkt.bodyJson = "{}";
        return SendPacket(device, pkt);
    }

    bool ProtocolHandler::RequestConversationMessages(const DeviceInfo& device, int64_t threadId, int64_t rangeStart) {
        KDEPacket pkt;
        pkt.id = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        pkt.type = "kdeconnect.sms.request_conversation";
        std::ostringstream ss;
        ss << "{\"threadID\":" << threadId << ",\"rangeStartTimestamp\":" << rangeStart << "}";
        pkt.bodyJson = ss.str();
        return SendPacket(device, pkt);
    }

    bool ProtocolHandler::SendSms(const DeviceInfo& device, const std::wstring& phoneNumber, const std::wstring& text) {
        KDEPacket pkt;
        pkt.id = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        pkt.type = "kdeconnect.sms.request";

        std::ostringstream ss;
        ss << "{"
           << "\"sendSms\":true,"
           << "\"phoneNumber\":\"" << EscapeJsonString(WideToUtf8(phoneNumber)) << "\","
           << "\"messageBody\":\"" << EscapeJsonString(WideToUtf8(text)) << "\""
           << "}";
        pkt.bodyJson = ss.str();
        return SendPacket(device, pkt);
    }

} // namespace KDEConnect
