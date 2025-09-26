// Test for designated initializers on P2
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

// ===== NEW CONST ARRAY DESIGNATED INITIALIZER TESTS =====

// Test simple const array with designated initializers
const struct Point const_simple_array[2] = {
    {.x = 10, .y = 20},
    {.z = 30, .x = 40}
};

// Test const array with multi-field designated initializers
const struct Point const_multi_array[1] = {
    {.x = 100, .y = 200, .z = 300}
};

// Test const array with mixed positional and designated initializers
const struct Point const_mixed_array[3] = {
    {1, 2, 3},                    // positional
    {.x = 10, .y = 20},          // designated (z defaults to 0)
    {.z = 30, .x = 40}           // designated out of order
};

// Test const array with nested designated initializers
const struct Nested const_nested_array[2] = {
    {
        .p = {.x = 10, .y = 20, .z = 30},
        .value = 100
    },
    {
        .value = 200,
        .p = {.z = 60, .x = 40, .y = 50}  // out of order
    }
};

// Test const array with unsigned arithmetic (original user case)
struct Config {
    int pin;
    unsigned int maxValue;
};

const struct Config const_unsigned_array[1] = {
    {
        .pin = 5,
        .maxValue = (65535U * 2U)  // This was the original failing case
    }
};

// Test deeply nested const structures
struct Inner {
    int a, b;
};

struct Middle {
    struct Inner inner;
    int c;
};

struct Outer {
    struct Middle middle;
    int d;
};

const struct Outer const_deep_nested[1] = {
    {
        .middle = {
            .inner = {.a = 1, .b = 2},
            .c = 3
        },
        .d = 4
    }
};

// Test const array with partial initialization
const struct Point const_partial_array[3] = {
    {.x = 10},                   // y and z default to 0
    {.y = 20, .z = 30},         // x defaults to 0
    {}                           // all fields default to 0
};

// Test const array with complex expressions
const struct Config const_complex_expr[2] = {
    {.pin = 5 + 3, .maxValue = 100 * 2},
    {.pin = (10 - 2), .maxValue = (0xFFFF + 1)}
};

// Functions to test that the const arrays work correctly
int test_const_arrays() {
    // Test simple const array
    if (const_simple_array[0].x != 10 || const_simple_array[0].y != 20) return 1;
    if (const_simple_array[1].z != 30 || const_simple_array[1].x != 40) return 2;
    
    // Test multi-field const array
    if (const_multi_array[0].x != 100 || const_multi_array[0].y != 200 || const_multi_array[0].z != 300) return 3;
    
    // Test nested const array
    if (const_nested_array[0].p.x != 10 || const_nested_array[0].value != 100) return 4;
    if (const_nested_array[1].p.z != 60 || const_nested_array[1].value != 200) return 5;
    
    // Test unsigned arithmetic const array
    if (const_unsigned_array[0].pin != 5 || const_unsigned_array[0].maxValue != 131070) return 6;
    
    // Test deep nested const array
    if (const_deep_nested[0].middle.inner.a != 1 || const_deep_nested[0].d != 4) return 7;
    
    return 0; // All tests passed
}
