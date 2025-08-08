#pragma once
#include <stack>

namespace std {

template<typename T>
class mstack : public stack<T> {
public:
    T getAndPop() {
        T top = this->top();
        this->pop();
        return top;
    }
};

}  // namespace std
