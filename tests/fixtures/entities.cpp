#define APPLY(value) ((value) + 1)

template <typename T> struct Box {
  T value;
  T get() const { return APPLY(value); }
};

template <typename T> struct Box<T *> {
  T *value;
};

enum class Mode { idle = 1, active = 2 };
using Count = int;

int declared(int value);
int declared(int value) { return value; }

template struct Box<int>;

struct AnonymousMembers {
  union {
    struct {
      int first;
    };
    struct {
      int second;
    };
  };
};
