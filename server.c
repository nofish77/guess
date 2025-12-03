// server_1.c - 剪刀石头布游戏服务器（改进版：支持玩家断线重连）
// 编译命令：gcc server_1.c -o server_1.exe -lws2_32

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
    // Windows 平台需要包含 Winsock 头文件并链接库
    #include <winsock2.h>
    #include <windows.h>
    #pragma comment(lib, "ws2_32.lib")  // 链接 ws2_32.lib
    #define CLOSESOCKET closesocket      // 定义跨平台关闭 socket 的宏
    #define sleep(x) Sleep((x)*1000)     // Windows 下 Sleep 单位是毫秒
#else
    // Linux/Unix 平台头文件
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <pthread.h>
    #define CLOSESOCKET close
    #define SOCKET int
    #define INVALID_SOCKET (-1)
    #define SOCKET_ERROR (-1)
    typedef int SOCKET;
#endif

#define PORT 8888           // 服务器监听端口
#define BUFFER_SIZE 1024    // 缓冲区大小
#define TIMEOUT_SECONDS 10  // 超时时间（秒）

// 存储两个玩家的 socket 连接
SOCKET player_sockets[2];
// 存储两个玩家的出拳选择（0表示未出拳，1=剪刀，2=石头，3=布）
int player_choices[2];
// 存储两个玩家的IP地址字符串
char player_ips[2][16];  // IPv4 地址最大长度为 15 字符 + 1 结束符
// 比分统计：玩家1胜、玩家2胜、平局
int scores[3] = {0, 0, 0};  // [玩家1胜，玩家2胜，平局]
// 游戏状态标志
int game_started = 0;       // 游戏是否已开始
int both_connected = 0;     // 两个玩家是否都已连接
int round_in_progress = 0;  // 当前是否正在进行一轮游戏
time_t round_start_time = 0; // 当前这一轮开始的时间戳（用于超时判负，从"游戏开始"就开始计时）
int player_disconnected[2] = {0, 0};  // 玩家是否断开连接（0=连接，1=断开）
// 线程同步锁（Windows使用CRITICAL_SECTION，Linux使用pthread_mutex）
#ifdef _WIN32
    CRITICAL_SECTION lock;
#else
    pthread_mutex_t lock;
#endif

/**
 * 获取出拳名称（数字转文字）
 * @param choice 出拳选择（1=剪刀，2=石头，3=布）
 * @return 出拳名称字符串
 */
const char* get_choice_name(int choice) {
    switch(choice) {
        case 1: return "剪刀";
        case 2: return "石头";
        case 3: return "布";
        default: return "未知";
    }
}

/**
 * 判断胜负
 * @param choice1 玩家1的选择
 * @param choice2 玩家2的选择
 * @return 0表示平局，1表示玩家1获胜，2表示玩家2获胜
 */
int judge_winner(int choice1, int choice2) {
    if (choice1 == choice2) {
        return 0;  // 平局
    }
    // 石头(2) > 剪刀(1)
    // 剪刀(1) > 布(3)
    // 布(3) > 石头(2)
    if ((choice1 == 2 && choice2 == 1) ||  // 石头胜剪刀
        (choice1 == 1 && choice2 == 3) ||   // 剪刀胜布
        (choice1 == 3 && choice2 == 2)) {   // 布胜石头
        return 1;  // 玩家1获胜
    }
    return 2;  // 玩家2获胜
}

/**
 * 向指定玩家发送消息
 * @param idx 玩家索引（0 或 1）
 * @param msg 要发送的消息
 */
void send_to(int idx, const char *msg) {
    if (player_sockets[idx] != INVALID_SOCKET && !player_disconnected[idx]) {
        send(player_sockets[idx], msg, strlen(msg), 0);
    }
}

/**
 * 向所有连接的玩家广播消息
 * @param msg 要发送的消息
 */
void broadcast(const char *msg) {
    for (int i = 0; i < 2; i++) {
        if (player_sockets[i] != INVALID_SOCKET && !player_disconnected[i]) {
            send(player_sockets[i], msg, strlen(msg), 0);
        }
    }
}

