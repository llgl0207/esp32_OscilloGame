#ifndef MICROPHONE_H
#define MICROPHONE_H

#include <Arduino.h>
#include <driver/i2s.h>

class Microphone {
public:
    Microphone(int bck_pin, int ws_pin, int data_pin, uint32_t sample_rate);
    ~Microphone();
    bool init();
    size_t read(int16_t* buffer, size_t buffer_len);
private:
    int         _bck_pin;
    int         _ws_pin;
    int         _data_pin;
    uint32_t    _sample_rate;
    i2s_port_t  _port;
    bool        _is_initialized;
};

#endif // MICROPHONE_H
