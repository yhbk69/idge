/*
 * @Author: Li RF
 * @Date: 2025-01-14 19:05:32
 * @LastEditors: Li RF
 * @LastEditTime: 2025-03-16 20:30:38
 * @Description: 配置解析模块 - 用于解析命令行参数和配置文件
 * Email: 1125962926@qq.com
 * Copyright (c) 2025 Li RF, All Rights Reserved.
 */

#ifndef _PARSE_CONFIG_HPP_
#define _PARSE_CONFIG_HPP_

#include <iostream>
#include <string>
#include "SharedTypes.hpp"

/* 
====================================================
作用：配置解析类 - 负责解析命令行参数和配置文件
====================================================
*/
class ConfigParser {
    
public:
    // 输入格式类型标识（用于指定输入源的格式）
    int input_format;  

    /* 
    ====================================================
    作用：显示帮助信息
    说明：当用户运行程序时传入 --help 参数时，显示程序的使用方法
    参数：program_name - 程序名称，用于显示在帮助信息中
    返回值：无
    ====================================================
    */
    void print_help(const std::string &program_name) const;

    /* 
    ====================================================
    作用：打印当前配置信息
    说明：将解析后的配置信息输出到标准输出，用于调试和验证
    参数：config - 已解析的应用配置结构体
    返回值：无
    ====================================================
    */
    void printConfig(const AppConfig &config) const;

    /* 
    ====================================================
    作用：解析命令行参数
    说明：将用户输入的命令行参数转换为结构化的配置对象
    参数：argc - 命令行参数数量，argv - 命令行参数数组
    返回值：AppConfig - 解析后的应用配置对象
    ====================================================
    */
    AppConfig parse_arguments(int argc, char *argv[]) const;

private:
    // 私有成员（如果有需要可以添加）

};

#endif