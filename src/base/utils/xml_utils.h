/*
 * Copyright (c) 2025-04-01 HeXiaotian
 * 版权声明：版权归 HeXiaotian 所有
 * Non-commercial use only. Redistribution, resale,
 * and derivative works are prohibited.
 * 仅限非商业使用，禁止再分发、转售及衍生作品
 */

#ifndef PROJECT2_XML_UTILS_H                // 头文件保护宏：如果未定义 PROJECT2_XML_UTILS_H，则定义它，防止重复包含
#define PROJECT2_XML_UTILS_H                // 定义宏 PROJECT2_XML_UTILS_H，标识该头文件已被包含

/**
 * @file xml_utils.h
 * @brief GB/T 28181 MANSCDP 消息用到的三个小工具：编码转换 + 两个字段抽取
 *
 * 三个函数都是正则/iconv 的薄封装，只做国标报文里最常被读的两个字段
 * （<SN> 会话序号、<CmdType> 命令类型）和 UTF-8→GB2312 的转码。
 *
 * 现状：src/biz/sip/ 的 GB28181 实现自带解析（按行切 SIP 头 + QRegularExpression），
 * 并未调用本文件，因此这里目前是**未被引用**的工具，保留自上游 demo，
 * 若日后要统一 MANSCDP 解析再启用，别把它当"国标正在用的代码"读。
 */

#include <string>                           // 包含标准字符串类，用于 std::string

/**
 * @brief UTF-8 转 GB2312（国标设备/平台常见编码）
 * @param inbuf      输入 UTF-8 字符串
 * @param outbuf     输出缓冲区，由调用方分配；转换结果以 '\0' 结尾
 * @param outbufsize outbuf 的字节数，函数会预留 1 字节写结尾 '\0'
 * @return 成功返回 0，失败返回 -1（iconv 打开或转换出错；截断也按失败处理）
 *
 * 注意 outbuf 不是"转换后的字节数"——想拿长度请自己 strlen。
 */
int utf8_to_gb2312(const char *inbuf, char *outbuf, size_t outbufsize);

/**
 * @brief 取 <SN>…</SN> 之间的数字串（会话序号，应答需原样回显）
 * @return 数字字符串；标签缺失或内容非纯数字时返回空串
 */
std::string extractSN(const std::string &xml);

/**
 * @brief 取 <CmdType>…</CmdType> 之间的内容（Keepalive/Catalog/DeviceInfo…）
 * @param xmlBody C 风格字符串（std::cmatch 要求）
 * @return 命令类型字符串，未找到返回空串
 */
std::string extractCmdType(const char *xmlBody);

#endif /* PROJECT2_XML_UTILS_H */            // 结束头文件保护宏