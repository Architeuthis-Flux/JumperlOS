// SPDX-License-Identifier: MIT
// Thin delegation onto the shared probe hardware drivers (see header).

#include "ProbeManager.h"

#include "../Commands.h" // showProbeLEDs
#include "../LEDs.h"

ProbeManager* ProbeManager::instance = nullptr;

ProbeManager& ProbeManager::getInstance() {
    if (instance == nullptr) {
        instance = new ProbeManager();
    }
    return *instance;
}

ProbeManager& probeManager = ProbeManager::getInstance();

int ProbeManager::buttonPressEvent(bool consume) {
    return probeButton.getButtonPress(consume);
}

int ProbeManager::buttonState() {
    return probeButton.getButtonState();
}

void ProbeManager::clearButtonState() {
    probeButton.clearButtonState();
}

void ProbeManager::blockButton(unsigned long ms) {
    blockProbeButton = ms;
    blockProbeButtonTimer = millis();
}

bool ProbeManager::buttonBlocked() {
    return blockProbeButton > 0 &&
           (millis() - blockProbeButtonTimer < blockProbeButton);
}

int ProbeManager::switchPos() {
    return switchPosition;
}

int ProbeManager::checkSwitchPosition() {
    return Probing::getInstance().checkSwitchPosition();
}

void ProbeManager::setProbeLEDs(int mode) {
    showProbeLEDs = mode;
}

void ProbeManager::bufferPower(int offOn, int flash, int force) {
    Probing::getInstance().routableBufferPower(offOn, flash, force);
}

float ProbeManager::probeCurrent() {
    return Probing::getInstance().checkProbeCurrent();
}
