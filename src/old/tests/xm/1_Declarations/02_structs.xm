// ===========================================================================
// Test 02: Struct Footprints and Member Types
// Target: process/ParserDeclarations.cpp
// ===========================================================================

namespace Geometry {

	// Test A: Standard geometric struct with primitives
	struct Point {
		var x = 0;
		var y = 0;
	}

	// Test B: Struct containing another struct (Composition)
	struct Rectangle {
		Point topLeft;
		Point bottomRight;
	}

	// Test C: Empty struct (legal footprint)
	struct EmptyNode {
	}

	// Test D: Struct with named function pointer delegates
	struct CallbackGate {
		var onTrigger; // Handled as an implicit delegate
	}
}

// ---------------------------------------------------------------------------
// INTENTIONAL SYNTAX ERROR
// Uncomment the lines below one at a time to test error reporting!
// ---------------------------------------------------------------------------

// Error A: Missing struct identifier
// struct { var z = 0; }

// Error B: Missing closing brace on struct body
// struct BrokenStruct { var fail = 1;