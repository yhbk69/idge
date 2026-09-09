/**
 * @file xml_utils.cpp
 * @brief XML 工具函数实现
 *
 * 作用：实现 XML 消息解析相关的工具函数，包括 UTF-8 到 GB2312 的编码转换，
 *       以及通过正则表达式从 XML 中提取 SN 和 CmdType 字段。
 */

// 版权声明：作者 HeXiaotian，日期 2025-04-01，仅限非商业使用，禁止再分发、转售和衍生作品
/*
 * Copyright (c) 2025-04-01 HeXiaotian
 * Non-commercial use only. Redistribution, resale,
 * and derivative works are prohibited.
 */

// 包含自定义的 XML 工具函数声明头文件
#include "xml_utils.h"
// 包含字符编码转换库（iconv），用于 UTF-8 与 GB2312 之间的转换
#include <iconv.h>
// 包含 C 风格字符串操作函数（如 strlen, memset 等）
#include <cstring>
// 包含正则表达式库，用于解析 XML 中的特定标签内容
#include <regex>

// ============================================================================
// utf8_to_gb2312 —— UTF-8 转 GB2312 编码转换
// ============================================================================
// 作用：使用 POSIX iconv 库将 UTF-8 编码字符串转换为 GB2312 编码。
//       主要用于兼容国标摄像头等仅支持 GB2312 编码的设备。
// @param inbuf 输入字符串（UTF-8 编码）
// @param outbuf 输出缓冲区（存放转换后的 GB2312 字符串）
// @param outbufsize 输出缓冲区的大小（字节数）
// @return 0 表示成功，-1 表示失败
// ============================================================================
int utf8_to_gb2312(const char *inbuf, char *outbuf, size_t outbufsize)
{
    // 打开转换描述符：从 UTF-8 转换到 GB2312
    iconv_t cd = iconv_open("GB2312", "UTF-8");
    // 若打开失败，返回 -1
    if (cd == (iconv_t)-1)
        return -1;

    // 将输入和输出指针转为 char* 类型（iconv 要求非 const 指针）
    char *in = (char *)inbuf;
    char *out = outbuf;
    // 计算输入字节数（不包括 '\0'）
    size_t inbytesleft = strlen(inbuf);
    // 输出缓冲区剩余字节数（保留一个字节用于存放字符串结束符）
    size_t outbytesleft = outbufsize - 1;
    // 将输出缓冲区清零
    memset(outbuf, 0, outbufsize);

    // 执行编码转换
    if (iconv(cd, &in, &inbytesleft, &out, &outbytesleft) == (size_t)-1)
    {
        // 转换失败，关闭转换描述符并返回 -1
        iconv_close(cd);
        return -1;
    }
    // 转换成功，关闭转换描述符
    iconv_close(cd);
    // 返回 0 表示成功
    return 0;
}

// ============================================================================
// extractSN —— 从 XML 中提取 SN（Session-Name）字段
// ============================================================================
// 作用：使用正则表达式匹配 <SN> 和 </SN> 之间的数字内容。
//       常用于 GB/T 28181 协议中设备目录查询响应的解析。
// @param xml 输入的 XML 字符串
// @return SN 对应的数字字符串，若未找到则返回空字符串
// ============================================================================
std::string extractSN(const std::string &xml)
{
    // 正则表达式：匹配 <SN> 和 </SN> 之间的数字（一个或多个数字）
    std::regex sn_regex("<SN>(\\d+)</SN>");
    // 用于存储匹配结果的 smatch 对象
    std::smatch match;
    // 在 xml 中搜索正则表达式
    if (std::regex_search(xml, match, sn_regex))
        // 如果匹配成功，返回第一个捕获组（即数字部分）的字符串
        return match[1].str();
    // 未找到匹配，返回空字符串
    return "";
}

// ============================================================================
// extractCmdType —— 从 XML 中提取 CmdType（命令类型）字段
// ============================================================================
// 作用：使用正则表达式匹配 <CmdType> 和 </CmdType> 之间的内容。
//       用于识别 GB/T 28181 消息类型（如 Catalog、DeviceInfo、Keepalive 等）。
// @param xmlBody 输入的 XML 字符串（C 风格字符串）
// @return 命令类型字符串，若未找到则返回空字符串
// ============================================================================
std::string extractCmdType(const char *xmlBody)
{
    // 正则表达式：匹配 <CmdType> 和 </CmdType> 之间的任意内容（非贪婪模式 .*?）
    std::regex pattern("<CmdType>(.*?)</CmdType>");
    // 用于存储匹配结果的 cmatch 对象（针对 C 风格字符串）
    std::cmatch matches;
    // 在 xmlBody 中搜索正则表达式，并要求匹配成功且至少有一个捕获组
    if (std::regex_search(xmlBody, matches, pattern) && matches.size() > 1)
        // 返回第一个捕获组的内容
        return matches[1].str();
    // 未找到匹配，返回空字符串
    return "";
}
