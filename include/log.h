#pragma once
#include <string>

namespace ctp {

void log(const char* fmt, ...);

// CTP 错误串是 GBK：Windows 走 MultiByteToWideChar，Linux 走 iconv(GB18030)
std::string gbk2utf8(const char* s);

} // namespace ctp

