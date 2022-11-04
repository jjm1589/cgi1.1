#include <iostream> // 提供一个方便的命令行输入输出试验环境
#include "httpd.h" // 自定义文件引入
#include <stdio.h> // 标准输入输出库
#include <stdlib.h> // 这包括有关内存分配的函数
#include <string.h> // 包含各种字符串函数
#include <unistd.h> // 相当于windows操作系统的"windows.h"，是操作系统为用户提供的统一API接口
#include <netinet/in.h> // 包含Internet域地址所需的常量和结构
#include <sys/socket.h> // 创建socket所需的
#include <pthread.h> // 线程相关的
#include <sys/types.h> // 包含许多应在适当时使用的基本派生类型
#include <sys/stat.h> // 是unix/linux系统定义文件状态所在的伪标准头文件。
#include <assert.h> // 断言，终端程序使用
#include <dirent.h> // 主要用于文件系统的目录读取操作
#include <fcntl.h> // 包含的相关函数有 open，fcntl，shutdown，unlink，fclose等
#include <signal.h> // 信号量
#include <sys/wait.h> // 进程控制
#include <netdb.h> // 定义了与网络有关的结构，变量类型，宏，函数。
#include <arpa/inet.h> // 定义sockaddr_in结构体

using namespace std;

// 创建线程池
ThreadPool* pool = threadPoolCreate(3, 10, 100);

#define HEADER_SIZE   10240L /* 请求头行的最大尺寸 */
#define CGI_POST      10240L /* 读取POST数据到CGI的缓存区大小*/
#define CGI_BUFFER    10240L /* 读取CGI输出的缓冲区大小 */
#define FLAT_BUFFER   10240L /* 读取静态文件的缓冲区大小 */


#define ENABLE_CGI      1    /* 是否启用CGI (also POST and HEAD) */
#define ENABLE_DEFAULTS 1    /* 是否启用默认的index文件 (.php, .pl, .html) */
/*
	例如，当浏览器输入127.0.0.1:8080时，默认index的执行限制
*/
#define INDEX_DEFAULTS  { "index.htm", "index.html", 0}
#define INDEX_EXECUTES  {         0,            0,  -1}
// 当前服务器名称
#define VERSION_STRING  "cgi/1.1"
/*
* 初始化存放请求容器的大小
*/
#define INIT_VEC_SIZE 1024
/*
 * 可变的 vector
 */
typedef struct {
	void ** buffer;
	unsigned int size;
	unsigned int alloc_size;
} vector_t;
/*
	定义socket请求的结构体
	传入的请求socket数据
*/
struct socket_request {
	int                fd;       /* Socket 本身 */
	socklen_t          addr_len; /* 地址类型的长度 */
	struct sockaddr_in address;  /* 远程 address */
	pthread_t          thread;   /* 处理线程程序 */
};
/*
 * CGI 处理的数据的结构体
 */
struct cgi_wait {
	int                fd;       /* 读 */
	int                fd2;      /* 写 */
	int                pid;      /* 进程 ID */
};
// 服务器的socket
int serversock;
// 端口号
int default_port;
// 服务器默认静态文件存放的目录
string default_root;

// 最后一个断开连接的socket指针，需要free掉它
// void * _last_unaccepted;

// 断开socket之后退出
void handleShutdown(int sig) {
	printf("\n[info] Shutting down.\n");
	/*
	 * Shutdown the socket.
	 */
	shutdown(serversock, SHUT_RDWR);
	close(serversock);
	/*
	 * 释放线程数据块，用于下一个连接。
	 */
	// free(_last_unaccepted);
	exit(sig);
}

/*
* 初始化，动态分配内存
*/
vector_t * alloc_vector(void) {
	vector_t* v = (vector_t *) malloc(sizeof(vector_t));
	v->buffer = (void **) malloc(INIT_VEC_SIZE * sizeof(void *));
	v->size = 0;
	v->alloc_size = INIT_VEC_SIZE;

	return v;
}
/*
* 释放内存
*/
void free_vector(vector_t* v) {
	free(v->buffer);
	free(v);
}

/**
 * 向容器（vector）添加item，
 * 大小不够，进行扩容，重新分配
*/
void vector_append(vector_t * v, void * item) {
	if(v->size == v->alloc_size) {
		v->alloc_size = v->alloc_size * 2; // 扩容
		v->buffer = (void **) realloc(v->buffer, v->alloc_size * sizeof(void *));
	}

	v->buffer[v->size] = item;
	v->size++;
}

/**
 * 返回容器（vector）内指定的内容
*/
void * vector_at(vector_t * v, unsigned int idx) {
	return idx >= v->size ? NULL : v->buffer[idx];
}

/*
 * 删除vector
 * 释放掉它的内容
 */
void delete_vector(vector_t * vector) {
	unsigned int i = 0;
	for (i = 0; i < vector->size; ++i) {
		free(vector_at(vector, i));
	}
	free_vector(vector);
}

/*
 * 具有特定状态的普通纯文本响应。
 * 大多数用于失败的请求
 */
void generic_response(FILE * socket_stream, char * status, char * message) {
	fprintf(socket_stream,
			"HTTP/1.1 %s\r\n"
			"Server: " VERSION_STRING "\r\n"
			"Content-Type: text/plain\r\n"
			"Content-Length: %zu\r\n"
			"\r\n"
			"%s\r\n", status, strlen(message), message);
}

/*
 * 等待 CGI 线程完成
 * 关闭它的管道.
 */
void *wait_pid(void * onwhat) {
	struct cgi_wait * cgi_w = (struct cgi_wait*)onwhat;
	int status;
	/*
	 * 等待进程结束
	 */
	waitpid(cgi_w->pid, &status, 0);
	/*
	 * Close the respective pipe
	 */
	close(cgi_w->fd); // 读
	close(cgi_w->fd2); // 写
	/*
	 * 释放掉已经发送了的数据
	 */
	// free(onwhat);
	return NULL;
}

/*
 * 处理一个连接请求
 */
