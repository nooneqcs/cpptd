#include "log.h"
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <ctime>
#include <chrono>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <iconv.h>
#endif

namespace ctp {

void log(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count() % 1000;
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    fprintf(stdout, "%02d:%02d:%02d.%03lld %s\n",
        tmv.tm_hour, tmv.tm_min, tmv.tm_sec, (long long)ms, buf);
    fflush(stdout);
}

std::string gbk2utf8(const char* s) {
    if (!s || !*s) return "";
#ifdef _WIN32
    int n = MultiByteToWideChar(936, 0, s, -1, nullptr, 0);
    if (n <= 0) return std::string(s);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(936, 0, s, -1, &w[0], n);
    int m = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (m <= 0) return std::string(s);
    std::string u(m, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &u[0], m, nullptr, nullptr);
    if (!u.empty() && u.back() == '\0') u.pop_back();
    return u;
#else
    iconv_t cd = iconv_open("UTF-8", "GB18030");   // GB18030 ⊇ GBK
    if (cd == (iconv_t)-1) return std::string(s);
    size_t inlen = std::strlen(s);
    std::string out(inlen * 4 + 4, '\0');
    char* in = const_cast<char*>(s);
    char* outp = &out[0];
    size_t outlen = out.size();
    size_t r = iconv(cd, &in, &inlen, &outp, &outlen);
    iconv_close(cd);
    if (r == (size_t)-1) return std::string(s);
    out.resize(out.size() - outlen);
    return out;
#endif
}

} // namespace ctp

