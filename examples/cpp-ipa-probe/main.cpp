#include <stdint.h>

class Answer {
public:
    int value() const { return 42; }
};

int main() {
    const Answer answer;
    return answer.value();
}
