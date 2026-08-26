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

bool enabled() { return true; }
bool disabled() { return false; }
double low_ratio() { return 1.5; }
double high_ratio() { return 3.0; }
char first_character() { return 'a'; }
char second_character() { return 'b'; }
const wchar_t *wide_text() { return L"history"; }

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
