// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "winutils.h"

#include "filepath.h"
#include "qtcassert.h"

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include <QLibrary>
#include <QString>
#include <QTextStream>

namespace Utils {

QTCREATOR_UTILS_EXPORT QString winErrorMessage(unsigned long error)
{
    QString rc = QString::fromLatin1("#%1: ").arg(error);
#ifdef Q_OS_WIN
    char16_t *lpMsgBuf;

    const int len = FormatMessage(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            NULL, error, 0, (LPTSTR)&lpMsgBuf, 0, NULL);
    if (len) {
        rc = QString::fromUtf16(lpMsgBuf, len);
        LocalFree(lpMsgBuf);
    } else {
        rc += QString::fromLatin1("<unknown error>");
    }
#endif
    return rc;
}


#ifdef Q_OS_WIN
static inline QString msgCannotLoad(const char *lib, const QString &why)
{
    return QString::fromLatin1("Unable load %1: %2").arg(QLatin1String(lib), why);
}

static inline QString msgCannotResolve(const char *lib)
{
    return QString::fromLatin1("Unable to resolve all required symbols in  %1").arg(QLatin1String(lib));
}
#endif

QTCREATOR_UTILS_EXPORT QString winGetDLLVersion(WinDLLVersionType t,
                                                const QString &name,
                                                QString *errorMessage)
{
#ifdef Q_OS_WIN
    // Resolve required symbols from the version.dll
    using GetFileVersionInfoSizeProtoType = DWORD (APIENTRY*)(LPCTSTR, LPDWORD);
    using GetFileVersionInfoWProtoType = BOOL (APIENTRY*)(LPCWSTR, DWORD, DWORD, LPVOID);
    using VerQueryValueWProtoType = BOOL (APIENTRY*)(const LPVOID, LPWSTR lpSubBlock, LPVOID, PUINT);

    const char *versionDLLC = "version.dll";
    QLibrary versionLib(QLatin1String(versionDLLC), 0);
    if (!versionLib.load()) {
        *errorMessage = msgCannotLoad(versionDLLC, versionLib.errorString());
        return QString();
    }
    // MinGW requires old-style casts
    auto getFileVersionInfoSizeW = (GetFileVersionInfoSizeProtoType)(versionLib.resolve("GetFileVersionInfoSizeW"));
    auto getFileVersionInfoW = (GetFileVersionInfoWProtoType)(versionLib.resolve("GetFileVersionInfoW"));
    auto verQueryValueW = (VerQueryValueWProtoType)(versionLib.resolve("VerQueryValueW"));
    if (!getFileVersionInfoSizeW || !getFileVersionInfoW || !verQueryValueW) {
        *errorMessage = msgCannotResolve(versionDLLC);
        return QString();
    }

    // Now go ahead, read version info resource
    DWORD dummy = 0;
    const auto fileName = reinterpret_cast<LPCTSTR>(name.utf16()); // MinGWsy
    const DWORD infoSize = (*getFileVersionInfoSizeW)(fileName, &dummy);
    if (infoSize == 0) {
        *errorMessage = QString::fromLatin1("Unable to determine the size of the version information of %1: %2").arg(name, winErrorMessage(GetLastError()));
        return QString();
    }
    QByteArray dataV(infoSize + 1, '\0');
    char *data = dataV.data();
    if (!(*getFileVersionInfoW)(fileName, dummy, infoSize, data)) {
        *errorMessage = QString::fromLatin1("Unable to determine the version information of %1: %2").arg(name, winErrorMessage(GetLastError()));
        return QString();
    }
    VS_FIXEDFILEINFO  *versionInfo;
    const LPCWSTR backslash = TEXT("\\");
    UINT len = 0;
    if (!(*verQueryValueW)(data, const_cast<LPWSTR>(backslash), &versionInfo, &len)) {
        *errorMessage = QString::fromLatin1("Unable to determine version string of %1: %2").arg(name, winErrorMessage(GetLastError()));
        return QString();
    }
    QString rc;
    switch (t) {
    case WinDLLFileVersion:
        QTextStream(&rc) << HIWORD(versionInfo->dwFileVersionMS) << '.'
                         << LOWORD(versionInfo->dwFileVersionMS) << '.'
                         << HIWORD(versionInfo->dwFileVersionLS) << '.'
                         << LOWORD(versionInfo->dwFileVersionLS);
        break;
    case WinDLLProductVersion:
        QTextStream(&rc) << HIWORD(versionInfo->dwProductVersionMS) << '.'
                         << LOWORD(versionInfo->dwProductVersionMS) << '.'
                         << HIWORD(versionInfo->dwProductVersionLS) << '.'
                         << LOWORD(versionInfo->dwProductVersionLS);
        break;
    }
    return rc;
#endif
    Q_UNUSED(t)
    Q_UNUSED(name)
    Q_UNUSED(errorMessage)
    return QString();
}

QTCREATOR_UTILS_EXPORT bool is64BitWindowsSystem()
{
#ifdef Q_OS_WIN
    SYSTEM_INFO systemInfo;
    GetNativeSystemInfo(&systemInfo);
    return systemInfo.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64
            || systemInfo.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_IA64
            || systemInfo.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64;
#else
    return false;
#endif
}

QTCREATOR_UTILS_EXPORT bool is64BitWindowsBinary(const FilePath &binaryIn)
{
       QTC_ASSERT(!binaryIn.isEmpty() && binaryIn.isLocal(), return false);
#ifdef Q_OS_WIN32
#  ifdef __GNUC__   // MinGW lacking some definitions/winbase.h
#    define SCS_64BIT_BINARY 6
#  endif
        bool isAmd64 = false;
        DWORD binaryType = 0;
        const QString binary = binaryIn.nativePath();
        bool success = GetBinaryTypeW(reinterpret_cast<const TCHAR*>(binary.utf16()), &binaryType) != 0;
        if (success && binaryType == SCS_64BIT_BINARY)
            isAmd64=true;
        return isAmd64;
#else
        return false;
#endif
}

QTCREATOR_UTILS_EXPORT QString imageName(quint32 processId)
{
    QString result;
#ifdef Q_OS_WIN
    HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (handle == NULL)
        return result;

    wchar_t path[MAX_PATH];
    DWORD pathLen = MAX_PATH;
    if (QueryFullProcessImageName(handle, 0, path, &pathLen))
        result = QString::fromUtf16(reinterpret_cast<const char16_t*>(path));
    CloseHandle(handle);
#else
    Q_UNUSED(processId)
#endif
    return result;
}

WindowsCrashDialogBlocker::WindowsCrashDialogBlocker()
#ifdef Q_OS_WIN
    : silenceErrorMode(SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX | SEM_FAILCRITICALERRORS),
    originalErrorMode(SetErrorMode(silenceErrorMode))
#endif
{
}

WindowsCrashDialogBlocker::~WindowsCrashDialogBlocker()
{
#ifdef Q_OS_WIN
    unsigned int errorMode = SetErrorMode(originalErrorMode);
    // someone else messed with the error mode in between? Better not touch ...
    QTC_ASSERT(errorMode == silenceErrorMode, SetErrorMode(errorMode));
#endif
}


} // namespace Utils
