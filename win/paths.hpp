// Where things are on Windows, and text conversions for the Win32 API.
#pragma once

#include <string>

namespace mh {

// The midihands package folder (the parent of externals\ holding this DLL),
// UTF-8, without a trailing separator. Empty if it cannot be found.
std::string packageFolder();

std::wstring widen(const std::string& utf8);
std::string narrow(const std::wstring& wide);
// "message (0x80070005)" for an HRESULT or Win32 error.
std::string errorText(long code);

}  // namespace mh
