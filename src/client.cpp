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
#include <string>
#include <string.h> // C版本头文件 对应基于char*的字符串处理函数
#include <cmath>

#ifdef __CYGWIN__
// cygwin 不算 win32
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
int iSide = 0;//1:left;2:right
int lastSeeCycle = -1;
int goalieCatchCycle = -100;
// 默认为空：run() 按 iSide 选用 team1/team2，保证左右为两支不同队伍。
// 若传入 -team，则使用自定义队名覆盖上述默认。
std::string teamName;
bool teamNameFromArg = false;

double absDouble(double value) {
	return value < 0.0 ? -value : value;
}

double normalizeAngle(double angle) {
	while (angle > 180.0) angle -= 360.0;
	while (angle < -180.0) angle += 360.0;
	return angle;
}

struct SeenPlayer {
	int unum;
	double distance;
	double direction;
};

struct VisualState {
	int cycle;
	bool hasBall;
	double ballDistance;
	double ballDirection;
	bool hasOwnGoal;
	bool hasAttackGoal;
	double ownGoalDistance;
	double ownGoalDirection;
	double attackGoalDistance;
	double attackGoalDirection;
	SeenPlayer teammates[4];
	int teammateCount;
	SeenPlayer opponents[5];
	int opponentCount;
};

enum PlayMode {
	PM_UNKNOWN = 0,
	PM_BEFORE_KICK_OFF,
	PM_PLAY_ON,
	PM_GOAL_L,
	PM_GOAL_R,
	PM_KICK_OFF_L,
	PM_KICK_OFF_R,
	PM_OTHER
};

struct MatchState {
	PlayMode playMode;
	int sideFromServer; // 0 unknown, 1 left, 2 right
	int unumFromServer;
};

MatchState gMatchState = { PM_UNKNOWN, 0, 0 };
int lastHeardChaseUnum = 0;
int lastHeardChaseCycle = -100;
int lastChaseSayCycle = -100;
const char *gLastChaseReason = "none";
int defenderAdvanceSteps = 0;

struct DefenderRole {
	double normalMinimumGoalDistance;
	double normalMaximumGoalDistance;
	int flankSign;
};

DefenderRole defenderRole(int playerId) {
	DefenderRole role;
	role.normalMinimumGoalDistance = playerId == 2 ? 18.0 : 20.0;
	role.normalMaximumGoalDistance = playerId == 2 ? 27.0 : 29.0;
	role.flankSign = playerId == 2 ? -1 : 1;
	return role;
}

double estimateDistance(double selfToA, double selfToB,
		double dirA, double dirB) {
	const double delta = (dirA - dirB) * 3.141592653589793 / 180.0;
	const double d2 = selfToA * selfToA + selfToB * selfToB
			- 2.0 * selfToA * selfToB * std::cos(delta);
	return d2 > 0.0 ? std::sqrt(d2) : 0.0;
}

int chaseRoleRank(int playerId) {
	if (playerId == 4) return 4;
	if (playerId == 5) return 3;
	if (playerId == 2) return 2;
	if (playerId == 3) return 1;
	return 0;
}

bool parseInitMessage(const char *msg) {
	char sideChar = 0;
	int unum = 0;
	if (std::sscanf(msg, "(init %c %d", &sideChar, &unum) < 2) {
		return false;
	}
	if (sideChar == 'l' || sideChar == 'L') {
		gMatchState.sideFromServer = 1;
	}
	else if (sideChar == 'r' || sideChar == 'R') {
		gMatchState.sideFromServer = 2;
	}
	else {
		return false;
	}
	gMatchState.unumFromServer = unum;
	if (gMatchState.sideFromServer == 1 || gMatchState.sideFromServer == 2) {
		iSide = gMatchState.sideFromServer;
	}
	if (unum >= 1 && unum <= 5) {
		iPlayerId = unum;
	}
	printf("init_parse: sideFromServer=%d unumFromServer=%d iSide=%d iPlayerId=%d\n",
			gMatchState.sideFromServer, gMatchState.unumFromServer,
			iSide, iPlayerId);
	return true;
}

