// log_test.cpp —— 验证 GBK→UTF-8（Windows 走 MultiByteToWideChar，Linux 走 iconv）
#include <cstdio>
#include <cstring>
#include "log.h"

int main() {
    // GBK "错误" = B4 ED CE F3  →  UTF-8 6 字节 E9 94 99 E8 AF AF
    const char gbk[] = "\xb4\xed\xce\xf3";
    std::string u = ctp::gbk2utf8(gbk);
    bool ok1 = (u.size() == 6 && (unsigned char)u[0] == 0xE9);

    // 纯 ASCII 应原样返回
    std::string a = ctp::gbk2utf8("CTP:login fail");
    bool ok2 = (a == "CTP:login fail");

    // 空指针 / 空串
    bool ok3 = ctp::gbk2utf8(nullptr).empty() && ctp::gbk2utf8("").empty();

    printf("[%s] GBK \"错误\" -> %s (%zu bytes)\n", ok1 ? "PASS" : "FAIL", u.c_str(), u.size());
    printf("[%s] ASCII 原样返回\n", ok2 ? "PASS" : "FAIL");
    printf("[%s] 空指针/空串\n", ok3 ? "PASS" : "FAIL");
    return (ok1 && ok2 && ok3) ? 0 : 1;
}

