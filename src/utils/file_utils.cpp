#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TEXT_LINE_LENGTH 1024

// ============================================================================
// load_model - 加载RKNN模型文件
// 将整个模型文件读入内存，用于RKNN推理引擎加载模型
// ============================================================================
unsigned char* load_model(const char* filename, int* model_size)
{
    // 打开模型文件（二进制只读模式）
    FILE* fp = fopen(filename, "rb");
    if (fp == NULL) {
        printf("fopen %s fail!\n", filename);
        return NULL;
    }

    // 获取文件大小：先定位到文件末尾，再获取偏移量
    fseek(fp, 0, SEEK_END);
    int model_len = ftell(fp);

    // 分配内存并读取整个模型文件
    unsigned char* model = (unsigned char*)malloc(model_len);
    fseek(fp, 0, SEEK_SET);

    if (model_len != fread(model, 1, model_len, fp)) {
        printf("fread %s fail!\n", filename);
        free(model);
        fclose(fp);
        return NULL;
    }

    *model_size = model_len;
    fclose(fp);
    return model;
}

// ============================================================================
// read_data_from_file - 从文件读取任意二进制数据
// 支持读取图像文件、模型文件等任意格式的二进制数据
// ============================================================================
int read_data_from_file(const char *path, char **out_data)
{
    FILE *fp = fopen(path, "rb");
    if(fp == NULL) {
        printf("fopen %s fail!\n", path);
        return -1;
    }

    // 获取文件大小
    fseek(fp, 0, SEEK_END);
    int file_size = ftell(fp);

    // 分配内存（+1用于存放字符串结束符）
    char *data = (char *)malloc(file_size+1);
    data[file_size] = 0;

    // 回到文件开头并读取全部数据
    fseek(fp, 0, SEEK_SET);
    if(file_size != fread(data, 1, file_size, fp)) {
        printf("fread %s fail!\n", path);
        free(data);
        fclose(fp);
        return -1;
    }

    if(fp) {
        fclose(fp);
    }

    *out_data = data;
    return file_size;
}

// ============================================================================
// write_data_to_file - 将二进制数据写入文件
// 支持将模型输出、图像数据等写入文件进行保存
// ============================================================================
int write_data_to_file(const char *path, char *data, unsigned int size)
{
    FILE *fp;

    fp = fopen(path, "w");
    if(fp == NULL) {
        printf("open error: %s\n", path);
        return -1;
    }

    // 写入数据并刷新缓冲区
    fwrite(data, 1, size, fp);
    fflush(fp);

    fclose(fp);
    return 0;
}

// ============================================================================
// count_lines - 统计文本文件行数
// 通过逐字符扫描换行符来计算总行数
// ============================================================================
int count_lines(FILE* file)
{
    int count = 0;
    char ch;

    while(!feof(file))
    {
        ch = fgetc(file);
        if(ch == '\n')
        {
            count++;
        }
    }
    count += 1;  // 最后一行可能没有换行符

    rewind(file);  // 重置文件指针到开头
    return count;
}

// ============================================================================
// read_lines_from_file - 逐行读取文本文件
// 返回字符串数组，每行作为独立的字符串元素
// 常用于读取标签文件、配置文件等文本格式数据
// ============================================================================
char** read_lines_from_file(const char* filename, int* line_count)
{
    FILE* file = fopen(filename, "r");
    if (file == NULL) {
        printf("Failed to open the file.\n");
        return NULL;
    }

    // 先统计总行数以分配数组空间
    int num_lines = count_lines(file);
    printf("num_lines=%d\n", num_lines);
    char** lines = (char**)malloc(num_lines * sizeof(char*));
    memset(lines, 0, num_lines * sizeof(char*));

    char buffer[MAX_TEXT_LINE_LENGTH];
    int line_index = 0;

    // 逐行读取，移除换行符并存储
    while (fgets(buffer, sizeof(buffer), file) != NULL) {
        buffer[strcspn(buffer, "\n")] = '\0';  // 移除换行符

        lines[line_index] = (char*)malloc(strlen(buffer) + 1);
        strcpy(lines[line_index], buffer);

        line_index++;
    }

    fclose(file);

    *line_count = num_lines;
    return lines;
}

// ============================================================================
// free_lines - 释放 read_lines_from_file 分配的内存
// 依次释放每行字符串，最后释放字符串数组本身
// ============================================================================
void free_lines(char** lines, int line_count)
{
    for (int i = 0; i < line_count; i++) {
        if (lines[i] != NULL) {
            free(lines[i]);
        }
    }
    free(lines);
}
