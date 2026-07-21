// -*-c++-*-

/***************************************************************************
 client.cpp  -  A basic client that connects to
 the server
 -------------------
 begin                : 27-DEC-2001
 copyright            : (C) 2001 by The RoboCup Soccer Server
 Maintenance Group.
 email                : sserver-admin@lists.sourceforge.net
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU LGPL as published by the Free Software  *
 *   Foundation; either version 3 of the License, or (at your option) any  *
 *   later version.                                                        *
 *                                                                         *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H  //条件编译  C++预处理器
#include "config.h"
#endif

#include "compress.h"

#include <rcssbase/net/socketstreambuf.hpp> //rcssbase可以实现网络连接、参数读取、序列化等一些通用的功能，是仿真比赛server 的底层库
#include <rcssbase/net/udpsocket.hpp> //hpp,其实质就是将.cpp的实现代码混入.h头文件当中，定义与实现都包含在同一文件
#include <rcssbase/gzip/gzstream.hpp>

#ifdef HAVE_SSTREAM // 在C++有两种字符串流，一种在sstream中定义，另一种在strstream中定义
#include <sstream> //处理字符串
#else//C++引入了ostringstream、istringstream、stringstream这三个类，要使用他们创建对象就必须包含sstream.h头文件
#include <strstream> //字符串流操作 与c兼容
#endif
#include <iostream> //输入输出流
#include <cerrno> // errno 是记录系统的最后一次错误代码  在errno.h中定义  errno不同的值表示不同的含义
#include <csignal> // 定义了程序执行时如何处理不同的信号
#include <cstdio> //cstdio是将stdio.h的内容用C++头文件的形式表示出来。stdio.h是C标准函数库中的头文件，提供基本的文字的输入输出流操作
#include <cstdlib> //cstdlib是C++里面的一个常用函数库， 等价于C中的<stdlib.h>,stdlib.h可以提供一些函数与符号常量，
#include <cstring> //<cstring>是C标准库头文件<string.h>的C++标准库版本  包含了strcmp、strchr、strstr等操作
#include <string.h> // C版本头文件 对应基于char*的字符串处理函数

#ifdef __CYGWIN__
// cygwin is not win32
#elif defined(_WIN32) || defined(__WIN32__) || defined (WIN32)
#  define RCSS_WIN
#  include <winsock2.h> // 连接系统和用户使用的软件之间用于交流的一个接口，这个功能就是修复软件与系统正确的通讯的作用
#endif

#ifndef RCSS_WIN
#  include <unistd.h> // 是 C++ 程序设计语言中提供对操作系统 API 的访问功能的头文件的名称
#  include <sys/select.h> // select是一个计算机函数，位于头文件#include <sys/select.h> 。该函数用于监视文件描述符的变化情况——读写或是异常。
#  include <sys/time.h> // 定义了四个变量类型、两个宏和各种操作日期和时间的函数
#  include <sys/types.h> // 基本系统数据类型 定义了很多类型
#endif

int iPlayerId = 0;
int currentCycle = 0;
int lastCycle = 0;
int kickWait = 0;
int turnToSeeGoal = 0;
int iSide = 0;//1:left;2:right

//极坐标变为直角坐标
void poly2vector(double dLen, double dAngle, double &dX, double &dY) {
	
}

class Client {
private:
	rcss::net::Addr M_dest;  // 1)global scope(全局作用域符），用法（::name)
	rcss::net::UDPSocket M_socket;  // 2)class scope(类作用域符），用法(class::name)
	rcss::net::SocketStreamBuf * M_socket_buf; // 3)namespace scope(命名空间作用域符），用法(namespace::name)
	rcss::gz::gzstreambuf * M_gz_buf;
	std::ostream * M_transport;
	int M_comp_level;
	bool M_clean_cycle;

#ifdef HAVE_LIBZ
	Decompressor M_decomp;
#endif

	Client(); // 构造函数
	Client(const Client &);
	Client & operator=(const Client &); //运算符重载
public: // 构造函数初始化列表以一个冒号开始，接着是以逗号分隔的数据成员列表，每个数据成员后面跟一个放在括号中的初始化式。
	Client(const std::string & server, const int port) :
		M_dest(port), M_socket(), M_socket_buf(NULL), M_gz_buf(NULL),
				M_transport(NULL), M_comp_level(-1), M_clean_cycle(true) {
		M_dest.setHost(server);
		open(); // 打开
		bind(); // 绑定

		M_socket_buf->setEndPoint(M_dest); // 指针访问结构或者类的成员，必须使用 -> 运算符
	}

	virtual ~Client() { // 析构函数
		close(); // 关闭
	}

	bool sendCmd(char *command) {
		int len;
		len = strlen(command) + 1; // strlen c++字符串操作函数
		printf("command:%s\n", command);
		M_transport->write(command, len);
		M_transport->flush();
		if (!M_transport->good()) {
			printf("error send socket\n");
			return false;
		}
		return true;
	}

	void run() {
		char command[20];
		if (iSide == 1) {
			if (iPlayerId == 1) {
				sprintf(command, "(init team1 (goalie))");
			}
			else {
				sprintf(command, "(init team1)");
			}
		} else if (iSide == 2) {
			if (iPlayerId == 1) {
				sprintf(command, "(init team2 (goalie))");
			}
			else {
				sprintf(command, "(init team2)");
			}
		} else {
			return;
		}
		if (!sendCmd(command))
			return;
		if (iPlayerId == 1) {
			sprintf(command, "(move -45 0)");
		}
		else {
			sprintf(command, "(move -3 0)");
		}
		if (!sendCmd(command))
			return;
		messageLoop(); // 函数  消息循环 踢球中。。。
	}

private:

	int open() {
		if (M_socket.open()) {
			if (M_socket.setNonBlocking() < 0) {
				std::cerr << __FILE__ << ": " << __LINE__
						<< ": Error setting socket non-blocking: " << strerror(
						errno) << std::endl;
				M_socket.close();
				return -1;
			}
		} else {
			std::cerr << __FILE__ << ": " << __LINE__
					<< ": Error opening socket: " << strerror(errno)
					<< std::endl;
			M_socket.close();
			return -1;
		}

		M_socket_buf = new rcss::net::SocketStreamBuf(M_socket); // new 动态内存
		M_transport = new std::ostream(M_socket_buf);
		return 0;
	}

	bool bind() {
		if (!M_socket.bind(rcss::net::Addr())) { // rcss::net::Addr() 调用无参构造函数创建一个对象，再把对象传给函数
			std::cerr << __FILE__ << ": " << __LINE__
					<< ": Error connecting socket" << std::endl;
			M_socket.close();
			return false;
		}
		return true;
	}

	void close() {
		M_socket.close();

		if (M_transport) {
			delete M_transport;
			M_transport = NULL;
		}

		if (M_gz_buf) {
			delete M_gz_buf;
			M_gz_buf = NULL;
		}

		if (M_socket_buf) {
			delete M_socket_buf;
			M_socket_buf = NULL;
		}
	}

	int setCompression(int level) {
#ifdef HAVE_LIBZ
		if ( level >= 0 )
		{
			if ( ! M_gz_buf )
			{
				M_gz_buf = new rcss::gz::gzstreambuf( *M_socket_buf );
			}
			M_gz_buf->setLevel( level );
			M_transport->rdbuf( M_gz_buf );
		}
		else
		{
			M_transport->rdbuf( M_socket_buf );
		}
		return M_comp_level = level;
#endif
		return M_comp_level = -1;
	}

	void processMsg(char * msg, const size_t len) { // size_t; cstdlib.h提供的类型之一：size_t, wchar_t, div_t, ldiv_t, lldiv_t
#ifdef HAVE_LIBZ
		if ( M_comp_level >= 0 )
		{
			M_decomp.decompress( msg, len, Z_SYNC_FLUSH );
			char * out;
			int size;
			M_decomp.getOutput( out, size );
			if ( size > 0 )
			{
				parseMsg( out, size );
			}
		}
		else
#endif
		{
			parseMsg(msg, len); // 函数 消息解析
		}
	}

	void parseMsg(char * msg, const size_t len) {
		std::cout << std::string( msg, len - 1 ) << std::endl;
		if (!std::strncmp(msg, "(ok compression", 15)) { //strncmp 比较字符串 相同返回0
			int level;
			if (std::sscanf(msg, " ( ok compression %d )", &level) == 1) { // level会等于compression 后面的数字
				setCompression(level);
			}
		} else if (!std::strncmp(msg, "(sense_body", 11) //串比较，比较msg和sense_body前11个字符  请求server 发送身体感知信息
				|| !std::strncmp(msg, "(see_global", 11) || !std::strncmp(msg,
				"(init", 5)) { // 如果教练打开了视觉消息开关（eye on），则在每个周期的开始，它将定期收到全局视觉消息
			M_clean_cycle = true;
		}


		if (iPlayerId == 1) {
			//守门员
			if (std::strncmp(msg, "(see ", 5)) {
				return;
			}
			double ball_dist = 0;
			double ball_dir = 0;
			char command[20];
			char *pball;
			pball = strstr(msg, "(ball)"); // strstr在串中查找指定字符串的第一次出现
			if (pball == 0) {
				sprintf(command, "(turn 60)"); //把结果输出到指定的字符串
				if (!sendCmd(command))
					return;
				return;
			}
			//看见球
			if (std::sscanf(pball, "(ball) %lf %lf", &ball_dist, &ball_dir)
					!= 2) {
				printf("get ball error\n");
				return;
			}

			//			printf("%s\t%lf\t%lf\t%lf\t%lf\n", msg, goal_dist, goal_dir,
			//					ball_dist, ball_dir);
			//扑球
			if (ball_dist < 3) {
				sprintf(command, "(catch %lf)", ball_dir);
				if (!sendCmd(command))
					return;
				return;
			}
			if (ball_dir > 10 || ball_dir < -10) {
				sprintf(command, "(turn %lf)", ball_dir);
				if (!sendCmd(command))
					return;
				return;
			}
		} else {
			if (!std::strncmp(msg, "(see ", 5)) {
				//see
				//get cycle
				if (std::sscanf(msg, "(see  %d )", &currentCycle) == 1) {
					if (currentCycle == 0) {
						//before_kick_off
						return;
					}
					if (currentCycle == lastCycle) {
						//before_kick_off
						return;
					}
					if (currentCycle > lastCycle) {
						kickWait++;
						if (kickWait < 3)
							return;
						double goal_dist = 0;
						double goal_dir = 0;
						double ball_dist = 0;
						double ball_dir = 0;
						char command[20];
						int len;
						int canSeeGoal;
						//kick_off
						lastCycle = currentCycle;
						//search goal
						char *pgoal;
						// 左队进攻右球门，右队进攻左球门。
						// move 命令的坐标由服务端按 side 自动镜像，
						// 但视觉消息中的球门名称不会自动替换。
						const char *opponentGoal = (iSide == 1)
								? "(goal r)"
								: "(goal l)";
						pgoal = strstr(msg, opponentGoal);
						if (pgoal != 0) {
							if (std::sscanf(pgoal, "(goal %*c) %lf %lf",
									&goal_dist, &goal_dir) != 2) {
								printf("get goal error\n");
							}
							canSeeGoal = 1;
						} else {
							canSeeGoal = 0;
						}
						char *pball;
						if (!turnToSeeGoal) {
							pball = strstr(msg, "(ball)");
							if (pball != 0) {
								if (std::sscanf(pball, "(ball) %lf %lf",
										&ball_dist, &ball_dir) != 2) {
									printf("get ball error\n");
								}
							} else {
								sprintf(command, "(turn 50)");
								if (!sendCmd(command))
									return;
								printf("turn to see ball\n");
								return;
							}
							printf("%s\t%lf\t%lf\t%lf\t%lf\n", msg, goal_dist,
									goal_dir, ball_dist, ball_dir);
							//ball turn
							if (ball_dir > 2 || ball_dir < -2) {
								sprintf(command, "(turn %lf)", ball_dir);
								if (!sendCmd(command))
									return;
								return;
							}
							//ball dash
							if (ball_dist > 0.5) {
								sprintf(command, "(dash 100)");
								if (!sendCmd(command))
									return;
								return;
							}
						}
						//kick
						turnToSeeGoal = 1;
						if (!canSeeGoal) {
							sprintf(command, "(turn 50)");
							len = strlen(command) + 1;
							printf("command:%s\n", command);
							M_transport->write(command, len);
							M_transport->flush();
							if (!M_transport->good()) {
								printf("error send socket\n");
								return;
							}
							printf("turn to see goal\n");
							return;
						}
						sprintf(command, "(kick 100 %lf)", goal_dir);
						if (!sendCmd(command))
							return;
						kickWait = 0;
						turnToSeeGoal = 0;
					}
				}
			}
		}
	}

	void messageLoop() {
		fd_set read_fds;
		fd_set read_fds_back;
		char buf[8192];
		std::memset(&buf, 0, sizeof(char) * 8192);

		int in = fileno( stdin );
		FD_ZERO(&read_fds);
		FD_SET(in, &read_fds);
		FD_SET(M_socket.getFD(), &read_fds);
		read_fds_back = read_fds;

#ifdef RCSS_WIN
		int max_fd = 0;
#else
		int max_fd = M_socket.getFD() + 1;
#endif
		while (1) {

			read_fds = read_fds_back;
			int ret = ::select(max_fd, &read_fds, NULL, NULL, NULL); //select
			if (ret < 0) {
				perror("Error selecting input");
				break;
			} else if (ret != 0) {
				// read from stdin
				if (FD_ISSET(in, &read_fds)) {
					if (std::fgets(buf, sizeof(buf), stdin) != NULL) {
						size_t len = std::strlen(buf);
						if (buf[len - 1] == '\n') {
							buf[len - 1] = '\0';
							--len;
						}

						M_transport->write(buf, len + 1);
						M_transport->flush();
						if (!M_transport->good()) {
							if (errno != ECONNREFUSED) {
								std::cerr << __FILE__ << ": " << __LINE__
										<< ": Error sending to socket: "
										<< strerror(errno) << std::endl
										<< "msg = [" << buf << "]\n";
							}
							M_socket.close();
						}
						std::cout << buf << std::endl;
					}
				}

				// read from socket
				if (FD_ISSET(M_socket.getFD(), &read_fds)) {
					rcss::net::Addr from;
					int len = M_socket.recv(buf, sizeof(buf) - 1, from);
					if (len == -1 && errno != EWOULDBLOCK) {
						if (errno != ECONNREFUSED) {
							std::cerr << __FILE__ << ": " << __LINE__
									<< ": Error receiving from socket: "
									<< strerror(errno) << std::endl;
						}
						M_socket.close();
					} else if (len > 0) {
						M_dest.setPort(from.getPort());
						M_socket_buf->setEndPoint(M_dest);
						processMsg(buf, len);
					}
				}
			}
		}
	}
};

namespace {
Client * client = static_cast<Client *> (0);

void sig_exit_handle(int) {
	std::cerr << "\nKilled. Exiting..." << std::endl;
	if (client) {
		delete client;
		client = static_cast<Client *> (0);
	}
	std::exit(EXIT_FAILURE);
}
}

int main(int argc, char **argv) {
	if (std::signal(SIGINT, &sig_exit_handle) == SIG_ERR || std::signal(  // 查看csignal.h 的功能
			SIGTERM, &sig_exit_handle) == SIG_ERR || std::signal(SIGHUP,
			&sig_exit_handle) == SIG_ERR) {
		std::cerr << __FILE__ << ": " << __LINE__
				<< ": could not set signal handler: " << std::strerror(errno)
				<< std::endl;
		std::exit(EXIT_FAILURE);
	}

	std::cerr << "Hit Ctrl-C to exit." << std::endl;

	std::string server = "localhost";
	int port = 6000;

	for (int i = 0; i < argc; ++i) {
		if (std::strcmp(argv[i], "-server") == 0) {
			if (i + 1 < argc) {
				server = argv[i + 1];
				++i;
			}
		} else if (std::strcmp(argv[i], "-port") == 0) {
			if (i + 1 < argc) {
				port = std::atoi(argv[i + 1]);
				++i;
			}
		}
		if (std::strcmp(argv[i], "-id") == 0) {
			if (i + 1 < argc) {
				iPlayerId = std::atoi(argv[i + 1]);
				++i;
			}
		}
		if (std::strcmp(argv[i], "-sidel") == 0) {
			iSide = 1;
		}
		if (std::strcmp(argv[i], "-sider") == 0) {
			iSide = 2;
		}
	}

	client = new Client(server, port);
	client->run();

	return EXIT_SUCCESS;
}
