#define RETRY_LIMIT 3

namespace device {

class Driver {
public:
    int initialize(int* buffer, int length) const;
};

int validate(int* value) { return value != nullptr; }

int Driver::initialize(int* buffer, int length) const {
    if (length > 0) {
        return validate(buffer) ? RETRY_LIMIT : -1;
    }
    return 0;
}

}  // namespace device