bool parseRefereeMessage(const char *msg, MatchState &state) {
	// (hear <time> referee <play_mode>)
	const char *referee = std::strstr(msg, "referee ");
	if (referee == 0) {
		return false;
	}
	referee += 8;
	if (std::strncmp(referee, "before_kick_off", 15) == 0) {
		state.playMode = PM_BEFORE_KICK_OFF;
	}
	else if (std::strncmp(referee, "play_on", 7) == 0) {
		state.playMode = PM_PLAY_ON;
	}
	else if (std::strncmp(referee, "goal_l", 6) == 0) {
		state.playMode = PM_GOAL_L;
	}
	else if (std::strncmp(referee, "goal_r", 6) == 0) {
		state.playMode = PM_GOAL_R;
	}
	else if (std::strncmp(referee, "kick_off_l", 10) == 0) {
		state.playMode = PM_KICK_OFF_L;
	}
	else if (std::strncmp(referee, "kick_off_r", 10) == 0) {
		state.playMode = PM_KICK_OFF_R;
	}
	else {
		state.playMode = PM_OTHER;
	}
	printf("referee_parse: playMode=%d\n", static_cast<int>(state.playMode));
	return true;
}

void handleHearMessage(const char *msg, int cycle) {
	if (std::strstr(msg, "referee ") != 0) {
		parseRefereeMessage(msg, gMatchState);
		return;
	}
	if (std::strstr(msg, "self") != 0) {
		return;
	}

	const char *quoted = std::strchr(msg, '"');
	if (quoted == 0 || quoted[1] != 'c') {
		return;
	}
	const int claimed = quoted[2] - '0';
	if (claimed < 2 || claimed > 5 || claimed == iPlayerId) {
		return;
	}
	lastHeardChaseUnum = claimed;
	lastHeardChaseCycle = cycle;
}

bool shouldChaseBall(const VisualState &state, int playerId) {
	gLastChaseReason = "none";
	if (!state.hasBall) {
		gLastChaseReason = "no_ball";
		return false;
	}

	bool sawComparableTeammate = false;
	for (int i = 0; i < state.teammateCount; ++i) {
		const SeenPlayer &tm = state.teammates[i];
		if (tm.unum < 2 || tm.unum > 5 || tm.unum == playerId) {
			continue;
		}
		sawComparableTeammate = true;
		const double est = estimateDistance(tm.distance, state.ballDistance,
				tm.direction, state.ballDirection);
		if (est + 1.5 < state.ballDistance) {
			gLastChaseReason = "closer";
			return false;
		}
		if (absDouble(est - state.ballDistance) <= 1.5 && tm.unum < playerId) {
			gLastChaseReason = "closer";
			return false;
		}
	}

	// 门前危急：允许最近后卫接管，即使角色优先级低于 4/5。
	if (state.hasOwnGoal && state.ballDistance < 8.0
			&& state.ownGoalDistance < 25.0
			&& (playerId == 2 || playerId == 3)) {
		bool closerThanOtherDefender = true;
		for (int i = 0; i < state.teammateCount; ++i) {
			const SeenPlayer &tm = state.teammates[i];
			if (tm.unum != 2 && tm.unum != 3) continue;
			if (tm.unum == playerId) continue;
			const double est = estimateDistance(tm.distance, state.ballDistance,
					tm.direction, state.ballDirection);
			if (est + 0.5 < state.ballDistance) {
				closerThanOtherDefender = false;
				break;
			}
		}
		if (closerThanOtherDefender) {
			gLastChaseReason = "emergency";
			return true;
		}
	}

	if (lastHeardChaseUnum >= 2 && lastHeardChaseUnum <= 5
			&& lastHeardChaseUnum != playerId
			&& state.cycle - lastHeardChaseCycle <= 10) {
		gLastChaseReason = "hear";
		return false;
	}

	const bool hearFresh = lastHeardChaseUnum >= 2 && lastHeardChaseUnum <= 5
			&& state.cycle - lastHeardChaseCycle <= 10;
	if (!sawComparableTeammate && !hearFresh) {
		// 信息不足：仅 4 号主动追，避免四人同冲。
		if (chaseRoleRank(playerId) < 4) {
			gLastChaseReason = "priority";
			return false;
		}
	}

	gLastChaseReason = "self";
	return true;
}