/**
 * 恢复游戏状态给重连的玩家
 * @param player_idx 玩家索引（0或1）
 */
void restore_game_state(int player_idx) {
    char state_msg[BUFFER_SIZE * 2];
    
    // 如果游戏正在进行，发送完整的游戏开始提示（就像首次连接时一样）
    if (round_in_progress) {
        // 检查玩家是否已经出拳
        if (player_choices[player_idx] != 0) {
            // 玩家已经出拳，告知其选择
            sprintf(state_msg,
                "\n=== 重连成功！已恢复游戏状态 ===\n"
                "当前比分：你 %d : %d 对手（平局 %d 次）\n"
                "\n你已出拳：%s\n"
                "等待对手出拳...\n",
                player_idx == 0 ? scores[0] : scores[1],
                player_idx == 0 ? scores[1] : scores[0],
                scores[2],
                get_choice_name(player_choices[player_idx]));
            send_to(player_idx, state_msg);
        } else {
            // 玩家还未出拳，发送完整的游戏开始提示
            int opponent_idx = 1 - player_idx;
            if (player_choices[opponent_idx] != 0) {
                // 对手已出拳，提示玩家尽快出拳
                sprintf(state_msg,
                    "\n=== 重连成功！已恢复游戏状态 ===\n"
                    "当前比分：你 %d : %d 对手（平局 %d 次）\n"
                    "\n对手已出拳，请尽快出拳！\n"
                    "游戏开始！请输入你的选择：（请在10秒内出拳，否则判负！）\n"
                    "1 - 剪刀\n2 - 石头\n3 - 布\n> ",
                    player_idx == 0 ? scores[0] : scores[1],
                    player_idx == 0 ? scores[1] : scores[0],
                    scores[2]);
            } else {
                // 双方都未出拳，发送完整的游戏开始提示
                sprintf(state_msg,
                    "\n=== 重连成功！已恢复游戏状态 ===\n"
                    "当前比分：你 %d : %d 对手（平局 %d 次）\n"
                    "\n游戏开始！请输入你的选择：（请在10秒内出拳，否则判负！）\n"
                    "1 - 剪刀\n2 - 石头\n3 - 布\n> ",
                    player_idx == 0 ? scores[0] : scores[1],
                    player_idx == 0 ? scores[1] : scores[0],
                    scores[2]);
            }
            send_to(player_idx, state_msg);
        }
    } else {
        // 游戏未在进行，发送比分和等待提示
        sprintf(state_msg,
            "\n=== 重连成功！已恢复游戏状态 ===\n"
            "当前比分：你 %d : %d 对手（平局 %d 次）\n"
            "\n等待继续游戏...\n请输入 y 继续或 n 退出\n> ",
            player_idx == 0 ? scores[0] : scores[1],
            player_idx == 0 ? scores[1] : scores[0],
            scores[2]);
        send_to(player_idx, state_msg);
    }
    
    // 通知另一个玩家
    sprintf(state_msg, "\n玩家%d已重连并恢复游戏状态！\n", player_idx + 1);
    send_to(1 - player_idx, state_msg);
    
    printf("玩家%d重连成功，已恢复游戏状态（比分：%d:%d，平局%d次，轮次进行中：%s）\n",
        player_idx + 1,
        player_idx == 0 ? scores[0] : scores[1],
        player_idx == 0 ? scores[1] : scores[0],
        scores[2],
        round_in_progress ? "是" : "否");
}

/**
 * 处理玩家出拳的线程函数（Windows版本）
 * @param lpParam 玩家索引（0或1）
 * @return 线程退出码
 */
