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
    MSG_DATA // For game data later
};

// Fixed size packet for simplicity
typedef struct {
    uint8_t type;
    uint8_t src_mac[6];
    char name[16]; // "Player_XXXX"
    uint8_t padding[8]; 
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
    NET_CONNECTED
};

class Network_Manager {
public:
    static void init();
    static void update();
    static void startDiscovery();
    static void stopDiscovery();
    static void pair(const uint8_t* target_mac);
    static void disconnect();

    static NetState getState();
    static int getPeerCount();
    static const PeerInfo* getPeers();
};
