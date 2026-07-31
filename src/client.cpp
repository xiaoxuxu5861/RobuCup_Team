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
// ===== 守门员状态机 =====
enum GoalieState {
    GS_HOLDING,      // 守位（默认）
    GS_ALIGNING,     // 横向调整
    GS_CHASING,      // 迎球（威胁球）
    GS_CATCHING,     // 正在扑球
    GS_CLEARING,     // 接球后解围
    GS_RETURNING     // 回门
};

GoalieState goalieState = GS_HOLDING;
int goalieLastBallDistance = -1;
int goalieLastBallCycle = -1;
int goalieCatchAttemptCycle = -100;
bool goalieCatchSuccess = false;
int goalieStateEntryCycle = 0;
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

struct BodyState {
	bool hasStamina;
	double stamina;
	double effort;
	int cycle;
};

BodyState gBodyState = { false, 8000.0, 1.0, -1 };
int lastHeardChaseUnum = 0;
int lastHeardChaseCycle = -100;
int lastPassTargetUnum = 0;
int lastPassCycle = -100;
int lastChaseSayCycle = -100;
const char *gLastChaseReason = "none";
const char *gLastAttackAction = "none";
int lastForwardMode = 0; // 1 chase, 2 support
int lastForwardModeCycle = -100;

double absDouble(double value);
double normalizeAngle(double angle);

void clearTransientAttackState() {
	lastHeardChaseUnum = 0;
	lastHeardChaseCycle = -100;
	lastPassTargetUnum = 0;
	lastPassCycle = -100;
	lastChaseSayCycle = -100;
	lastForwardMode = 0;
	lastForwardModeCycle = -100;
	gLastChaseReason = "restart";
	gLastAttackAction = "restart_wait";
}

