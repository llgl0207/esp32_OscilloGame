#include "network_manager.h"
#include <WiFi.h>
#include <esp_now.h>

static NetState current_state = NET_IDLE;
static std::vector<PeerInfo> discovered_peers;
static uint8_t my_mac[6];
static char my_name[16];

// Connection details
static uint8_t connected_peer_mac[6];
static uint8_t pending_peer_mac[6]; // For incoming/outgoing requests
static unsigned long last_broadcast_time = 0;
static const unsigned long BROADCAST_INTERVAL = 500; // 500ms
static const unsigned long PEER_TIMEOUT = 3000; // 3s timeout for peers

// Callbacks
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
    // Handle send status if needed
}

void OnDataRecv(const uint8_t *mac, const uint8_t *incomingData, int len) {
    if (len != sizeof(NetMessage)) return;
    
    NetMessage *msg = (NetMessage *)incomingData;
    
    // 1. Discovery
    if (msg->type == MSG_DISCOVERY) {
        if (current_state == NET_DISCOVERING) {
            // Check if exists
            bool found = false;
            for (auto &p : discovered_peers) {
                if (memcmp(p.mac, msg->src_mac, 6) == 0) {
                    p.last_seen = millis();
                    found = true;
                    break;
                }
            }
            if (!found) {
                PeerInfo p;
                memcpy(p.mac, msg->src_mac, 6);
                p.name = String(msg->name);
                p.last_seen = millis();
                discovered_peers.push_back(p);
            }
        }
    }
    // 2. Pair Request
    else if (msg->type == MSG_PAIR_REQUEST) {
        if (current_state == NET_DISCOVERING || current_state == NET_IDLE) {
            // Auto-accept for simplicity in this demo
            // In a real app, we would show a prompt
            
            // Send Accept
            if (!esp_now_is_peer_exist(msg->src_mac)) {
                esp_now_peer_info_t peerInfo;
                memset(&peerInfo, 0, sizeof(peerInfo));
                memcpy(peerInfo.peer_addr, msg->src_mac, 6);
                peerInfo.channel = 0;  
                peerInfo.encrypt = false;
                peerInfo.ifidx = WIFI_IF_AP; // Use AP interface since we are in AP_STA mode and AP is active
                esp_now_add_peer(&peerInfo);
            }
            
            NetMessage reply;
            reply.type = MSG_PAIR_ACCEPT;
            memcpy(reply.src_mac, my_mac, 6);
            strncpy(reply.name, my_name, 16);
            esp_now_send(msg->src_mac, (uint8_t *) &reply, sizeof(reply));
            
            memcpy(connected_peer_mac, msg->src_mac, 6);
            current_state = NET_CONNECTED;
        }
    }
    // 3. Pair Accept
    else if (msg->type == MSG_PAIR_ACCEPT) {
        if (current_state == NET_PAIRING) {
             memcpy(connected_peer_mac, msg->src_mac, 6);
             current_state = NET_CONNECTED;
        }
    }
    // 4. Disconnect
    else if (msg->type == MSG_DISCONNECT) {
        if (current_state == NET_CONNECTED) {
            if (memcmp(connected_peer_mac, msg->src_mac, 6) == 0) {
                current_state = NET_IDLE;
                esp_now_del_peer(connected_peer_mac);
            }
        }
    }
    // 5. Data (Test)
    else if (msg->type == MSG_DATA) {
        if (current_state == NET_CONNECTED) {
            // Verify it is from connected peer
            if (memcmp(connected_peer_mac, msg->src_mac, 6) == 0) {
                Serial.printf("Received DATA from Peer: %02X:%02X:%02X:%02X:%02X:%02X\n",
                    msg->src_mac[0], msg->src_mac[1], msg->src_mac[2], 
                    msg->src_mac[3], msg->src_mac[4], msg->src_mac[5]);
            }
        }
    }
}

void Network_Manager::init() {
    // Set mode to AP_STA to allow both Web Server (AP) and ESP-NOW
    WiFi.mode(WIFI_AP_STA);
    
    WiFi.macAddress(my_mac);
    sprintf(my_name, "Player_%02X:%02X", my_mac[4], my_mac[5]);
    
    if (esp_now_init() != ESP_OK) {
        return;
    }
    
    esp_now_register_send_cb(OnDataSent);
    esp_now_register_recv_cb(OnDataRecv);
}

