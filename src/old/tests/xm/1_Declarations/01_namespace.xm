// ===========================================================================
// Test 01: Namespaces and Scoping
// Target: process/ParserDeclarations.cpp & ParserStatements.cpp
// ===========================================================================

namespace Core {
    var global_speed = 100;
    
    namespace Math {
        var pi = 3.14159;
        
        // Nested variable targeting global_speed should be visible in future passes
        var double_speed = Core.global_speed * 2;
    }
    
    // Testing duplicate scope resolution in same file
    namespace Math {
        var e = 2.71828;
    }
}

// ---------------------------------------------------------------------------
// INTENTIONAL SYNTAX ERROR
// Uncomment the lines below one at a time to test error reporting!
// ---------------------------------------------------------------------------

// Error A: Missing identifier after namespace keyword
// namespace { var x = 5; }

// Error B: Missing closing brace on namespace
namespace Broken { var y = 10; }