void *handleRequest(void *socket) {
	struct socket_request * request = (struct socket_request *)socket;
	/*
	 * 将socket转换为标准的文件描述符
	 */
	FILE *socket_stream = NULL;
	// 创建一个指向现有系统文件描述符的新流。
	socket_stream = fdopen(request->fd, "r+");
	if (!socket_stream) {
		fprintf(stderr,"Ran out of a file descriptors, can not respond to request.\n");
		goto _disconnect;
	};
	/*
	 * 读请求，直到客户端关闭连接
	 */
	while (1) {
		vector_t * queue = alloc_vector();
		cerr << "开始读客户端发来的请求" << endl;
		char buf[HEADER_SIZE];
		while (!feof(socket_stream)) { // 文件结束：返回非0值；文件未结束：返回0值
			/*
			 * 当客户端还未断开连接时，将请求头读入队列中
			 */
			/*从 socket_stream 流中读取 HEADER_SIZE - 2 个字符存储到字符数组 buf 所指向的内存空间。
			 它的返回值是一个指针，指向字符串中第一个字符的地址。
			 */
			char * in = fgets( buf, HEADER_SIZE - 2, socket_stream );  
			if (!in) { // EOF
				break;
			}
			if (!strcmp(in, "\r\n") || !strcmp(in,"\n")) { // strcmp相等返回0
				/*
				 * 到达头部末尾 .
				 */
				break;
			}
			if (!strstr(in, "\n")) {
				/*
				 * 超过请求行的大小
				 */
				generic_response(socket_stream, (char *)"400 Bad Request", (char *)"Bad request: Request line was too long.");
				delete_vector(queue); // 释放掉
				goto _disconnect;
			}
			/*
			 * 将请求行存储到容器队列中
			 */
			// 在C++中void类型不能用来初始化char 类型的实体，但是C语言不强制转换void*不会报错。所以需要进行强制类型转换
			char * request_line = (char *) malloc((strlen(buf)+1) * sizeof(char));
			strcpy(request_line, buf);
			vector_append(queue, (void*)request_line); // 将请求行存储到vector中
		}

		if (feof(socket_stream)) { // 读取结束
			/*
			 * 读取结束，说明客户端关闭连接
			 */
			delete_vector(queue);
			break;
		}

		/*
		 * 请求变量设置
		 */
		char * filename          = NULL; /* Filename as received (ie, /index.html) */
		char * querystring       = NULL; /* Query string, URL encoded */
		int request_type         = 0;    /* Request type, 0=GET, 1=POST, 2=HEAD ... */
		char * _filename         = NULL; /* Filename relative to server (ie, pages/index.html) */
		char * ext               = NULL; /* Extension for requested file */
		char * host              = NULL; /* Hostname for request, if supplied. */
		char * http_version      = NULL; /* HTTP version used in request */
		unsigned long c_length   = 0L;   /* Content-Length, usually for POST */
		char * c_type            = NULL; /* Content-Type, usually for POST */
		char * c_cookie          = NULL; /* HTTP_COOKIE */
		char * c_uagent          = NULL; /* User-Agent, for CGI */
		char * c_referer         = NULL; /* Referer, for CGI */

		/*
		 * 处理请求头
		 */
		unsigned int i = 0;
		for (i = 0; i < queue->size; ++i) {
			char * str = (char*)(vector_at(queue,i));
			/*
			 * 在头部找冒号，除了第一行请求信息，都是key:value的形式
			 */
			// 返回值为char * 类型（ 返回指向 str1 中第一次出现的 str2 的指针）；如果 str2 不是 str1 的一部分，则返回空指针。
			char * colon = strstr(str,": "); 
			if (!colon) {
				if (i > 0) {
					/*
					 * Request string outside of first entry.
					 */
					generic_response(socket_stream, (char *)"400 Bad Request", (char *)"Bad request: A header line was missing colon.");
					delete_vector(queue);
					goto _disconnect;
				}
				/*
				 * 请求方法类型
				 */
				int r_type_width = 0;
				switch (str[0]) {
					case 'G':
						if (strstr(str, "GET ") == str) {
							// printf("get请求：%s", str);
							r_type_width = 4;
							request_type = 1;
						} else {
							goto _unsupported;
						}
						break;
#if ENABLE_CGI
					case 'P':
						if (strstr(str, "POST ") == str) {
							/*
							 * POST: 发送数据给CGI
							 */
							// printf("post请求：%s\n", str);
							r_type_width = 5;
							request_type = 2;
						} else {
							goto _unsupported;
						}
						break;
					case 'H':
						if (strstr(str, "HEAD ") == str) {
							/*
							 * HEAD: 只检索头部
							 */
							r_type_width = 5;
							request_type = 3;
						} else {
							goto _unsupported;
						}
						break;
#endif
					default:
						/*
						 * 其他未支持的方法
						 */
						goto _unsupported;
						break;
				}

				filename = str + r_type_width; // 定位到请求路径，例如：GET / HTTP/1.1，定位到的是'/'
				// printf("filename:%s", filename);
				if (filename[0] == ' ' || filename[0] == '\r' || filename[0] == '\n') {
					/*
					 * 请求缺少文件名，或者请求格式有误的情况
					 */
					generic_response(socket_stream, (char *)"400 Bad Request", (char *)"Bad request: No filename.");
					delete_vector(queue);
					goto _disconnect;
				}

				/*
				 * 获取HTTP版本号
				 */
				http_version = strstr(filename, "HTTP/");
				if (!http_version) {
					/*
					 * 在当前请求中没有http版本号
					 */
					generic_response(socket_stream, (char *)"400 Bad Request", (char *)"Bad request: No HTTP version supplied.");
					delete_vector(queue);
					goto _disconnect;
				}
				http_version[-1] = '\0';
				
				querystring = strstr(filename, "?"); // 用于获取get请求的参数
				if (querystring) {
					querystring++;
					querystring[-1] = '\0';
				}
			} else {
				/**
				 * 在正常的请求中，第一行一般是：GET / HTTP/1.1这样的形式
				 * 如果第一行存在冒号，可以判断是非法请求
				*/
				if (i == 0) {
					generic_response(socket_stream, (char *)"400 Bad Request", (char *)"Bad request: First line was not a request.");
					delete_vector(queue);
					goto _disconnect;
				}

				/*
				 * 分离头部，取出key:value信息中的value
				 */
				colon[0] = '\0';
				colon += 2;
				char * eol = strstr(colon,"\r");
				if (eol) {
					eol[0] = '\0';
					eol[1] = '\0';
				} else {
					eol = strstr(colon,"\n");
					if (eol) {
						eol[0] = '\0';
					}
				}

				/*
				 * 处理 header
				 * str: colon
				 */
				if (!strcmp(str, "Host")) { // 相等返回0
					/*
					 * Host: The hostname of the (virtual) host the request was for.
					 */
					host = colon;
				} else if (!strcmp(str, "Content-Length")) {
					/*
					 * Content-Length: Length of message (after these headers) in bytes.
					 */
					c_length = atol(colon);
				} else if (!strcmp(str, "Content-Type")) {
					/*
					 * Content-Type: MIME-type of the message.
					 */
					c_type = colon;
				} else if (!strcmp(str, "Cookie")) {
					/*
					 * Cookie: CGI cookies
					 */
					c_cookie = colon;
				} else if (!strcmp(str, "User-Agent")) {
					/*
					 * Client user-agent string
					 */
					c_uagent = colon;
				} else if (!strcmp(str, "Referer")) {
					/*
					 * Referer page
					 */
					c_referer = colon;
				}
			}
		}

		/*
		 * 所有请求信息都已读取
		 */
		if (!request_type) {
_unsupported:
			/*
			 * 服务器不能理解的请求返回501
			 */
			generic_response(socket_stream, (char *)"501 Not Implemented", (char *)"Not implemented: The request type sent is not understood by the server.");
			delete_vector(queue);
			goto _disconnect;
		}

		if (!filename || strstr(filename, "'") || strstr(filename," ") ||
			(querystring && strstr(querystring," "))) {
			/*
			 * 没有指定文件名，或者接收一个无效或格式错误的请求
			 */
			generic_response(socket_stream, (char *)"400 Bad Request", (char *)"Bad request: No filename provided.");
			delete_vector(queue);
			goto _disconnect;
		}

		/*
		 * 在请求行中获取到了重要的信息，即_filename: 默认静态文件文件夹下的目录或文件
		 */
		_filename = (char *) calloc(sizeof(char) * (strlen(default_root.c_str()) + strlen(filename) + 2), 1);
		strcat(_filename, default_root.c_str());
		strcat(_filename, filename);
		
		/*
		 * 对相对路径的跳转进行限制
		 */
		if (strstr(_filename, "/../") || (strstr(_filename, "/..") == _filename + strlen(_filename) - 3)) {
			generic_response(socket_stream, (char *)"400 Bad Request", (char *)"Bad request");
			free(_filename);
			delete_vector(queue);
			goto _disconnect;
		}

		/*
		 * ext: 文件后缀，可为NULL
		 */
		ext = filename + 1;
		while (strstr(ext+1,".")) {
			ext = strstr(ext+1,".");
		}
		if (ext == filename + 1) {
			/*
			 * 要么我们没找到点，
			 * 或者这个点在前面。
			 * 如果圆点在前面，它不是扩展名，
			 * 而是一个没有扩展名的隐藏文件。
			 */
			ext = NULL;
		}

		/*
		 * 检查是否是目录
		 */
		struct stat stats;
		if (stat(_filename, &stats) == 0 && S_ISDIR(stats.st_mode)) {
			// printf("检查是否是目录_filename:%s", _filename);
			if (_filename[strlen(_filename)-1] != '/') {
				/*
				 * 请求一个没有/结尾的目录
				 * 抛出 'moved permanently' 并且重定向到客户端
				 */
				fprintf(socket_stream, "HTTP/1.1 301 Moved Permanently\r\n");
				fprintf(socket_stream, "Server: " VERSION_STRING "\r\n");
				fprintf(socket_stream, "Location: %s/\r\n", filename);
				fprintf(socket_stream, "Content-Length: 0\r\n\r\n");
			} else {

#if ENABLE_DEFAULTS
				/*
				 *	检查默认的index文件
				 */
				struct stat extra_stats;

				char* index_html = new char[strlen(_filename) + 30];

				/*
				 * 默认执行的index文件
				 */
				const char *       index_defaults[] = INDEX_DEFAULTS;
				int index_executes[] = INDEX_EXECUTES;
				unsigned int index = 0;

				while (index_defaults[index] != (char *)0) {
					index_html[0] = '\0';
					strcat(index_html, _filename);
					strcat(index_html, index_defaults[index]);
					if ((stat(index_html, &extra_stats) == 0) && ((extra_stats.st_mode & S_IXOTH) == (unsigned)index_executes[index])) {
						/*
						 * 这个index存在，使用它代替目录列表。
						 */
						_filename = (char *) realloc(_filename, strlen(index_html)+1);
						stats = extra_stats;
						memcpy(_filename, index_html, strlen(index_html)+1);
						ext = _filename;
						while (strstr(ext+1,".")) { // 这个是为了防止文件名称里有'.'
							ext = strstr(ext+1,".");
						}
						goto _use_file;
					}
					++index;
				}
#endif

				/*
				 * 如果请求的目录，没有默认的index文件，那么就会列出当前目录下所有的文件
				 */
				struct dirent **files = {0};
				int filecount = -1;
				/**扫描目录
				 * 扫描_filename目录下(不包括子目录)满足filter过滤模式的文件，返回的结果是compare函数经过排序的，并保存在files中。
				*/
				filecount = scandir(_filename, &files, 0, alphasort);

				/*
				 * 打印出当前目录下的所有文件
				 */
				fprintf(socket_stream, "HTTP/1.1 200 OK\r\n");
				fprintf(socket_stream, "Server: " VERSION_STRING "\r\n");
				fprintf(socket_stream, "Content-Type: text/html\r\n");

				/*
				 * 为HTML分配一些内存
				 */
				char * listing = (char *) malloc(1024);
				listing[0] = '\0';
				strcat(listing, "<!doctype html><html><head><meta charset='UTF-8'><title>Directory Listing</title></head><body>");
				int i = 0;
				for (i = 0; i < filecount; ++i) {
					/*
					 * 获取当前目录下的完整的文件名称，以便于使用stat来判断是否还是目录
					 */
					char* _fullname = new char[strlen(_filename) + 1 + strlen(files[i]->d_name) + 1];
					sprintf(_fullname, "%s/%s", _filename, files[i]->d_name);
					if (stat(_fullname, &stats) == 0 && S_ISDIR(stats.st_mode)) {
						/*
						 * 忽略掉当前目录
						 */
						free(files[i]);
						continue;
					}

					/*
					 * 给当前目录下的文件添加跳转链接
					 */
					char* _file = new char[2 * strlen(files[i]->d_name) + 64];
					sprintf(_file, "<a href=\"%s\">%s</a><br>\n", files[i]->d_name, files[i]->d_name);
					listing = (char *) realloc(listing, strlen(listing) + strlen(_file) + 1);
					strcat(listing, _file);
					free(files[i]);
				}
				free(files);

				listing = (char *) realloc(listing, strlen(listing) + 64);
				strcat(listing,"</body></html>");

				/*
				 * 发送listing.
				 */
				fprintf(socket_stream, "Content-Length: %zu\r\n", (sizeof(char) * strlen(listing)));
				fprintf(socket_stream, "\r\n");
				fprintf(socket_stream, "%s", listing);
				free(listing); // 释放内存
			}
		} else {
_use_file:
			;
			/*
			 * 打开请求的文件
			 */
			FILE * content = fopen(_filename, "rb");
			if (!content) {
				/*
				 * 打不开文件 - 404. (Perhaps 403)
				 */
				string s = default_root;
				s += "/404.htm";
				content = fopen(s.c_str(), "rb");

				if (!content) {
					/*
					 * 如果没有显示404的页面，则返回一个默认的404页面
					 */
					generic_response(socket_stream, (char *)"404 File Not Found", (char *)"The requested file could not be found.");
					goto _next;
				}

				/*
				 * 将内部文件名替换为404页面
				 */
				fprintf(socket_stream, "HTTP/1.1 404 File Not Found\r\n");
				_filename = (char *) realloc(_filename, strlen(s.c_str()) + 1);
				_filename[0] = '\0';
				strcat(_filename, s.c_str());
				ext = strstr(_filename, ".");
			} else {
#if ENABLE_CGI
				if (stats.st_mode & S_IXOTH) {
					/*
					 * CGI可执行
					 * 关闭文件
					 */
					fclose(content);
					/*
					 * 准备管道
					 */
					int cgi_pipe_r[2]; // 读
					int cgi_pipe_w[2]; // 写
					if (pipe(cgi_pipe_r) < 0) {  // 利用管道进行进程间通信，成功：0；失败：-1，
						fprintf(stderr, "Failed to create read pipe!\n");
					}
					if (pipe(cgi_pipe_w) < 0) {
						fprintf(stderr, "Failed to create write pipe!\n");
					}

					/*
					 * Fork.
					 * 返回值大于0 -> 父进程在运行
					 * 返回值等于0 -> 子进程在运行
					 * 返回值小于0 -> 函数系统调用出错
					 */
					pid_t _pid = 0;
					_pid = fork(); 
					printf("=======================================_pid:%d\n", _pid);
					if (_pid == 0) {
						/*
						 * 设置管道
						   STDIN_FILENO：接收键盘的输入
						   STDOUT_FILENO：向屏幕输出
						 */
						dup2(cgi_pipe_r[0],STDIN_FILENO);
						dup2(cgi_pipe_w[1],STDOUT_FILENO);
						
						fprintf(stdout, "Expires: -1\r\n");

						/*
						 * 在正确的目录下操作
						 */
						char * dir = _filename;
						while (strstr(_filename,"/")) {
							_filename = strstr(_filename,"/") + 1;
						}
						_filename[-1] = '\0';
						char docroot[1024];
						/**
						 * 获取当前工作目录
						*/
						getcwd(docroot, 1023);
						string temp = "/";
						temp += default_root;
						strcat(docroot, temp.c_str());
						// 将当前的工作目录改变成dir目录。
						chdir(dir);
						/*
						 * 设置 CGI 环境变量
						 * CONTENT_LENGTH    : The length of the query information. It's available only for POST requests.
						 * CONTENT_TYPE      : POST encoding type
						 * DOCUMENT_ROOT     : the root directory
						 * GATEWAY_INTERFACE : The CGI version (CGI/1.1)
						 * HTTP_COOKIE       : Cookies provided by client
						 * HTTP_HOST         : Same as above
						 * HTTP_REFERER      : Referer page.
						 * HTTP_USER_AGENT   : Browser user agent
						 * PATH_TRANSLATED   : On-disk file path
						 * QUERY_STRING      : /file.ext?this_stuff&here
						 * REDIRECT_STATUS   : HTTP status of CGI redirection (PHP)
						 * REMOTE_ADDR       : IP of remote user
						 * REMOTE_HOST       : Hostname of remote user (reverse DNS)
						 * REQUEST_METHOD    : GET, POST, HEAD, etc.
						 * SCRIPT_FILENAME   : Same as PATH_TRANSLATED (PHP, primarily)
						 * SCRIPT_NAME       : Request file path
						 * SERVER_NAME       : Our hostname or Host: header
						 * SERVER_PORT       : TCP host port
						 * SERVER_PROTOCOL   : The HTTP version of the request
						 * SERVER_SOFTWARE   : Our application name and version
						 */
						/**
						 * setenv (const char *__name, const char *__value, int __replace)
						 * 参数：name为环境变量名称字符串。 value则为变量内容，overwrite用来决定是否要改变已存在的环境变量。
						 * 如果overwrite不为0，而该环境变量原已有内容，则原内容会被改为参数value所指的变量内容。
						 * 如果overwrite为0，且该环境变量已有内容，则参数value会被忽略。
						 * 返回值：成功返回0，错误返回-1.
						*/
						setenv("SERVER_SOFTWARE", VERSION_STRING, 1);

						if (!host) {
							char hostname[1024];
							hostname[1023]='\0';
							gethostname(hostname, 1023);
							setenv("SERVER_NAME", hostname, 1);
							setenv("HTTP_HOST",   hostname, 1);
						} else {
							setenv("SERVER_NAME", host, 1);
							setenv("HTTP_HOST",   host, 1);
						}
						setenv("DOCUMENT_ROOT", docroot, 1);
						setenv("GATEWAY_INTERFACE", "CGI/1.1", 1);
						setenv("SERVER_PROTOCOL", http_version, 1);
						char port_string[20];
						sprintf(port_string, "%d", default_port);
						setenv("SERVER_PORT", port_string, 1);
						if (request_type == 1) {
							setenv("REQUEST_METHOD", "GET", 1);
						} else if (request_type == 2) {
							setenv("REQUEST_METHOD", "POST", 1); 
						} else if (request_type == 3) {
							setenv("REQUEST_METHOD", "HEAD", 1);
						}
						if (querystring) {
							if (strlen(querystring)) {
								setenv("QUERY_STRING", querystring, 1);
							} else {
								setenv("QUERY_STRING", "", 1);
							}
						}
						char* fullpath = new char[1024 + strlen(_filename)];
						getcwd(fullpath, 1023);
						strcat(fullpath, "/");
						strcat(fullpath, _filename);
						
						setenv("PATH_TRANSLATED", fullpath, 1);
						setenv("SCRIPT_NAME", filename, 1);
						setenv("SCRIPT_FILENAME", fullpath, 1);
						setenv("REDIRECT_STATUS", "200", 1);
						char c_lengths[100];
						c_lengths[0] = '\0';
						sprintf(c_lengths, "%lu", c_length);
						setenv("CONTENT_LENGTH", c_lengths, 1);
						if (c_type) {
							setenv("CONTENT_TYPE", c_type, 1);
						}
						struct hostent * client;
						client = gethostbyaddr((const char *)&request->address.sin_addr.s_addr,
								sizeof(request->address.sin_addr.s_addr), AF_INET);
						if (client != NULL) {
							setenv("REMOTE_HOST", client->h_name, 1);
						}
						setenv("REMOTE_ADDR", inet_ntoa(request->address.sin_addr), 1);
						if (c_cookie) {
							setenv("HTTP_COOKIE", c_cookie, 1);
						}
						if (c_uagent) {
							setenv("HTTP_USER_AGENT", c_uagent, 1);
						}
						if (c_referer) {
							setenv("HTTP_REFERER", c_referer, 1);
						}

						/*
						 * 执行
						 */
						char executable[1024];
						executable[0] = '\0';
						sprintf(executable, "./%s", _filename);
						// printf("_filename@@@@：%s\n", _filename);
						execlp(executable, executable, (char *)NULL);

						/*
						 * cgi执行失败
						 */
						fprintf(stderr,"[warn] Failed to execute CGI script: %s?%s.\n", fullpath, querystring);

						/*
						 * 清理、释放原始进程
						 */
						delete_vector(queue);
						free(dir);
						// free(_last_unaccepted);
						// pthread_detach(request->thread);
						// free(request);

						sleep(1);
						return NULL;
					}

					/*
					 * 服务器线程
					 * 当CGI应用程序完成执行时，创建一个线程来关闭管道。
					 */
					struct cgi_wait * cgi_w = (cgi_wait *) malloc(sizeof(struct cgi_wait));
					cgi_w->pid = _pid;
					cgi_w->fd  = cgi_pipe_w[1];
					cgi_w->fd2 = cgi_pipe_r[0];
					pthread_t _waitthread;
					/**
					 * 创建线程
					 * 参数1：传递一个 pthread_t 类型的指针变量，也可以直接传递某个 pthread_t 类型变量的地址。
					 * 参数2：用于手动设置新建线程的属性。
					 * 参数3：以函数指针的方式指明新建线程需要执行的函数，该函数的参数最多有 1 个（可以省略不写），
					 * 		  形参和返回值的类型都必须为 void* 类型。
					 * 参数4：指定传递给 wait_pid 函数的实参，当不需要传递任何数据时，将 arg 赋值为 NULL 即可。
					*/
					pthread_create(&_waitthread, NULL, wait_pid, (void *)(cgi_w));

					/*
					 * 打开管道的一端。
					 * 我们映射cgi_pipe以从CGI应用程序读取输出。
					 * cgi_pipe_post映射到CGI应用程序的stdin
					 * 我们在这里传递我们的POST数据(如果有的话)。
					 */
					FILE * cgi_pipe = fdopen(cgi_pipe_w[0], "r");
					FILE * cgi_pipe_post = fdopen(cgi_pipe_r[1], "w");

					if (c_length > 0) {
						/*
						 * 写post数据到应用
						 */
						size_t total_read = 0;
						char buf[CGI_POST];
						while ((total_read < c_length) && (!feof(socket_stream))) {
							size_t diff = c_length - total_read;
							if (diff > CGI_POST) {
								/*
								 * 如果剩下的比缓冲的多，
								 * 只读取缓冲区大小的数据。
								 */
								diff = CGI_POST;
							}
							size_t read;
							read = fread(buf, 1, diff, socket_stream);
							total_read += read;
							// printf("写post数据到cgi程序中：%s\n", buf);
							/*
							 * 写到 CGI管道							 
							 */
							fwrite(buf, 1, read, cgi_pipe_post);
						}
					}
					if (cgi_pipe_post) {
						fclose(cgi_pipe_post);
					}

					/*
					 * 从cgi应用中读取头部
					 */
					char buf[CGI_BUFFER];
					if (!cgi_pipe) {
						generic_response(socket_stream, (char *)"500 Internal Server Error", (char *)"Failed to execute CGI script.");
						// pthread_detach(_waitthread);
						goto _next;
					}
					fprintf(socket_stream, "HTTP/1.1 200 OK\r\n");
					fprintf(socket_stream, "Server: " VERSION_STRING "\r\n");
					unsigned int j = 0;
					while (!feof(cgi_pipe)) {
						/*
						 * 一直获取头部信息，直到结束
						 */
						char * in = fgets(buf, CGI_BUFFER - 2, cgi_pipe);
						if (!in) {
							fprintf(stderr,"[warn] Read nothing [%d on %p %d %d]\n", ferror(cgi_pipe), (void *)cgi_pipe, cgi_pipe_w[1], _pid);
							perror("[warn] Specifically");
							buf[0] = '\0';
							break;
						}
						if (!strcmp(in, "\r\n") || !strcmp(in, "\n")) {
							/*
							 * 读取头部信息结束
							 */
							buf[0] = '\0';
							break;
						}
						if (!strstr(in, ": ") && !strstr(in, "\r\n")) {
							fprintf(stderr, "[warn] Garbage trying to read header line from CGI [%zu]\n", strlen(buf));
							break;
						}
						fwrite(in, 1, strlen(in), socket_stream);
						++j;
					}
					if (j < 1) {
						fprintf(stderr,"[warn] CGI script did not give us headers.\n");
					}
					if (feof(cgi_pipe)) {
						fprintf(stderr,"[warn] Sadness: Pipe closed during headers.\n");
					}

					if (request_type == 3) {
						/*
						 * 如果是head请求，直接到这就可以
						 */
						fprintf(socket_stream, "\r\n");
						// pthread_detach(_waitthread);
						goto _next;
					}

					int enc_mode = 0;
					if (!strcmp(http_version, "HTTP/1.1")) {
						/*
						 * 将Transfer-Encoding设置为chunked，这样我们就可以发送了
						 * 一旦我们得到了它们，就会立即销毁
						 * 一次性读取所有输出
						 */
						fprintf(socket_stream, "Transfer-Encoding: chunked\r\n");
					} else {
						/*
						 * 不是 HTTP/1.1 版本
						 * 使用 Connection: Close
						 */
						fprintf(socket_stream, "Connection: close\r\n\r\n");
						enc_mode = 1;
					}

					if (strlen(buf) > 0) {
						fprintf(stderr, "[warn] Trying to dump remaining content.\n");
						fprintf(socket_stream, "\r\n%zX\r\n", strlen(buf));
						fwrite(buf, 1, strlen(buf), socket_stream);
					}

					/*
					 * 从CGI脚本读取输出并作为块发送。
					 */
					while (!feof(cgi_pipe)) {
						size_t read = -1;
						read = fread(buf, 1, CGI_BUFFER - 1, cgi_pipe);
						if (read < 1) {
							/*
							 * 没有读取到，或者出错了
							 */
							fprintf(stderr, "[warn] Read nothing on content without eof.\n");
							perror("[warn] Error on read");
							break;
						}
						if (enc_mode == 0) {
							fprintf(socket_stream, "\r\n%zX\r\n", read);
						}
						fwrite(buf, 1, read, socket_stream);
					}
					if (enc_mode == 0) {
						/*
						 * 我们以一个0长度的块结束`chunked`编码
						 */
						fprintf(socket_stream, "\r\n0\r\n\r\n");
					}

					/*
					 * 释放等待的进程的内存
					 */
					// pthread_detach(_waitthread);
					if (cgi_pipe) {
						fclose(cgi_pipe);
					}

					/*
					 * 完成cgi的执行，关闭资源
					 */
					if (enc_mode == 0) {
						/*
						 * HTTP/1.1
						 * Chunked encoding.
						 */
						goto _next;
					} else {
						/*
						 * HTTP/1.0
						 * Non-chunked, break the connection.
						 */
						delete_vector(queue);
						goto _disconnect;
					}
				}
#endif

				/*
				 * Flat file: Status OK.
				 */
				fprintf(socket_stream, "HTTP/1.1 200 OK\r\n");
			}

			fprintf(socket_stream, "Server: " VERSION_STRING "\r\n");

			/*
			 * 判断文件的MIME类型
			 */
			// printf("ext:%s\n", ext);
			if (ext) {
				if (!strcmp(ext,".htm") || !strcmp(ext,".html")) {
					fprintf(socket_stream, "Content-Type: text/html\r\n");
				} else if (!strcmp(ext,".css")) {
					fprintf(socket_stream, "Content-Type: text/css\r\n");
				} else if (!strcmp(ext,".png")) {
					fprintf(socket_stream, "Content-Type: image/png\r\n");
				} else if (!strcmp(ext,".jpg")) {
					fprintf(socket_stream, "Content-Type: image/jpeg\r\n");
				} else if (!strcmp(ext,".gif")) {
					fprintf(socket_stream, "Content-Type: image/gif\r\n");
				} else if (!strcmp(ext,".pdf")) {
					fprintf(socket_stream, "Content-Type: application/pdf\r\n");
				} else if (!strcmp(ext,".manifest")) {
					fprintf(socket_stream, "Content-Type: text/cache-manifest\r\n");
				} else {
					fprintf(socket_stream, "Content-Type: text/unknown\r\n");
				}
			} else {
				fprintf(socket_stream, "Content-Type: text/unknown\r\n");
			}

			if (request_type == 3) {
				/*
				 * HEAD请求,只执行到这,
				 */
				fprintf(socket_stream, "\r\n");
				fclose(content);
				goto _next;
			}
			/*
			 * 响应长度
			 */
			fseek(content, 0L, SEEK_END); // 文件指针定位到文件末尾，偏移0个字节
			long size = ftell(content); // 获取文件的 当前指针位置 相对于 文件首地址 的 偏移字节数 ;
			fseek(content, 0L, SEEK_SET);  // 文件头 SEEK_SET

			fprintf(socket_stream, "Content-Length: %lu\r\n", size);
			fprintf(socket_stream, "\r\n");

			/*
			 * 读文件
			 */
			char buffer[FLAT_BUFFER];
			fflush(stdout); //将缓冲区的内容输出到设备中
			while (!feof(content)) {
				/*
				 * 将文件以流的形式写入，直到到达文件末尾。
				 */
				size_t read = fread(buffer, 1, FLAT_BUFFER-1, content);
				fwrite(buffer, 1, read, socket_stream);
			}

			fprintf(socket_stream, "\r\n");
			cerr << "结束了" << endl;
			sleep(1);
			fclose(content);
		}

_next:
		/*
		 * Clean up.
		 */
		fflush(socket_stream);
		free(_filename);
		delete_vector(queue);
		sleep(1);
	}

_disconnect:
	
	if (socket_stream) {
		fclose(socket_stream);
	}
	shutdown(request->fd, 2);

	
	// if (request->thread) {
	// 	pthread_detach(request->thread); // 让线程分离 ---自动退出,无系统残留资源
	// }
	// free(request);
	sleep(1);
	return NULL;
}
const int ips_len = 4;
/**
 * 添加ip地址的黑名单
*/
bool netIpIsValid(char * ip) {
	long ret = inet_addr(ip);
	string ip_black[ips_len] = {"127.0.0.1", "192.168.3.67", "192.168.3.68", "0.0.0.0"}; // 添加ip地址黑名单
	if ((ret == 0) || (ret == 0xffffffff))  // 先判断ip地址输入是否正确合法
	{
		std::cout<< "访问的地址："<< ip << "不合法"<< std::endl; // 不合法的地址直接退出
		
		return false;
	}else {
		std::cout<< "访问的地址："<< ip << "合法" << std::endl; // 合法的地址进行下一步的黑名单的判断

		for (int i = 0; i < ips_len; i++)
		{
			if (strstr(ip_black->c_str(), ip))
			{
				cout << "deny from " << ip << endl; 
				return false;
			}
			
			
		}
		cout << "allow from " << ip << endl; 
		return true;
	}
	
}

