#ifndef _RKNN_MODEL_ZOO_FILE_UTILS_H_
#define _RKNN_MODEL_ZOO_FILE_UTILS_H_

// ============================================================================
// 文件工具函数头文件
// 提供文件读写操作，用于RKNN模型文件加载、图像数据读取等
// ============================================================================

/**
 * @brief 从文件中读取二进制数据
 * 
 * @param path [in] 文件路径
 * @param out_data [out] 读取的数据指针（调用者需负责释放内存）
 * @return int -1: 错误; > 0: 读取的数据大小（字节数）
 */
int read_data_from_file(const char *path, char **out_data);

/**
 * @brief 将二进制数据写入文件
 * 
 * @param path [in] 文件路径
 * @param data [in] 要写入的数据
 * @param size [in] 数据大小（字节数）
 * @return int 0: 成功; -1: 错误
 */
int write_data_to_file(const char *path, char *data, unsigned int size);

/**
 * @brief 从文本文件中读取所有行
 * 
 * @param path [in] 文件路径
 * @param line_count [out] 文件行数
 * @return char** 字符串数组，使用完毕后需调用 free_lines() 释放内存
 */
char** read_lines_from_file(const char* path, int* line_count);

/**
 * @brief 释放 read_lines_from_file 返回的字符串数组
 * 
 * @param lines [in] 字符串数组
 * @param line_count [in] 行数
 */
void free_lines(char** lines, int line_count);

#endif //_RKNN_MODEL_ZOO_FILE_UTILS_H_
