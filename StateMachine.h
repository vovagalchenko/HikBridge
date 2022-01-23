//
// Created by Vova Galchenko on 1/23/22.
//

#ifndef HIKBRIDGE_STATEMACHINE_H
#define HIKBRIDGE_STATEMACHINE_H

#include <optional>
#include <functional>

class StateMachine {
public:

};

class HikBridgeState {
public:
    std::optional<HikBridgeState> handleBecomingCurrent();
    std::optional<HikBridgeState>
};




#endif //HIKBRIDGE_STATEMACHINE_H
