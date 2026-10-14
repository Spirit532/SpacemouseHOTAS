// Kernel uinput devices: the virtual HOTAS and a keyboard/mouse for key and button mappings.
#pragma once
#include "platform/platform.h"

class UinputPad : public VirtualPad {
public:
    ~UinputPad() override { close(); }
    bool open(int deviceId) override;
    void close() override;
    bool isOpen() const override { return fd_ >= 0; }
    bool send(const float* axes, uint32_t buttons, int hat) override;
    const char* status() const override { return status_; }
    const char* help() const override;

private:
    int fd_ = -1;
    uint32_t sentButtons_ = 0;
    char status_[96] = "uinput not opened";
};

// Created on first use so systems without key mappings never get an extra input device.
void injectKey(int vk, bool down);
void injectMouseButton(int button, bool down);