#ifdef _WIN32
DWORD WINAPI handle_player_thread(LPVOID lpParam) {
    int player_idx = (int)(intptr_t)lpParam;
#else
/**
 * 处理玩家出拳的线程函数（Linux版本）
 * @param arg 玩家索引（0或1）
 * @return 线程退出码
 */
void* handle_player_thread(void* arg) {
    int player_idx = (int)(intptr_t)arg;
#endif
    char buffer[BUFFER_SIZE];
    int valread;
    
    // 持续接收该玩家的消息
    while ((valread = recv(player_sockets[player_idx], buffer, BUFFER_SIZE - 1, 0)) > 0) {
        buffer[valread] = '\0';  // 正确终止字符串
        
        // 去除首尾空白字符
        char *input = buffer;
        while (*input == ' ' || *input == '\t' || *input == '\r' || *input == '\n') input++;
        
        if (strlen(input) == 0) continue;  // 忽略空输入
        
        // 进入临界区（加锁）
#ifdef _WIN32
        EnterCriticalSection(&lock);
#else
        pthread_mutex_lock(&lock);
#endif
        
        // 检查是否为重连请求（如果主线程已经处理了，这里会跳过）
        if (strcmp(input, "RECONNECT") == 0) {
            // 玩家重连（主线程可能已经处理了，但这里也处理一次以防万一）
            player_disconnected[player_idx] = 0;  // 标记为已连接
            both_connected = 1;  // 恢复连接状态
            
            // 恢复游戏状态
            restore_game_state(player_idx);
            
            // 退出临界区
#ifdef _WIN32
            LeaveCriticalSection(&lock);
#else
            pthread_mutex_unlock(&lock);
#endif
            continue;
        }
        
        // 如果游戏未开始，等待
        if (!game_started) {
#ifdef _WIN32
            LeaveCriticalSection(&lock);
#else
            pthread_mutex_unlock(&lock);
#endif
            continue;
        }
        
        // 处理出拳选择
        if (round_in_progress) {
            // 检查玩家是否已经出拳
            if (player_choices[player_idx] != 0) {
                // 玩家已经出拳，提示等待
                send_to(player_idx, "你已经出拳了，请等待对手出拳...\n");
            } else {
                // 玩家还未出拳，处理出拳选择
                int choice = atoi(input);
                
                // 验证输入是否有效
                if (choice >= 1 && choice <= 3) {
                    player_choices[player_idx] = choice;
                    printf("收到玩家%d出拳: %s\n", player_idx + 1, get_choice_name(choice));
                    
                    // 立即给玩家反馈
                    char feedback[BUFFER_SIZE];
                    sprintf(feedback, "你出了\"%s\"，等待对手出拳...\n", get_choice_name(choice));
                    send_to(player_idx, feedback);
                    
                    // 检查两个玩家是否都已出拳
                    if (player_choices[0] != 0 && player_choices[1] != 0) {
                        // 两个玩家都已出拳，判断胜负
                        int winner = judge_winner(player_choices[0], player_choices[1]);
                        
                        char result_msg_1[BUFFER_SIZE];
                        char result_msg_2[BUFFER_SIZE];
                        char server_log[BUFFER_SIZE];
                        
                        if (winner == 0) {
                            // 平局
                            scores[2]++;  // 平局数+1
                            sprintf(result_msg_1,
                                "\n结果：%s对%s，平局！\n"
                                "当前比分：你 %d : %d 对手（平局 %d 次）\n"
                                "是否继续？(y/n) > ",
                                get_choice_name(player_choices[0]),
                                get_choice_name(player_choices[1]),
                                scores[0],
                                scores[1],
                                scores[2]);
                            sprintf(result_msg_2,
                                "\n结果：%s对%s，平局！\n"
                                "当前比分：你 %d : %d 对手（平局 %d 次）\n"
                                "是否继续？(y/n) > ",
                                get_choice_name(player_choices[1]),
                                get_choice_name(player_choices[0]),
                                scores[1],
                                scores[0],
                                scores[2]);
                            sprintf(server_log, "结果：%s对%s，平局！\n\n", 
                                get_choice_name(player_choices[0]),
                                get_choice_name(player_choices[1]));
                        } else if (winner == 1) {
                            // 玩家1获胜
                            scores[0]++;
                            sprintf(result_msg_1,
                                "\n你出了\"%s\"\n"
                                "对手出了\"%s\"，%s\n"
                                "当前比分：你 %d : %d 对手（平局 %d 次）\n"
                                "是否继续？(y/n) > ",
                                get_choice_name(player_choices[0]),
                                get_choice_name(player_choices[1]),
                                "你赢了！",
                                scores[0],
                                scores[1],
                                scores[2]);
                            sprintf(result_msg_2,
                                "\n你出了\"%s\"\n"
                                "对手出了\"%s\"，%s\n"
                                "当前比分：你 %d : %d 对手（平局 %d 次）\n"
                                "是否继续？(y/n) > ",
                                get_choice_name(player_choices[1]),
                                get_choice_name(player_choices[0]),
                                "你输了！",
                                scores[1],
                                scores[0],
                                scores[2]);
                            sprintf(server_log, "结果：%s胜%s！玩家1获胜！\n\n",
                                get_choice_name(player_choices[0]),
                                get_choice_name(player_choices[1]));
                        } else {
                            // 玩家2获胜
                            scores[1]++;
                            sprintf(result_msg_1,
                                "\n你出了\"%s\"\n"
                                "对手出了\"%s\"，%s\n"
                                "当前比分：你 %d : %d 对手（平局 %d 次）\n"
                                "是否继续？(y/n) > ",
                                get_choice_name(player_choices[0]),
                                get_choice_name(player_choices[1]),
                                "你输了！",
                                scores[0],
                                scores[1],
                                scores[2]);
                            sprintf(result_msg_2,
                                "\n你出了\"%s\"\n"
                                "对手出了\"%s\"，%s\n"
                                "当前比分：你 %d : %d 对手（平局 %d 次）\n"
                                "是否继续？(y/n) > ",
                                get_choice_name(player_choices[1]),
                                get_choice_name(player_choices[0]),
                                "你赢了！",
                                scores[1],
                                scores[0],
                                scores[2]);
                            sprintf(server_log, "结果：%s胜%s！玩家2获胜！\n\n",
                                get_choice_name(player_choices[1]),
                                get_choice_name(player_choices[0]));
                        }
                        
                        // 发送结果给两个玩家
                        send_to(0, result_msg_1);
                        send_to(1, result_msg_2);
                        printf("%s", server_log);
                        
                        // 重置出拳状态，等待下一轮
                        round_in_progress = 0;
                        player_choices[0] = 0;
                        player_choices[1] = 0;
                    } else {
                        // 只有当前玩家出拳，等待另一个玩家
                        printf("等待玩家%d出拳...\n", 2 - player_idx);
                        // 反馈已在上面发送
                    }
                } else {
                    // 输入无效
                    send_to(player_idx, "输入无效！请输入 1（剪刀）、2（石头）或 3（布）\n> ");
                }
            }
        } else if (!round_in_progress) {
            // 处理继续/退出选择
            if (input[0] == 'y' || input[0] == 'Y') {
                // 开始新一轮（从看到"游戏开始"提示那一刻重新开始计时）
                round_in_progress = 1;
                round_start_time = time(NULL);
                player_choices[0] = 0;
                player_choices[1] = 0;
                const char *new_round_msg = "\n游戏开始！请输入你的选择：（请在10秒内出拳，否则判负！）\n1 - 剪刀\n2 - 石头\n3 - 布\n> ";
                broadcast(new_round_msg);
                printf("开始新一轮游戏...\n");
            } else if (input[0] == 'n' || input[0] == 'N') {
                // 退出游戏
                const char *exit_msg = "\n游戏结束，感谢参与！\n";
                broadcast(exit_msg);
                game_started = 0;
                break;
            } else {
                send_to(player_idx, "请输入 y 继续或 n 退出\n> ");
            }
        }
        
        // 退出临界区（解锁）
#ifdef _WIN32
        LeaveCriticalSection(&lock);
#else
        pthread_mutex_unlock(&lock);
#endif
    }
    
    // 玩家断开连接
    printf("玩家%d断开连接\n", player_idx + 1);
    
    // 进入临界区
#ifdef _WIN32
    EnterCriticalSection(&lock);
#else
    pthread_mutex_lock(&lock);
#endif
    
    // 标记为断开，但不立即结束游戏（允许重连）
    player_disconnected[player_idx] = 1;
    player_sockets[player_idx] = INVALID_SOCKET;
    
    // 如果游戏已开始，通知另一个玩家
    if (game_started) {
        char disconnect_msg[BUFFER_SIZE];
        sprintf(disconnect_msg, "\n玩家%d断开连接，等待其重连...\n", player_idx + 1);
        send_to(1 - player_idx, disconnect_msg);
        printf("玩家%d断开，游戏状态已保存，等待重连...\n", player_idx + 1);
    } else {
        // 如果游戏未开始，完全断开
        both_connected = 0;
        game_started = 0;
    }
    
    // 退出临界区
#ifdef _WIN32
    LeaveCriticalSection(&lock);
#else
    pthread_mutex_unlock(&lock);
#endif
    
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/**
 * 超时检测线程（Windows版本）
 * @param lpParam 未使用
 * @return 线程退出码
 */
#ifdef _WIN32
DWORD WINAPI timeout_thread(LPVOID lpParam) {
#else
/**
 * 超时检测线程（Linux版本）
 * @param arg 未使用
 * @return 线程退出码
 */
void* timeout_thread(void* arg) {
#endif
    while (1) {
        sleep(1);  // 每秒检查一次
        
        // 进入临界区
#ifdef _WIN32
        EnterCriticalSection(&lock);
#else
        pthread_mutex_lock(&lock);
#endif
        
        // 如果游戏正在进行且两个玩家都已连接
        if (round_in_progress && both_connected && !player_disconnected[0] && !player_disconnected[1]) {
            // 使用全局的轮次起始时间，从"游戏开始"广播那一刻就开始计时
            time_t current_time = time(NULL);
            if (round_start_time > 0 && (current_time - round_start_time) >= TIMEOUT_SECONDS) {
                // 找出未出拳的玩家
                int timeout_player = -1;
                if (player_choices[0] == 0 && player_choices[1] != 0) {
                    timeout_player = 0;
                } else if (player_choices[1] == 0 && player_choices[0] != 0) {
                    timeout_player = 1;
                } else if (player_choices[0] == 0 && player_choices[1] == 0) {
                    // 两个玩家都超时，随机选择一个判负
                    timeout_player = (rand() % 2);
                }
                
                if (timeout_player != -1) {
                    // 超时判负（谁没出拳谁输）
                    int winner = 1 - timeout_player;
                    scores[winner]++;
                    
                    char timeout_msg_1[BUFFER_SIZE];
                    char timeout_msg_2[BUFFER_SIZE];
                    sprintf(timeout_msg_1,
                        "\n玩家%d超时未出拳，本局判负！\n"
                        "当前比分：你 %d : %d 对手（平局 %d 次）\n"
                        "是否继续？(y/n) > ",
                        timeout_player + 1,
                        scores[0],
                        scores[1],
                        scores[2]);
                    sprintf(timeout_msg_2,
                        "\n玩家%d超时未出拳，本局判负！\n"
                        "当前比分：你 %d : %d 对手（平局 %d 次）\n"
                        "是否继续？(y/n) > ",
                        timeout_player + 1,
                        scores[1],
                        scores[0],
                        scores[2]);

                    send_to(0, timeout_msg_1);
                    send_to(1, timeout_msg_2);
                    printf("玩家%d超时未出拳，玩家%d获胜！\n\n", timeout_player + 1, winner + 1);
                    
                    // 重置状态：本轮结束，等待是否继续
                    round_in_progress = 0;
                    player_choices[0] = 0;
                    player_choices[1] = 0;
                    round_start_time = 0;
                }
            }
        }
        
        // 退出临界区
#ifdef _WIN32
        LeaveCriticalSection(&lock);
#else
        pthread_mutex_unlock(&lock);
#endif
    }
    
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/**
 * 主函数：启动服务器，处理游戏逻辑
 */
int main() {
    WSADATA wsa;
    SOCKET server_fd, new_sock;
    struct sockaddr_in address;
    int addrlen = sizeof(address);
    
#ifdef _WIN32
    // Windows 初始化 Winsock
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
        printf("WSAStartup failed.\n");
        return 1;
    }
    system("chcp 65001 > nul"); // 设置控制台为 UTF-8 编码，支持中文
    // 初始化临界区
    InitializeCriticalSection(&lock);
#else
    // Linux 初始化互斥锁
    pthread_mutex_init(&lock, NULL);
#endif
    
    // 初始化随机数种子（用于超时判负时的随机选择）
    srand(time(NULL));
    
    // 初始化玩家 socket 数组
    for (int i = 0; i < 2; i++) {
        player_sockets[i] = INVALID_SOCKET;
        player_choices[i] = 0;
        player_disconnected[i] = 0;
    }
    
    // 创建 socket
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == INVALID_SOCKET) {
        printf("Socket creation error\n");
        return 1;
    }
    
    // 允许地址复用
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
    
    // 绑定地址和端口
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);
    
    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        printf("Bind failed\n");
        CLOSESOCKET(server_fd);
        return 1;
    }
    
    // 监听连接
    if (listen(server_fd, 2) < 0) {
        printf("Listen failed\n");
        CLOSESOCKET(server_fd);
        return 1;
    }
    
    printf("【服务器启动】监听端口: %d（支持断线重连）\n", PORT);
    
    // 启动超时检测线程
#ifdef _WIN32
    HANDLE hTimeoutThread = CreateThread(NULL, 0, timeout_thread, NULL, 0, NULL);
    if (hTimeoutThread == NULL) {
        printf("创建超时检测线程失败\n");
    }
#else
    pthread_t timeout_tid;
    if (pthread_create(&timeout_tid, NULL, timeout_thread, NULL) != 0) {
        printf("创建超时检测线程失败\n");
    }
    pthread_detach(timeout_tid);
#endif
    
    // 存储线程句柄（用于重连时重新创建）
#ifdef _WIN32
    HANDLE player_threads[2] = {NULL, NULL};
#else
    pthread_t player_threads[2] = {0, 0};
#endif
    
    // 主循环：接受玩家连接
    while (1) {
        // 重置游戏状态（仅在首次连接时）
        if (!game_started) {
            game_started = 0;
            both_connected = 0;
            round_in_progress = 0;
            scores[0] = scores[1] = scores[2] = 0;
            player_choices[0] = player_choices[1] = 0;
            player_disconnected[0] = player_disconnected[1] = 0;
        }
        
        // 检查是否有玩家需要重连
        int need_reconnect = -1;
        if (game_started) {
            // 如果游戏已开始，检查是否有玩家断开
            if (player_disconnected[0] && player_sockets[0] == INVALID_SOCKET) {
                need_reconnect = 0;
            } else if (player_disconnected[1] && player_sockets[1] == INVALID_SOCKET) {
                need_reconnect = 1;
            }
        }
        
        // 接受玩家连接（首次连接或重连）
        if (need_reconnect >= 0) {
            // 等待断开的玩家重连
            printf("等待玩家%d重连...\n", need_reconnect + 1);
        } else if (!both_connected) {
            // 首次连接：等待玩家1
            if (player_sockets[0] == INVALID_SOCKET) {
                printf("等待玩家1连接...\n");
            } else {
                printf("等待玩家2连接...\n");
            }
        } else {
            // 两个玩家都已连接，继续游戏循环
            sleep(1);
            continue;
        }
        
        if ((new_sock = accept(server_fd, (struct sockaddr*)&address, &addrlen)) == INVALID_SOCKET) {
            printf("Accept failed\n");
            continue;
        }
        
        int player_idx;
        if (need_reconnect >= 0) {
            // 这是重连
            player_idx = need_reconnect;
            player_sockets[player_idx] = new_sock;
            strcpy(player_ips[player_idx], inet_ntoa(address.sin_addr));
            printf("玩家%d重连 (IP: %s)\n", player_idx + 1, player_ips[player_idx]);
            
            // 先恢复连接状态标记
            player_disconnected[player_idx] = 0;
            both_connected = 1;
            
            // 在主线程中立即读取 RECONNECT 消息并恢复状态（确保消息不丢失）
            char reconnect_buffer[BUFFER_SIZE];
            int reconnect_read = recv(new_sock, reconnect_buffer, BUFFER_SIZE - 1, 0);
            if (reconnect_read > 0) {
                reconnect_buffer[reconnect_read] = '\0';
                char *reconnect_input = reconnect_buffer;
                while (*reconnect_input == ' ' || *reconnect_input == '\t' || *reconnect_input == '\r' || *reconnect_input == '\n') reconnect_input++;
                
                if (strcmp(reconnect_input, "RECONNECT") == 0) {
                    printf("在主线程中收到玩家%d的RECONNECT消息\n", player_idx + 1);
                    // 进入临界区
#ifdef _WIN32
                    EnterCriticalSection(&lock);
#else
                    pthread_mutex_lock(&lock);
#endif
                    // 恢复游戏状态（立即发送消息）
                    restore_game_state(player_idx);
                    // 退出临界区
#ifdef _WIN32
                    LeaveCriticalSection(&lock);
#else
                    pthread_mutex_unlock(&lock);
#endif
                } else {
                    printf("警告：收到非RECONNECT消息：%s\n", reconnect_input);
                }
            } else {
                printf("警告：读取重连消息失败，valread=%d\n", reconnect_read);
            }
            
            // 重新创建处理线程（继续处理后续消息）
#ifdef _WIN32
            if (player_threads[player_idx] != NULL) {
                CloseHandle(player_threads[player_idx]);
            }
            player_threads[player_idx] = CreateThread(NULL, 0, handle_player_thread, (LPVOID)(intptr_t)player_idx, 0, NULL);
            if (player_threads[player_idx] == NULL) {
                printf("创建玩家%d处理线程失败\n", player_idx + 1);
            }
#else
            if (player_threads[player_idx] != 0) {
                pthread_detach(player_threads[player_idx]);
            }
            if (pthread_create(&player_threads[player_idx], NULL, handle_player_thread, (void*)(intptr_t)player_idx) != 0) {
                printf("创建玩家%d处理线程失败\n", player_idx + 1);
            }
            pthread_detach(player_threads[player_idx]);
#endif
        } else if (player_sockets[0] == INVALID_SOCKET) {
            // 玩家1首次连接
            player_idx = 0;
            player_sockets[0] = new_sock;
            strcpy(player_ips[0], inet_ntoa(address.sin_addr));
            printf("玩家1已连接 (IP: %s)\n", player_ips[0]);
            
            // 发送等待消息给玩家1
            const char *wait_msg1 = "连接服务器成功...\n正在等待另一位玩家加入...\n";
            send_to(0, wait_msg1);
            
            // 继续等待玩家2
            continue;
        } else {
            // 玩家2首次连接
            player_idx = 1;
            player_sockets[1] = new_sock;
            strcpy(player_ips[1], inet_ntoa(address.sin_addr));
            printf("玩家2已连接 (IP: %s)\n", player_ips[1]);
            
            both_connected = 1;
            game_started = 1;
            round_in_progress = 1;
            round_start_time = time(NULL);
            
            // 通知双方游戏开始
            const char *start_msg = "游戏开始！请输入你的选择：（请在10秒内出拳，否则判负！）\n1 - 剪刀\n2 - 石头\n3 - 布\n> ";
            broadcast(start_msg);
            printf("通知双方：游戏开始！（请在10秒内出拳，否则判负！）\n\n");
            
            // 为每个玩家创建处理线程
#ifdef _WIN32
            for (int i = 0; i < 2; i++) {
                player_threads[i] = CreateThread(NULL, 0, handle_player_thread, (LPVOID)(intptr_t)i, 0, NULL);
                if (player_threads[i] == NULL) {
                    printf("创建玩家%d处理线程失败\n", i + 1);
                }
            }
#else
            for (int i = 0; i < 2; i++) {
                if (pthread_create(&player_threads[i], NULL, handle_player_thread, (void*)(intptr_t)i) != 0) {
                    printf("创建玩家%d处理线程失败\n", i + 1);
                }
                pthread_detach(player_threads[i]);  // 分离线程，不等待结束
            }
#endif
        }
    }
    
    // 清理资源
    CLOSESOCKET(server_fd);
#ifdef _WIN32
    DeleteCriticalSection(&lock);
    WSACleanup();
#else
    pthread_mutex_destroy(&lock);
#endif
    
    return 0;
}

