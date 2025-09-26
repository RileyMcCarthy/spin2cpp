// Test for designated initializers
struct Point {
    int x;
    int y;
    int z;
};

struct Nested {
    struct Point p;
    int value;
};

// Test basic struct designated initializers
struct Point test_basic() {
    struct Point p = {.x = 10, .y = 20};
    return p;
}

// Test out-of-order designated initializers
struct Point test_outoforder() {
    struct Point p = {.z = 5, .x = 15};
    return p;
}

// Test mixed positional and designated initializers
struct Point test_mixed() {
    struct Point p = {1, .z = 30};
    return p;
}

// Test nested designated initializers
struct Nested test_nested() {
    struct Nested n = {.p.x = 100, .value = 200};
    return n;
}

// Test array designated initializers
int test_array() {
    int arr[5] = {[2] = 10, [4] = 20};
    return arr[2] + arr[4];  // Should return 30
}

// Test array of structs with designated initializers
struct Point test_array_struct() {
    struct Point points[2] = {{.x = 1, .y = 2}, {.x = 3, .y = 4}};
    return points[1];
}