bool isRestartHoldMode() {
	return gMatchState.playMode == PM_BEFORE_KICK_OFF
			|| gMatchState.playMode == PM_GOAL_L
			|| gMatchState.playMode == PM_GOAL_R;
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
	if (isRestartHoldMode()) {
		clearTransientAttackState();
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
	if (quoted == 0) {
		return;
	}

	if (quoted[1] == 'p') {
		const int target = quoted[2] - '0';
		if (target >= 2 && target <= 5) {
			lastPassTargetUnum = target;
			lastPassCycle = cycle;
			printf("pass_heard: cycle=%d target=%d self=%d\n",
					cycle, target, iPlayerId);
		}
		return;
	}

	if (quoted[1] != 'c') {
		return;
	}

	const int claimed = quoted[2] - '0';
	if (claimed < 2 || claimed > 5 || claimed == iPlayerId) {
		return;
	}
	lastHeardChaseUnum = claimed;
	lastHeardChaseCycle = cycle;
}

bool parseSenseBodyMessage(const char *msg) {
	int cycle = -1;
	if (std::sscanf(msg, "(sense_body %d", &cycle) != 1) {
		return false;
	}

	const char *stamina = std::strstr(msg, "(stamina ");
	if (stamina == 0) {
		return false;
	}

	double value = 0.0;
	double effort = 1.0;
	if (std::sscanf(stamina, "(stamina %lf %lf", &value, &effort) < 1) {
		return false;
	}

	gBodyState.hasStamina = true;
	gBodyState.stamina = value;
	gBodyState.effort = effort;
	gBodyState.cycle = cycle;
	printf("body_parse: cycle=%d stamina=%.1f effort=%.2f\n",
			gBodyState.cycle, gBodyState.stamina, gBodyState.effort);
	return true;
}

int limitPower(int value, int maximum) {
	return value > maximum ? maximum : value;
}

int staminaDashPower(int basePower, bool chasing) {
	if (!gBodyState.hasStamina) {
		return basePower;
	}
	if (gBodyState.effort < 0.65) {
		return chasing ? limitPower(basePower, 65) : limitPower(basePower, 25);
	}
	if (gBodyState.stamina < 1800.0) {
		return chasing ? limitPower(basePower, 55) : limitPower(basePower, 20);
	}
	if (gBodyState.stamina < 3500.0) {
		return chasing ? limitPower(basePower, 75) : limitPower(basePower, 35);
	}
	if (gBodyState.stamina < 5200.0) {
		return chasing ? limitPower(basePower, 90) : limitPower(basePower, 45);
	}
	return basePower;
}

bool findTeammate(const VisualState &state, int unum, SeenPlayer &player) {
	for (int i = 0; i < state.teammateCount; ++i) {
		if (state.teammates[i].unum == unum) {
			player = state.teammates[i];
			return true;
		}
	}
	return false;
}

bool hasNearbyOpponent(const VisualState &state, double distanceLimit) {
	for (int i = 0; i < state.opponentCount; ++i) {
		if (state.opponents[i].distance <= distanceLimit) {
			return true;
		}
	}
	return false;
}

bool hasOpponentInLane(const VisualState &state, double laneDirection,
		double distanceLimit, double angleLimit) {
	for (int i = 0; i < state.opponentCount; ++i) {
		const double diff = normalizeAngle(state.opponents[i].direction
				- laneDirection);
		if (state.opponents[i].distance <= distanceLimit
				&& absDouble(diff) <= angleLimit) {
			return true;
		}
	}
	return false;
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
    double ballDistance = 0.0, ballDirection = 0.0;
    double goalDistance = 0.0, goalDirection = 0.0;
    const int hasBall = readBallInfo(msg, ballDistance, ballDirection);
    const int hasAttackGoal = readAttackGoalInfo(msg, goalDistance, goalDirection);
    char command[128];

    // 检测球速趋势（射门威胁判断）
    bool ballApproaching = false;
    if (goalieLastBallDistance > 0) {
        ballApproaching = (ballDistance < goalieLastBallDistance - 0.3);
    }
    goalieLastBallDistance = (int)ballDistance;
    goalieLastBallCycle = cycle;

    // 状态切换逻辑
    switch (goalieState) {
        case GS_HOLDING: {
            // 球在禁区内且快速靠近 → 转为迎球
            if (hasBall && ballDistance < 12.0 && ballApproaching) {
                goalieState = GS_CHASING;
                goalieStateEntryCycle = cycle;
                break;
            }
            // 球在可扑范围内 → 转为扑球
            if (hasBall && ballDistance <= 1.5) {
                goalieState = GS_CATCHING;
                goalieStateEntryCycle = cycle;
                break;
            }
            // 保持守位：沿门线横向移动
            if (hasBall) {
                double alignDir = (ballDirection > 5.0) ? 15.0 : (ballDirection < -5.0 ? -15.0 : 0.0);
                if (fabs(alignDir) > 1.0) {
                    sprintf(command, "(turn %.1f)", alignDir);
                    sendCmd(command);
                } else {
                    sprintf(command, "(dash 20)");
                    sendCmd(command);
                }
            } else {
                sprintf(command, "(turn 10)");
                sendCmd(command);
            }
            break;
        }

        case GS_CHASING: {
            // 球靠近且可扑 → 转为扑球
            if (hasBall && ballDistance <= 1.5) {
                goalieState = GS_CATCHING;
                goalieStateEntryCycle = cycle;
                break;
            }
            // 球远离或超过12米 → 回门
            if (!hasBall || ballDistance > 14.0) {
                goalieState = GS_RETURNING;
                goalieStateEntryCycle = cycle;
                break;
            }
            // 迎球移动
            if (ballDistance > 2.0) {
                sprintf(command, "(dash 60)");
                sendCmd(command);
            } else {
                sprintf(command, "(dash 30)");
                sendCmd(command);
            }
            // 转向球
            sprintf(command, "(turn %.1f)", ballDirection);
            sendCmd(command);
            break;
        }

        case GS_CATCHING: {
            // 接球后解围
            if (goalieCatchAttemptCycle > 0 && cycle - goalieCatchAttemptCycle <= 5) {
                if (hasBall && ballDistance < 1.5) {
                    double kickDir = hasAttackGoal ? goalDirection : 0.0;
                    if (fabs(kickDir) > 45.0) kickDir = (kickDir > 0) ? 30.0 : -30.0;
                    sprintf(command, "(kick 100 %.1f)", kickDir);
                    sendCmd(command);
                    goalieState = GS_CLEARING;
                    goalieStateEntryCycle = cycle;
                    goalieCatchAttemptCycle = -100;
                    break;
                }
            }
            // 冷却检查：如果刚尝试过 catch（10个周期内），不再重复发送
    if (goalieCatchAttemptCycle > 0 && cycle - goalieCatchAttemptCycle < 10) {
        // 冷却中，只转向，不扑球
        sprintf(command, "(turn %.1f)", ballDirection);
        sendCmd(command);
        break;
    }
            // 尝试扑球
            if (hasBall && ballDistance <= 1.15) {
                sprintf(command, "(catch %.1f)", ballDirection);
                goalieCatchAttemptCycle = cycle;
                sendCmd(command);
            } else if (hasBall && ballDistance <= 3.0) {
                sprintf(command, "(dash 40)");
                sendCmd(command);
            } else {
                // 没扑到，回门
                goalieState = GS_RETURNING;
                goalieStateEntryCycle = cycle;
            }
            break;
        }

        case GS_CLEARING: {
            // 解围后回到守位
            if (cycle - goalieStateEntryCycle > 3) {
                goalieState = GS_HOLDING;
                goalieStateEntryCycle = cycle;
            }
            sprintf(command, "(turn 10)");
            sendCmd(command);
            break;
        }

        case GS_RETURNING: {
            // 回到门线附近
            if (ballDistance > 8.0 && ballDistance < 15.0) {
                sprintf(command, "(dash -30)");
                sendCmd(command);
            } else {
                goalieState = GS_HOLDING;
                goalieStateEntryCycle = cycle;
            }
            sprintf(command, "(turn %.1f)", ballDirection);
            sendCmd(command);
            break;
        }

        default: {
            goalieState = GS_HOLDING;
            goalieStateEntryCycle = cycle;
            break;
        }
    }
}

	bool isForwardPlayer() {
		return iPlayerId == 4 || iPlayerId == 5;
	}

	bool isFreshPassTarget(int cycle) {
		return isForwardPlayer()
				&& lastPassTargetUnum == iPlayerId
				&& cycle - lastPassCycle >= 0
				&& cycle - lastPassCycle <= 8;
	}

	int stableForwardChaseDecision(int mayChase, double ballDistance,
			double chaseLimit, int cycle) {
		if (!isForwardPlayer()) {
			return mayChase;
		}

		const int desiredMode = (mayChase && ballDistance <= chaseLimit) ? 1 : 2;
		if (lastForwardMode != 0 && desiredMode != lastForwardMode
				&& cycle - lastForwardModeCycle < 4) {
			return lastForwardMode == 1;
		}

		if (desiredMode != lastForwardMode) {
			lastForwardMode = desiredMode;
			lastForwardModeCycle = cycle;
		}
		return desiredMode == 1;
	}

	void logAttackDecision(int cycle, const VisualState &visual,
			const char *action, const char *command) {
		printf("attack_debug: cycle=%d id=%d action=%s stamina=%.1f ball=%.1f goal=%d %.1f partner=%d opp=%d command=%s\n",
				cycle, iPlayerId, action,
				gBodyState.hasStamina ? gBodyState.stamina : -1.0,
				visual.hasBall ? visual.ballDistance : -1.0,
				visual.hasAttackGoal ? 1 : 0,
				visual.hasAttackGoal ? visual.attackGoalDistance : -1.0,
				visual.teammateCount, visual.opponentCount, command);
	}

	void makeForwardSupportCommand(const VisualState &visual, char *command) {
		const double supportOffset = iPlayerId == 5 ? 28.0 : -24.0;
		const double laneDirection = normalizeAngle(visual.ballDirection
				+ supportOffset);
		const int supportPower = staminaDashPower(iPlayerId == 5 ? 55 : 45,
				false);

		if (visual.ballDistance < 6.0) {
			gLastAttackAction = "support_widen";
			if (absDouble(laneDirection) > 10.0) {
				sprintf(command, "(turn %.1f)", laneDirection);
			}
			else {
				sprintf(command, "(dash %d)", supportPower);
			}
			return;
		}

		if (visual.ballDistance > 18.0) {
			gLastAttackAction = "support_catchup";
			if (absDouble(visual.ballDirection) > 12.0) {
				sprintf(command, "(turn %.1f)", visual.ballDirection);
			}
			else {
				sprintf(command, "(dash %d)", supportPower);
			}
			return;
		}

		gLastAttackAction = "support_lane";
		if (absDouble(laneDirection) > 16.0) {
			sprintf(command, "(turn %.1f)", laneDirection);
		}
		else {
			sprintf(command, "(dash %d)", staminaDashPower(40, false));
		}
	}

	void makeForwardPossessionCommand(const VisualState &visual, int cycle,
			char *command) {
		const int partnerUnum = iPlayerId == 4 ? 5 : 4;
		SeenPlayer partner;
		const bool hasPartner = findTeammate(visual, partnerUnum, partner);
		const bool underPressure = hasNearbyOpponent(visual, 5.0);
		const bool goalBlocked = visual.hasAttackGoal
				&& hasOpponentInLane(visual, visual.attackGoalDirection,
						10.0, 18.0);

		const bool usefulPass = hasPartner
				&& partner.distance >= 4.0
				&& partner.distance <= 24.0
				&& absDouble(partner.direction) <= 55.0
				&& ((iPlayerId == 4 && (underPressure || goalBlocked))
						|| (iPlayerId == 5 && underPressure
								&& (!visual.hasAttackGoal
										|| visual.attackGoalDistance > 20.0)));
		if (usefulPass) {
			gLastAttackAction = "pass";
			sprintf(command, "(kick 50 %.1f)(say p%d)",
					partner.direction, partnerUnum);
			return;
		}

		if (visual.hasAttackGoal && visual.attackGoalDistance <= 28.0
				&& !goalBlocked) {
			gLastAttackAction = "shoot";
			const int shootPower = gBodyState.hasStamina
					&& gBodyState.stamina < 3000.0 ? 82 : 100;
			sprintf(command, "(kick %d %.1f)", shootPower,
					visual.attackGoalDirection);
			return;
		}

		if (!visual.hasAttackGoal) {
			gLastAttackAction = "advance_scan";
			if (absDouble(visual.ballDirection) > 8.0) {
				sprintf(command, "(turn %.1f)", visual.ballDirection);
			}
			else if (visual.ballDistance <= 0.55) {
				const double scanDirection = iPlayerId == 4 ? 32.0 : -32.0;
				sprintf(command, "(turn %.1f)", scanDirection);
			}
			else {
				const double safeTouchDirection = iPlayerId == 4 ? 18.0 : -18.0;
				sprintf(command, "(kick 8 %.1f)", safeTouchDirection);
			}
			return;
		}

		if (visual.attackGoalDistance > 34.0) {
			gLastAttackAction = "dribble_touch";
			if (absDouble(visual.attackGoalDirection) > 25.0) {
				sprintf(command, "(turn %.1f)", visual.attackGoalDirection);
			}
			else {
				sprintf(command, "(kick 16 %.1f)", visual.attackGoalDirection);
			}
		}
		else {
			gLastAttackAction = "advance_touch";
			if (absDouble(visual.attackGoalDirection) > 35.0) {
				sprintf(command, "(turn %.1f)", visual.attackGoalDirection);
			}
			else {
				sprintf(command, "(kick 24 %.1f)", visual.attackGoalDirection);
			}
		}
		(void)cycle;
	}

	void handleFieldPlayer(const char *msg, const VisualState &visual,
			int cycle) {
		char command[160];

		if (isRestartHoldMode()) {
			gLastAttackAction = "restart_wait";
			if (isForwardPlayer()) {
				logAttackDecision(cycle, visual, gLastAttackAction, "none");
			}
			if (visual.hasBall && absDouble(visual.ballDirection) > 12.0) {
				sprintf(command, "(turn %.1f)", visual.ballDirection);
				sendCmd(command);
			}
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

		const int mayChase = shouldChaseBall(visual, iPlayerId);
		const int passTarget = isFreshPassTarget(cycle);
		printf("chase_debug: cycle=%d id=%d side=%d srvSide=%d playMode=%d ball=%.1f tm=%d chase=%d pass=%d reason=%s\n",
				cycle, iPlayerId, iSide, gMatchState.sideFromServer,
				static_cast<int>(gMatchState.playMode), ballDistance,
				visual.teammateCount, mayChase, passTarget, gLastChaseReason);
		int activeChase = stableForwardChaseDecision(mayChase,
				ballDistance, chaseLimit, cycle);
		double effectiveChaseLimit = chaseLimit;
		if (passTarget) {
			activeChase = 1;
			effectiveChaseLimit = 30.0;
			gLastAttackAction = "receive";
		}

		if (!activeChase || ballDistance > effectiveChaseLimit) {
			if (isForwardPlayer()) {
				makeForwardSupportCommand(visual, command);
				logAttackDecision(cycle, visual, gLastAttackAction, command);
				sendCmd(command);
				return;
			}
			if (makeDepthCommand(msg, minimumHomeDistance,
					maximumHomeDistance, command)) {
				sendCmd(command);
			}
			else if (absDouble(ballDirection) > 6.0) {
				sprintf(command, "(turn %.1f)", ballDirection);
				sendCmd(command);
			}
			else {
				sprintf(command, "(turn 0)");
				sendCmd(command);
			}
			return;
		}

		if (ballDistance > 0.75) {
			if (absDouble(ballDirection) > 12.0) {
				sprintf(command, "(turn %.1f)", ballDirection);
				if (isForwardPlayer()) {
					gLastAttackAction = passTarget ? "receive_turn" : "chase_turn";
				}
			}
			else {
				int dashPower = iPlayerId <= 3 ? 82 : 100;
				if (isForwardPlayer() && ballDistance < 4.0) {
					dashPower = passTarget ? 80 : 75;
				}
				dashPower = staminaDashPower(dashPower, true);
				if (cycle - lastChaseSayCycle >= 4) {
					sprintf(command, "(dash %d)(say c%d)", dashPower, iPlayerId);
					lastChaseSayCycle = cycle;
				}
				else {
					sprintf(command, "(dash %d)", dashPower);
				}
				if (isForwardPlayer()) {
					gLastAttackAction = passTarget ? "receive_dash" : "chase_dash";
				}
			}
			if (isForwardPlayer()) {
				logAttackDecision(cycle, visual, gLastAttackAction, command);
			}
			sendCmd(command);
			return;
		}

		if (isForwardPlayer()) {
			makeForwardPossessionCommand(visual, cycle, command);
			logAttackDecision(cycle, visual, gLastAttackAction, command);
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

		if (!std::strncmp(msg, "(sense_body", 11)) {
			parseSenseBodyMessage(msg);
			M_clean_cycle = true;
			return;
		}

		if (!std::strncmp(msg, "(see_global", 11)) {
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
