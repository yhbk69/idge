/*
 * Copyright (c) 2025-04-01 HeXiaotian
 * 版权声明：版权归 HeXiaotian 所有
 * Non-commercial use only. Redistribution, resale,
 * and derivative works are prohibited.
 * 仅限非商业使用，禁止再分发、转售及衍生作品
 */

/**
 * @file xml_utils.h
 * @brief XML 工具函数集
 *
 * 作用：提供 XML 消息解析相关的工具函数，主要用于 GB/T 28181 视频监控协议
 *       中的 XML 消息处理。包括字符编码转换（UTF-8 → GB2312）和
 *       特定 XML 字段（SN、CmdType）的提取。
 */

#ifndef PROJECT2_XML_UTILS_H                // 头文件保护宏：如果未定义 PROJECT2_XML_UTILS_H，则定义它，防止重复包含
#define PROJECT2_XML_UTILS_H                // 定义宏 PROJECT2_XML_UTILS_H，标识该头文件已被包含

#include <string>                           // 包含标准字符串类，用于 std::string

// ============================================================================
// utf8_to_gb2312 —— UTF-8 转 GB2312 编码
// ============================================================================
// 作用：将 UTF-8 编码的字符串转换为 GB2312 编码，用于兼容中文 Windows 环境
//       或 GB2312 编码的设备（如某些国标摄像头）。
// @param inbuf 输入 UTF-8 字符串
// @param outbuf 输出 GB2312 字符串缓冲区（调用者需确保足够大）
// @param outbufsize 输出缓冲区大小（字节数）
// @return 成功返回 0，失败返回 -1
// ============================================================================
int utf8_to_gb2312(const char *inbuf, char *outbuf, size_t outbufsize);

// ============================================================================
// extractSN —— 提取 XML 中的 Session-Name (SN) 字段
// ============================================================================
// 作用：从 XML 字符串中通过正则表达式提取 <SN> 标签内的数字内容。
//       常用于 GB/T 28181 协议中的设备目录查询响应解析。
// @param xml 包含 SN 的 XML 字符串
// @return SN 值（数字字符串），未找到则返回空字符串
// ============================================================================
std::string extractSN(const std::string &xml);

// ============================================================================
// extractCmdType —— 提取 XML 中的命令类型字段
// ============================================================================
// 作用：从 XML 消息体中通过正则表达式提取 <CmdType> 标签内的内容。
//       用于识别 GB/T 28181 消息类型（如 Catalog、DeviceInfo、Keepalive 等）。
// @param xmlBody XML 消息体字符串（C 风格字符串）
// @return 命令类型字符串，未找到则返回空字符串
// ============================================================================
std::string extractCmdType(const char *xmlBody);

#endif /* PROJECT2_XML_UTILS_H */            // 结束头文件保护宏
