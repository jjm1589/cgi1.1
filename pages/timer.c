#include <stdio.h>  // 标准输入输出库
#include <string.h> // 包含各种字符串函数
#include <time.h>   // 包含用于操作日期和时间的各种函数
#include <stdlib.h> //getenv

#define BUF_MAX_SIZE (2 * 1024)
int main(int argc, char *argv[])
{
    // URL解码
    void urldecode(char *p);
    /* 请求方式 */
    char *req_method = getenv("REQUEST_METHOD");
    char studentNo[256];
    char studentName[256];
    char src[256];
    time_t tnow;
    time(&tnow); // 计算当前日历时间
    if (0 == strcmp("GET", req_method))
    {                                           /* 处理GET请求 */
        char *get_arg = getenv("QUERY_STRING"); //获取Get数据
        if (NULL == get_arg)
        {
            get_arg = (char *)"";
        }
       
        urldecode(get_arg);
        sscanf(get_arg, "studentNo=%[^&]&studentName=%[^&]", studentNo, studentName);
    }
    else if (0 == strcmp("POST", req_method))
    {                                                 /* 处理POST请求 */
        char *content_len = getenv("CONTENT_LENGTH"); //获取数据长度
        char *content_type = getenv("CONTENT_TYPE");  //获取数据类型 application/x-www-form-urlencoded、multipart/form-data、text/plain 其中：multipart/form-data是文件传输

        int len = 0;
        if (NULL != content_len)
        {
            len = atoi(content_len);
        }
        if (len > 0)
        { //获取post数据
            char dat_buf[BUF_MAX_SIZE] = {0};
            if (len > BUF_MAX_SIZE)
            {
                len = BUF_MAX_SIZE;
            }
            len = fread(dat_buf, 1, len, stdin); // 读取post请求数据
            // 转码
            urldecode(dat_buf);
            sscanf(dat_buf, "studentNo=%[^&]&studentName=%[^&]", studentNo, studentName);
        }
    }
    printf("Content-Length: 100\r\nContent-Type: text/html; charset=utf-8\r\n\r\n学号:%s 姓名：%s 时间：%s", studentNo, studentName, ctime(&tnow));
    return 0;
}

void urldecode(char *p)

{

    int i = 0;

    while (*(p + i))

    {

        if ((*p = *(p + i)) == '%')

        {

            *p = *(p + i + 1) >= 'A' ? ((*(p + i + 1) & 0XDF) - 'A') + 10 : (*(p + i + 1) - '0');

            *p = (*p) * 16;

            *p += *(p + i + 2) >= 'A' ? ((*(p + i + 2) & 0XDF) - 'A') + 10 : (*(p + i + 2) - '0');

            i += 2;
        }

        else if (*(p + i) == '+')

        {

            *p = ' ';
        }

        p++;
    }

    *p = '\0';
}