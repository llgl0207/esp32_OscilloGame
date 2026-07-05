#pragma once
#include <Arduino.h>
#include <vector>
#include <string>

// Message Types
enum NetMsgType {
    MSG_DISCOVERY = 0,
    MSG_PAIR_REQUEST,
    MSG_PAIR_ACCEPT,
    MSG_PAIR_REJECT,
    MSG_DISCONNECT,
    MSG_DATA,
    MSG_START_GAME,
    MSG_GAME_DATA,
    MSG_END_GAME
};

// Game Data Structure (Tank)
struct TankData {
    float x, y, angle;
    uint8_t bullet_count;
    struct {
        float x, y;
    } bullets[5];
};

// Game Data Structure (Pong)
struct PongData {
    float ball_x, ball_y;       // 球的位置
    float ball_vx, ball_vy;     // 球的速度
    float paddle1_x;            // 主机球拍 X 坐标 (底部)
    float paddle2_x;            // 客户机球拍 X 坐标 (顶部, 主机视角)
    uint8_t score1;             // 主机得分
    uint8_t score2;             // 客户机得分
};

// Fixed size packet for simplicity
typedef struct {
    uint8_t type;
    uint8_t src_mac[6];
    char name[16]; // "Player_XXXX"
    union {
        uint8_t padding[8];
        TankData tank_data;
        PongData pong_data;
        struct {
            uint8_t game_id;
            uint32_t seed;
        } start_req;
        struct {
            uint8_t reason; // 0: Quit, 1: Died
        } end_req;
    } payload;
} NetMessage;

// Peer Info
struct PeerInfo {
    uint8_t mac[6];
    String name;
    unsigned long last_seen;
};

enum NetState {
    NET_IDLE,
    NET_DISCOVERING,
    NET_PAIRING,
    NET_CONNECTED,
    NET_IN_GAME
};

class Network_Manager {
public:
    static void init();
    static void disable();
    static void enable();
    static void update();
    static void startDiscovery();
    static void stopDiscovery();
    static void pair(const uint8_t* target_mac);
    static void disconnect();
    
    // Game Methods
    static void startGame(uint8_t gameId, uint32_t seed);
    static void sendGameData(const TankData& data);
    static void sendGameData(const PongData& data);  // Pong 重载
    static void endGame(uint8_t reason);

    static bool hasGameRequest(uint8_t* gameIdOut, uint32_t* seedOut);
    static void clearGameRequest();
    static bool getRemoteGameData(TankData* dataOut);
    static bool getRemoteGameData(PongData* dataOut);  // Pong 重载
    static bool isRemoteGameEnded(uint8_t* reasonOut);
    static void clearRemoteGameEnded();

    static NetState getState();
    static int getPeerCount();
    static const PeerInfo* getPeers();
    static uint8_t getActiveGameId();  // 获取当前活跃游戏 ID (0=无, 1=坦克, 2=乒乓球)

    // Suspend/Resume ESP-NOW (for AI chat)
    static void suspend_esp_now();
    static void resume_esp_now();
};