void Network_Manager::update() {
    // Cleanup old peers
    if (current_state == NET_DISCOVERING) {
        for (auto it = discovered_peers.begin(); it != discovered_peers.end(); ) {
            if (millis() - it->last_seen > PEER_TIMEOUT) {
                it = discovered_peers.erase(it);
            } else {
                ++it;
            }
        }
        
        // Broadcast Presence
        if (millis() - last_broadcast_time > BROADCAST_INTERVAL) {
            last_broadcast_time = millis();
            
            NetMessage msg;
            msg.type = MSG_DISCOVERY;
            memcpy(msg.src_mac, my_mac, 6);
            strncpy(msg.name, my_name, 16);
            
            uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
            
            if (!esp_now_is_peer_exist(broadcastAddress)) {
                esp_now_peer_info_t peerInfo;
                memset(&peerInfo, 0, sizeof(peerInfo));
                memcpy(peerInfo.peer_addr, broadcastAddress, 6);
                peerInfo.channel = 0;  
                peerInfo.encrypt = false;
                peerInfo.ifidx = WIFI_IF_AP; // Broadcast on AP interface
                esp_now_add_peer(&peerInfo);
            }
            
            esp_now_send(broadcastAddress, (uint8_t *) &msg, sizeof(msg));
        }
    }
    
    // Connected Logic (Test Heartbeat)
    if (current_state == NET_CONNECTED) {
        static unsigned long last_data_time = 0;
        if (millis() - last_data_time > 1000) {
            last_data_time = millis();
            
            NetMessage msg;
            msg.type = MSG_DATA;
            memcpy(msg.src_mac, my_mac, 6);
            strncpy(msg.name, my_name, 16);
            
            // Send to connected peer
            esp_now_send(connected_peer_mac, (uint8_t *) &msg, sizeof(msg));
             Serial.println("Sent MSG_DATA (Heartbeat)");
        }
    }
}

void Network_Manager::startDiscovery() {
    if (current_state == NET_CONNECTED) return;
    current_state = NET_DISCOVERING;
    discovered_peers.clear();
}

void Network_Manager::stopDiscovery() {
    if (current_state == NET_DISCOVERING) {
        current_state = NET_IDLE;
    }
}

void Network_Manager::pair(const uint8_t* target_mac) {
    if (current_state == NET_CONNECTED) return;
    
    memcpy(pending_peer_mac, target_mac, 6);
    
    if (!esp_now_is_peer_exist(pending_peer_mac)) {
        esp_now_peer_info_t peerInfo;
        memset(&peerInfo, 0, sizeof(peerInfo));
        memcpy(peerInfo.peer_addr, pending_peer_mac, 6);
        peerInfo.channel = 0;  
        peerInfo.encrypt = false;
        peerInfo.ifidx = WIFI_IF_AP; // Use AP interface
        esp_now_add_peer(&peerInfo);
    }
    
    NetMessage msg;
    msg.type = MSG_PAIR_REQUEST;
    memcpy(msg.src_mac, my_mac, 6);
    strncpy(msg.name, my_name, 16);
    
    esp_now_send(pending_peer_mac, (uint8_t *) &msg, sizeof(msg));
    
    current_state = NET_PAIRING;
}

void Network_Manager::disconnect() {
    if (current_state == NET_CONNECTED) {
        NetMessage msg;
        msg.type = MSG_DISCONNECT;
        memcpy(msg.src_mac, my_mac, 6);
        strncpy(msg.name, my_name, 16);
        
        esp_now_send(connected_peer_mac, (uint8_t *) &msg, sizeof(msg));
        
        esp_now_del_peer(connected_peer_mac);
        current_state = NET_IDLE;
    }
}

NetState Network_Manager::getState() {
    return current_state;
}

int Network_Manager::getPeerCount() {
    return discovered_peers.size();
}

const PeerInfo* Network_Manager::getPeers() {
    return discovered_peers.data();
}