void start_httpd(unsigned short port, string doc_root)
{
	default_port = port;
	default_root = doc_root;
	cerr << "Starting server (port: " << port <<
		", doc_root: " << doc_root << ")" << endl;
	/**
	 * 初始化socket参数
	*/
	struct sockaddr_in sin;
	serversock = socket(AF_INET, SOCK_STREAM, 0); // tcp
	sin.sin_family      = AF_INET; // 协议族
	sin.sin_port        = htons(port); // 端口号
	sin.sin_addr.s_addr = INADDR_ANY; // 任意ip地址

	/**
	 * 为socket设置重用
	*/
	int _true = 1;
	if (setsockopt(serversock, SOL_SOCKET, SO_REUSEADDR, &_true, sizeof(int)) < 0 ) {
		close(serversock);
		return ;
	}

	/**
	 * 绑定socket
	*/
	if (bind(serversock, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
		fprintf(stderr,"Failed to bind socket to port %d!\n", port);
		return ;
	}

	/**
	 * 开始监听浏览器请求
	*/
	listen(serversock, 50);
	printf("[info] Listening on port %d.\n", port);
	printf("[info] Serving out of %s.\n", default_root.c_str());
	printf("[info] Server version string is " VERSION_STRING ".\n");
	/*
	 * Extensions
	 */
#if ENABLE_CGI
	printf("[extn] CGI support is enabled.\n");
#endif
#if ENABLE_DEFAULTS
	printf("[extn] Default indexes are enabled.\n");
#endif
	/*
	 * 提供一个信号处理函数，要求内核在处理该信号时切换到用户态执行这个处理函数。
	 * 中断信号，当用户按下ctrl-C时，就执行handleShutdown。
	 */
	signal(SIGINT, handleShutdown);

	/*
	 * 当服务器close一个连接时，若client端接着发数据。根据TCP协议的规定，会收到一个RST响应，
	 * client再往这个服务器发送数据时，系统会发出一个SIGPIPE信号给进程，告诉进程这个连接已经断开了，不要再写了。
	 * 根据信号的默认处理规则SIGPIPE信号的默认执行动作是terminate(终止、退出),所以client会退出。
	 * 若不想客户端退出可以把SIGPIPE设为SIG_IGN
	 */
	signal(SIGPIPE, SIG_IGN);
	/*
	 * 开始接收浏览器发出的连接请求
	 */
	while (1) {
		/*
		 * accept一个连接，并把它放在一个新的线程上执行
		 */
		unsigned int c_len;
		struct socket_request * incoming = (socket_request *) calloc(sizeof(struct socket_request),1);
		c_len = sizeof(incoming->address);
		// _last_unaccepted = (void *)incoming; // 将新的连接设置成最后一个断开的请求
		incoming->fd = accept(serversock, (struct sockaddr *) &(incoming->address), &c_len);
		struct sockaddr_in* ip = &(incoming->address);
		struct in_addr ip_str = ip->sin_addr;
		int ret = netIpIsValid(inet_ntoa(ip_str));
		// _last_unaccepted = NULL;
		if (!ret)
		{
			char buf[520]="HTTP/1.1 403 forbidden\r\nContent-Length: 100\r\nContent-Type: text/html; charset=utf-8\r\n\r\nerror message and the content should not be returned, nor should any metadata about the real file.";//HTTP响应
            send(incoming->fd, buf, strlen(buf),0);
			close(incoming->fd); // 关闭客户端资源
			continue;
		}
		// pthread_create(&(incoming->thread), NULL, handleRequest, (void *)(incoming)); // 创建线程
		threadPoolAdd(pool, (void (*)(void *))handleRequest, (void *)(incoming));
	}

	printf("**********");
	sleep(30);

	threadPoolDestroy(pool);

}

/**
 * 以下方法实现线程池管理
*/
const int NUMBER = 2;
// 任务结构体
typedef struct Task
{
    void (*function)(void* arg);
    void* arg;
}Task;

// 线程池结构体
struct ThreadPool
{
    // 任务队列
    Task* taskQ;
    int queueCapacity;  // 容量
    int queueSize;      // 当前任务个数
    int queueFront;     // 队头 -> 取数据
    int queueRear;      // 队尾 -> 放数据

    pthread_t managerID;    // 管理者线程ID
    pthread_t *threadIDs;   // 工作的线程ID
    int minNum;             // 最小线程数量
    int maxNum;             // 最大线程数量
    int busyNum;            // 忙的线程的个数
    int liveNum;            // 存活的线程的个数
    int exitNum;            // 要销毁的线程个数
    pthread_mutex_t mutexPool;  // 锁整个的线程池
    pthread_mutex_t mutexBusy;  // 锁busyNum变量
    pthread_cond_t notFull;     // 任务队列是不是满了
    pthread_cond_t notEmpty;    // 任务队列是不是空了

    int shutdown;           // 是不是要销毁线程池, 销毁为1, 不销毁为0
};

ThreadPool* threadPoolCreate(int min, int max, int queueSize)
{
    ThreadPool* pool = (ThreadPool*)malloc(sizeof(ThreadPool));
    do 
    {
        if (pool == NULL)
        {
            printf("malloc threadpool fail...\n");
            break;
        }

        pool->threadIDs = (pthread_t*)malloc(sizeof(pthread_t) * max);
        if (pool->threadIDs == NULL)
        {
            printf("malloc threadIDs fail...\n");
            break;
        }
        memset(pool->threadIDs, 0, sizeof(pthread_t) * max);
        pool->minNum = min;
        pool->maxNum = max;
        pool->busyNum = 0;
        pool->liveNum = min;    // 和最小个数相等
        pool->exitNum = 0;

        if (pthread_mutex_init(&pool->mutexPool, NULL) != 0 ||
            pthread_mutex_init(&pool->mutexBusy, NULL) != 0 ||
            pthread_cond_init(&pool->notEmpty, NULL) != 0 ||
            pthread_cond_init(&pool->notFull, NULL) != 0)
        {
            printf("mutex or condition init fail...\n");
            break;
        }

        // 任务队列
        pool->taskQ = (Task*)malloc(sizeof(Task) * queueSize);
        pool->queueCapacity = queueSize;
        pool->queueSize = 0;
        pool->queueFront = 0;
        pool->queueRear = 0;

        pool->shutdown = 0;

        // 创建线程
        pthread_create(&pool->managerID, NULL, manager, pool);
        for (int i = 0; i < min; ++i)
        {
            pthread_create(&pool->threadIDs[i], NULL, worker, pool);
        }
        return pool;
    } while (0);

    // 释放资源
    if (pool && pool->threadIDs) free(pool->threadIDs);
    if (pool && pool->taskQ) free(pool->taskQ);
    if (pool) free(pool);

    return NULL;
}

int threadPoolDestroy(ThreadPool* pool)
{
    if (pool == NULL)
    {
        return -1;
    }

    // 关闭线程池
    pool->shutdown = 1;
    // 阻塞回收管理者线程
    pthread_join(pool->managerID, NULL);
    // 唤醒阻塞的消费者线程
    for (int i = 0; i < pool->liveNum; ++i)
    {
        pthread_cond_signal(&pool->notEmpty);
    }
    // 释放堆内存
    if (pool->taskQ)
    {
        free(pool->taskQ);
    }
    if (pool->threadIDs)
    {
        free(pool->threadIDs);
    }

    pthread_mutex_destroy(&pool->mutexPool);
    pthread_mutex_destroy(&pool->mutexBusy);
    pthread_cond_destroy(&pool->notEmpty);
    pthread_cond_destroy(&pool->notFull);

    free(pool);
    pool = NULL;

    return 0;
}


void threadPoolAdd(ThreadPool* pool, void(*func)(void*), void* arg)
{
    pthread_mutex_lock(&pool->mutexPool);
    while (pool->queueSize == pool->queueCapacity && !pool->shutdown)
    {
        // 阻塞生产者线程
        pthread_cond_wait(&pool->notFull, &pool->mutexPool);
    }
    if (pool->shutdown)
    {
        pthread_mutex_unlock(&pool->mutexPool);
        return;
    }
    // 添加任务
    pool->taskQ[pool->queueRear].function = func;
    pool->taskQ[pool->queueRear].arg = arg;
    pool->queueRear = (pool->queueRear + 1) % pool->queueCapacity;
    pool->queueSize++;

    pthread_cond_signal(&pool->notEmpty);
    pthread_mutex_unlock(&pool->mutexPool);
}

int threadPoolBusyNum(ThreadPool* pool)
{
    pthread_mutex_lock(&pool->mutexBusy);
    int busyNum = pool->busyNum;
    pthread_mutex_unlock(&pool->mutexBusy);
    return busyNum;
}

int threadPoolAliveNum(ThreadPool* pool)
{
    pthread_mutex_lock(&pool->mutexPool);
    int aliveNum = pool->liveNum;
    pthread_mutex_unlock(&pool->mutexPool);
    return aliveNum;
}

void* worker(void* arg)
{
    ThreadPool* pool = (ThreadPool*)arg;

    while (1)
    {
        pthread_mutex_lock(&pool->mutexPool);
        // 当前任务队列是否为空
        while (pool->queueSize == 0 && !pool->shutdown)
        {
            // 阻塞工作线程
            pthread_cond_wait(&pool->notEmpty, &pool->mutexPool);

            // 判断是不是要销毁线程
            if (pool->exitNum > 0)
            {
                pool->exitNum--;
                if (pool->liveNum > pool->minNum)
                {
                    pool->liveNum--;
                    pthread_mutex_unlock(&pool->mutexPool);
                    threadExit(pool);
                }
            }
        }

        // 判断线程池是否被关闭了
        if (pool->shutdown)
        {
            pthread_mutex_unlock(&pool->mutexPool);
            threadExit(pool);
        }

        // 从任务队列中取出一个任务
        Task task;
        task.function = pool->taskQ[pool->queueFront].function;
        task.arg = pool->taskQ[pool->queueFront].arg;
        // 移动头结点
        pool->queueFront = (pool->queueFront + 1) % pool->queueCapacity;
        pool->queueSize--;
        // 解锁
        pthread_cond_signal(&pool->notFull);
        pthread_mutex_unlock(&pool->mutexPool);

        printf("thread %ld start working...\n", pthread_self());
        pthread_mutex_lock(&pool->mutexBusy);
        pool->busyNum++;
        pthread_mutex_unlock(&pool->mutexBusy);
        task.function(task.arg);
        free(task.arg);
        task.arg = NULL;

        printf("thread %ld end working...\n", pthread_self());
        pthread_mutex_lock(&pool->mutexBusy);
        pool->busyNum--;
        pthread_mutex_unlock(&pool->mutexBusy);
    }
    return NULL;
}

void* manager(void* arg)
{
    ThreadPool* pool = (ThreadPool*)arg;
    while (!pool->shutdown)
    {
        // 每隔3s检测一次
        sleep(3);

        // 取出线程池中任务的数量和当前线程的数量
        pthread_mutex_lock(&pool->mutexPool);
        int queueSize = pool->queueSize;
        int liveNum = pool->liveNum;
        pthread_mutex_unlock(&pool->mutexPool);

        // 取出忙的线程的数量
        pthread_mutex_lock(&pool->mutexBusy);
        int busyNum = pool->busyNum;
        pthread_mutex_unlock(&pool->mutexBusy);

        // 添加线程
        // 任务的个数>存活的线程个数 && 存活的线程数<最大线程数
        if (queueSize > liveNum && liveNum < pool->maxNum)
        {
            pthread_mutex_lock(&pool->mutexPool);
            int counter = 0;
            for (int i = 0; i < pool->maxNum && counter < NUMBER
                && pool->liveNum < pool->maxNum; ++i)
            {
                if (pool->threadIDs[i] == 0)
                {
                    pthread_create(&pool->threadIDs[i], NULL, worker, pool);
                    counter++;
                    pool->liveNum++;
                }
            }
            pthread_mutex_unlock(&pool->mutexPool);
        }
        // 销毁线程
        // 忙的线程*2 < 存活的线程数 && 存活的线程>最小线程数
        if (busyNum * 2 < liveNum && liveNum > pool->minNum)
        {
            pthread_mutex_lock(&pool->mutexPool);
            pool->exitNum = NUMBER;
            pthread_mutex_unlock(&pool->mutexPool);
            // 让工作的线程自杀
            for (int i = 0; i < NUMBER; ++i)
            {
                pthread_cond_signal(&pool->notEmpty);
            }
        }
    }
    return NULL;
}

void threadExit(ThreadPool* pool)
{
    pthread_t tid = pthread_self();
    for (int i = 0; i < pool->maxNum; ++i)
    {
        if (pool->threadIDs[i] == tid)
        {
            pool->threadIDs[i] = 0;
            printf("threadExit() called, %ld exiting...\n", tid);
            break;
        }
    }
    pthread_exit(NULL);
}