const char * attackGoalName() {
	return iSide == 2 ? "(goal l)" : "(goal r)";
}

const char * attackGoalShortName() {
	return iSide == 2 ? "(g l)" : "(g r)";
}

const char * ownGoalName() {
	return iSide == 2 ? "(goal r)" : "(goal l)";
}

const char * ownGoalShortName() {
	return iSide == 2 ? "(g r)" : "(g l)";
}

void formationPosition(int playerId, double &x, double &y) {
	// 开球落位一律使用左队坐标系。
	// 右队的 (move) 由服务器自动镜像，客户端不可再取反。
	static const double formationX[6] = { 0.0, -47.0, -30.0, -30.0, -10.0, -10.0 };
	static const double formationY[6] = { 0.0, 0.0, -13.0, 13.0, -10.0, 10.0 };

	if (playerId < 1 || playerId > 5) playerId = 5;
	x = formationX[playerId];
	y = formationY[playerId];
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
		char command[128];
		double startX = 0.0;
		double startY = 0.0;

		if (iSide != 1 && iSide != 2) {
			return;
		}

		if (!teamNameFromArg) {
			teamName = (iSide == 1) ? "team1" : "team2";
		}

		if (iPlayerId == 1) {
			sprintf(command, "(init %.40s (goalie))", teamName.c_str());
		}
		else {
			sprintf(command, "(init %.40s)", teamName.c_str());
		}
		if (!sendCmd(command))
			return;

		formationPosition(iPlayerId, startX, startY);
		sprintf(command, "(move %.1f %.1f)", startX, startY);
		if (!sendCmd(command))
			return;

		sprintf(command, iPlayerId == 1
				? "(change_view normal high)"
				: "(change_view wide high)");
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
		(void)level;
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

	int readObjectInfo(const char *msg, const char *name,
			double &distance, double &direction) {
		const char *object = strstr(msg, name);
		if (object == 0) return 0;
		return std::sscanf(object + strlen(name), " %lf %lf",
				&distance, &direction) == 2;
	}

	int readBallInfo(const char *msg, double &distance, double &direction) {
		return readObjectInfo(msg, "(ball)", distance, direction)
				|| readObjectInfo(msg, "(b)", distance, direction);
	}

	int readAttackGoalInfo(const char *msg,
			double &distance, double &direction) {
		return readObjectInfo(msg, attackGoalName(), distance, direction)
				|| readObjectInfo(msg, attackGoalShortName(), distance, direction);
	}

	int readOwnGoalInfo(const char *msg,
			double &distance, double &direction) {
		return readObjectInfo(msg, ownGoalName(), distance, direction)
				|| readObjectInfo(msg, ownGoalShortName(), distance, direction);
	}

	void collectPlayersFromSee(const char *msg, const char *tag,
			VisualState &state) {
		const size_t tagLen = std::strlen(tag);
		const char *cursor = msg;
		while ((cursor = std::strstr(cursor, tag)) != 0) {
			char seenTeam[64];
			int unum = 0;
			double distance = 0.0;
			double direction = 0.0;
			seenTeam[0] = '\0';
			const int n = std::sscanf(cursor + tagLen, "%63s %d) %lf %lf",
					seenTeam, &unum, &distance, &direction);
			cursor += tagLen;
			if (n < 4 || unum < 1) {
				continue;
			}

			SeenPlayer seen;
			seen.unum = unum;
			seen.distance = distance;
			seen.direction = direction;

			if (!teamName.empty() && std::strcmp(seenTeam, teamName.c_str()) == 0) {
				if (unum >= 2 && unum <= 5 && unum != iPlayerId
						&& state.teammateCount < 4) {
					state.teammates[state.teammateCount++] = seen;
				}
			}
			else if (state.opponentCount < 5) {
				state.opponents[state.opponentCount++] = seen;
			}
		}
	}

	bool parseVisualState(const char *msg, VisualState &state) {
		std::memset(&state, 0, sizeof(state));
		if (std::sscanf(msg, "(see %d", &state.cycle) != 1) {
			return false;
		}

		state.hasBall = readBallInfo(msg, state.ballDistance,
				state.ballDirection) != 0;
		state.hasOwnGoal = readOwnGoalInfo(msg, state.ownGoalDistance,
				state.ownGoalDirection) != 0;
		state.hasAttackGoal = readAttackGoalInfo(msg, state.attackGoalDistance,
				state.attackGoalDirection) != 0;

		collectPlayersFromSee(msg, "(player ", state);
		collectPlayersFromSee(msg, "(p ", state);
		return true;
	}

	int makeDepthCommand(const char *msg, double minimumDistance,
			double maximumDistance, char *command) {
		double goalDistance = 0.0;
		double goalDirection = 0.0;
		if (!readOwnGoalInfo(msg, goalDistance, goalDirection)) return 0;

		double moveDirection = 0.0;
		if (goalDistance > maximumDistance) {
			moveDirection = goalDirection;
		}
		else if (goalDistance < minimumDistance) {
			moveDirection = normalizeAngle(goalDirection + 180.0);
		}
		else {
			return 0;
		}

		if (absDouble(moveDirection) > 12.0) {
			sprintf(command, "(turn %.1f)", moveDirection);
		}
		else {
			sprintf(command, "(dash 55)");
		}
		return 1;
	}

	void handleGoalkeeper(const char *msg, int cycle) {
		double ballDistance = 0.0;
		double ballDirection = 0.0;
		double goalDistance = 0.0;
		double goalDirection = 0.0;
		const int hasBall = readBallInfo(msg, ballDistance, ballDirection);
		const int hasAttackGoal = readAttackGoalInfo(msg,
				goalDistance, goalDirection);
		char command[128];

		// 扑球成功后，下一两次观测内执行解围踢球。
		if (goalieCatchCycle >= 0 && cycle - goalieCatchCycle <= 2) {
			if (hasBall && ballDistance < 1.5) {
				sprintf(command, "(kick 100 %.1f)",
						hasAttackGoal ? goalDirection : 0.0);
				goalieCatchCycle = -100;
				sendCmd(command);
				return;
			}
		}
		else {
			goalieCatchCycle = -100;
		}

		if (!hasBall) {
			if (makeDepthCommand(msg, 4.0, 9.0, command)) {
				sendCmd(command);
			}
			else {
				sprintf(command, "(turn 45)");
				sendCmd(command);
			}
			return;
		}

		if (ballDistance <= 1.15) {
			sprintf(command, "(catch %.1f)", ballDirection);
			goalieCatchCycle = cycle;
			sendCmd(command);
			return;
		}

		if (ballDistance <= 11.0) {
			if (absDouble(ballDirection) > 10.0) {
				sprintf(command, "(turn %.1f)", ballDirection);
			}
			else if (ballDistance > 6.0) {
				sprintf(command, "(dash 70)");
			}
			else if (ballDistance > 3.0) {
				sprintf(command, "(dash 45)");
			}
			else {
				sprintf(command, "(dash 25)");
			}
			sendCmd(command);
			return;
		}

		if (makeDepthCommand(msg, 4.0, 9.0, command)) {
			sendCmd(command);
		}
		else if (absDouble(ballDirection) > 5.0) {
			sprintf(command, "(turn %.1f)", ballDirection);
			sendCmd(command);
		}
		else {
			sprintf(command, "(turn 0)");
			sendCmd(command);
		}
	}

	bool findVisibleTeammate(const VisualState &visual, int unum,
			SeenPlayer &teammate) {
		for (int i = 0; i < visual.teammateCount; ++i) {
			if (visual.teammates[i].unum == unum) {
				teammate = visual.teammates[i];
				return true;
			}
		}
		return false;
	}

	double ballDistanceFromOwnGoal(const VisualState &visual) {
		if (!visual.hasBall || !visual.hasOwnGoal) {
			return 1000.0;
		}
		return estimateDistance(visual.ballDistance,
				visual.ownGoalDistance, visual.ballDirection,
				visual.ownGoalDirection);
	}

	bool ballInDefenderZone(const VisualState &visual,
			const DefenderRole &role, bool partnerVisible) {
		if (!partnerVisible || !visual.hasOwnGoal) {
			return true;
		}
		const double threatSide = normalizeAngle(
				visual.ballDirection - visual.ownGoalDirection);
		return threatSide * role.flankSign >= -8.0;
	}

	void defenderDepthRange(const VisualState &visual,
			const DefenderRole &role, double &minimumDistance,
			double &maximumDistance) {
		const double ballGoalDistance = ballDistanceFromOwnGoal(visual);
		if (ballGoalDistance < 18.0) {
			minimumDistance = 15.0;
			maximumDistance = 22.0;
		}
		else if (ballGoalDistance < 34.0) {
			minimumDistance = role.normalMinimumGoalDistance;
			maximumDistance = role.normalMaximumGoalDistance;
		}
		else {
			minimumDistance = role.normalMinimumGoalDistance + 3.0;
			maximumDistance = role.normalMaximumGoalDistance + 3.0;
		}
	}

	bool makeDefenderReturnCommand(const VisualState &visual,
			const DefenderRole &role, char *command) {
		double minimumDistance = 0.0;
		double maximumDistance = 0.0;
		defenderDepthRange(visual, role, minimumDistance, maximumDistance);

		if (!visual.hasOwnGoal) {
			if (defenderAdvanceSteps > 0) {
				sprintf(command, "(dash -45)");
				--defenderAdvanceSteps;
			}
			else {
				sprintf(command, "(turn %d)", role.flankSign * 35);
			}
			return true;
		}

		double moveDirection = 0.0;
		if (visual.ownGoalDistance > maximumDistance) {
			moveDirection = visual.ownGoalDirection;
		}
		else if (visual.ownGoalDistance < minimumDistance) {
			moveDirection = normalizeAngle(
					visual.ownGoalDirection + 180.0);
		}
		else {
			return false;
		}

		if (absDouble(moveDirection) > 12.0) {
			sprintf(command, "(turn %.1f)", moveDirection);
		}
		else {
			sprintf(command, "(dash 55)");
			if (visual.ownGoalDistance > maximumDistance
					&& defenderAdvanceSteps > 0) {
				--defenderAdvanceSteps;
			}
		}
		return true;
	}

	void makeDefenderClearCommand(const VisualState &visual,
			const DefenderRole &role, char *command) {
		double clearDirection = role.flankSign * 55.0;
		if (visual.hasAttackGoal
				&& absDouble(visual.attackGoalDirection) <= 90.0) {
			clearDirection = visual.attackGoalDirection;
		}
		else if (visual.hasOwnGoal) {
			clearDirection = normalizeAngle(
					visual.ownGoalDirection + 180.0);
		}
		sprintf(command, "(kick 100 %.1f)", clearDirection);
	}

	void handleDefender(const VisualState &visual, int cycle,
			bool mayChase) {
		char command[160];
		const DefenderRole role = defenderRole(iPlayerId);
		const int partnerUnum = iPlayerId == 2 ? 3 : 2;
		SeenPlayer partner;
		const bool partnerVisible = findVisibleTeammate(
				visual, partnerUnum, partner);

		if (gMatchState.playMode == PM_BEFORE_KICK_OFF
				|| gMatchState.playMode == PM_GOAL_L
				|| gMatchState.playMode == PM_GOAL_R) {
			double x = 0.0;
			double y = 0.0;
			formationPosition(iPlayerId, x, y);
			sprintf(command, "(move %.1f %.1f)", x, y);
			defenderAdvanceSteps = 0;
			sendCmd(command);
			return;
		}

		if (!visual.hasBall) {
			if (!makeDefenderReturnCommand(visual, role, command)) {
				sprintf(command, "(turn %d)", role.flankSign * 35);
			}
			sendCmd(command);
			return;
		}

		if (visual.ballDistance <= 0.75) {
			makeDefenderClearCommand(visual, role, command);
			sendCmd(command);
			return;
		}

		const double ballGoalDistance = ballDistanceFromOwnGoal(visual);
		const bool emergency = ballGoalDistance < 18.0
				|| (visual.hasOwnGoal
						&& visual.ownGoalDistance < 24.0
						&& visual.ballDistance < 8.0);
		const bool ownZone = ballInDefenderZone(
				visual, role, partnerVisible);
		const int advanceLimit = emergency ? 9 : 6;
		const bool insideBoundary = !visual.hasOwnGoal
				? defenderAdvanceSteps < advanceLimit
				: visual.ownGoalDistance < 32.0;
		const bool shouldChallenge = mayChase && insideBoundary
				&& (emergency || ownZone);

		printf("defense_debug: cycle=%d id=%d ballGoal=%.1f zone=%d emergency=%d chase=%d steps=%d\n",
				cycle, iPlayerId, ballGoalDistance, ownZone ? 1 : 0,
				emergency ? 1 : 0, shouldChallenge ? 1 : 0,
				defenderAdvanceSteps);

		if (shouldChallenge) {
			if (absDouble(visual.ballDirection) > 12.0) {
				sprintf(command, "(turn %.1f)", visual.ballDirection);
			}
			else {
				const int dashPower = emergency ? 95 : 75;
				if (cycle - lastChaseSayCycle >= 4) {
					sprintf(command, "(dash %d)(say c%d)",
							dashPower, iPlayerId);
					lastChaseSayCycle = cycle;
				}
				else {
					sprintf(command, "(dash %d)", dashPower);
				}
				if (defenderAdvanceSteps < advanceLimit) {
					++defenderAdvanceSteps;
				}
			}
			sendCmd(command);
			return;
		}

		if (partnerVisible && partner.distance < 7.0) {
			const double separateDirection = normalizeAngle(
					partner.direction + 180.0);
			if (absDouble(separateDirection) > 12.0) {
				sprintf(command, "(turn %.1f)", separateDirection);
			}
			else {
				sprintf(command, "(dash 35)");
			}
			sendCmd(command);
			return;
		}

		if (makeDefenderReturnCommand(visual, role, command)) {
			sendCmd(command);
			return;
		}

		if (absDouble(visual.ballDirection) > 7.0) {
			sprintf(command, "(turn %.1f)", visual.ballDirection);
		}
		else {
			sprintf(command, "(turn %d)", role.flankSign * 20);
		}
		sendCmd(command);
	}

	void handleFieldPlayer(const char *msg, const VisualState &visual,
			int cycle) {
		char command[160];
		const int mayChase = shouldChaseBall(visual, iPlayerId);

		printf("chase_debug: cycle=%d id=%d side=%d srvSide=%d playMode=%d ball=%.1f tm=%d chase=%d reason=%s\n",
				cycle, iPlayerId, iSide, gMatchState.sideFromServer,
				static_cast<int>(gMatchState.playMode),
				visual.hasBall ? visual.ballDistance : -1.0,
				visual.teammateCount, mayChase, gLastChaseReason);

		if (iPlayerId == 2 || iPlayerId == 3) {
			handleDefender(visual, cycle, mayChase != 0);
			return;
		}

		if (!visual.hasBall) {
			sprintf(command, "(turn %d)", iPlayerId % 2 == 0 ? 45 : -45);
			sendCmd(command);
			return;
		}

		const double ballDistance = visual.ballDistance;
		const double ballDirection = visual.ballDirection;
		double goalDistance = visual.attackGoalDistance;
		double goalDirection = visual.attackGoalDirection;

		double chaseLimit = 0.0;
		double minimumHomeDistance = 0.0;
		double maximumHomeDistance = 100.0;
		if (iPlayerId == 2) {
			chaseLimit = 22.0;
			minimumHomeDistance = 17.0;
			maximumHomeDistance = 30.0;
		}
		else if (iPlayerId == 3) {
			chaseLimit = 16.0;
			minimumHomeDistance = 19.0;
			maximumHomeDistance = 32.0;
		}
		else if (iPlayerId == 4) {
			chaseLimit = 38.0;
		}
		else {
			chaseLimit = 20.0;
			minimumHomeDistance = 32.0;
			maximumHomeDistance = 45.0;
		}

		if (!mayChase || ballDistance > chaseLimit) {
			if (makeDepthCommand(msg, minimumHomeDistance,
					maximumHomeDistance, command)) {
				sendCmd(command);
			}
			else if (absDouble(ballDirection) > 6.0) {
				sprintf(command, "(turn %.1f)", ballDirection);
				sendCmd(command);
			}
			else {
				sprintf(command, "(turn %d)",
						iPlayerId % 2 == 0 ? 20 : -20);
				sendCmd(command);
			}
			return;
		}

		if (ballDistance > 0.75) {
			if (absDouble(ballDirection) > 12.0) {
				sprintf(command, "(turn %.1f)", ballDirection);
			}
			else {
				const int dashPower = iPlayerId <= 3 ? 75 : 90;
				if (cycle - lastChaseSayCycle >= 4) {
					sprintf(command, "(dash %d)(say c%d)", dashPower, iPlayerId);
					lastChaseSayCycle = cycle;
				}
				else {
					sprintf(command, "(dash %d)", dashPower);
				}
			}
			sendCmd(command);
			return;
		}

		if (visual.hasAttackGoal) {
			const int kickPower = goalDistance < 25.0 ? 100 : 70;
			sprintf(command, "(kick %d %.1f)", kickPower, goalDirection);
		}
		else if (visual.hasOwnGoal) {
			// 看不见对方球门：朝远离本方球门方向解围，避免相对身体小偏角造成乌龙。
			const double clearDirection = normalizeAngle(
					visual.ownGoalDirection + 180.0);
			const int clearPower = (iPlayerId == 2 || iPlayerId == 3) ? 90 : 75;
			sprintf(command, "(kick %d %.1f)", clearPower, clearDirection);
		}
		else {
			// 两门都不可见：先转身寻找，下周期再决策（避免盲踢回传危险区）。
			sprintf(command, "(turn %d)", iPlayerId % 2 == 0 ? 40 : -40);
		}
		sendCmd(command);
	}

	void parseMsg(char * msg, const size_t len) {
		if (len > 0) {
			msg[len] = '\0';
		}

		if (!std::strncmp(msg, "(ok compression", 15)) {
			int level;
			if (std::sscanf(msg, "(ok compression %d", &level) == 1) {
				setCompression(level);
			}
			return;
		}

		if (!std::strncmp(msg, "(init", 5)) {
			parseInitMessage(msg);
			M_clean_cycle = true;
			return;
		}

		if (!std::strncmp(msg, "(hear", 5)) {
			int hearCycle = 0;
			if (std::sscanf(msg, "(hear %d", &hearCycle) == 1) {
				handleHearMessage(msg, hearCycle);
			}
			else {
				handleHearMessage(msg, lastSeeCycle);
			}
			M_clean_cycle = true;
			return;
		}

		if (!std::strncmp(msg, "(sense_body", 11)
				|| !std::strncmp(msg, "(see_global", 11)) {
			M_clean_cycle = true;
		}

		if (std::strncmp(msg, "(see ", 5)) return;

		int cycle = -1;
		if (std::sscanf(msg, "(see %d", &cycle) != 1) return;
		if (cycle == lastSeeCycle) return;
		lastSeeCycle = cycle;

		VisualState visual;
		if (!parseVisualState(msg, visual)) {
			return;
		}

		if (iPlayerId == 1) {
			handleGoalkeeper(msg, cycle);
		}
		else if (iPlayerId >= 2 && iPlayerId <= 5) {
			handleFieldPlayer(msg, visual, cycle);
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
				// 从标准输入读取
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

				// 从套接字读取
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
		if (std::strcmp(argv[i], "-team") == 0) {
			if (i + 1 < argc) {
				teamName = argv[i + 1];
				teamNameFromArg = true;
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

	if (iPlayerId < 1 || iPlayerId > 5) {
		std::cerr << "error: -id must be 1..5, got " << iPlayerId << std::endl;
		return EXIT_FAILURE;
	}
	if (iSide != 1 && iSide != 2) {
		std::cerr << "error: specify -sidel or -sider" << std::endl;
		return EXIT_FAILURE;
	}

	client = new Client(server, port);
	client->run();

	return EXIT_SUCCESS;
}
