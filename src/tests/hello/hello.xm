// tests/hello/hello.xm

extern kernel32:WriteFile(
	handle       ->* u8  : rcx,         // (64-bit) pointer to a byte passed in RCX
	buffer       ->* u8 : rdx,			// .. passed in RDX
	bytesToWrite->u32 : r8,				// u32 value passed in R8
	bytesWritten ->* u32 : r9,          // pointer to a u32 passed in R9
	overlapped   ->* u8                 // on stack no register clause
)->b : rax;

extern kernel32:GetStdHandle(
	stdHandleId->i32 : rcx
) ->* u8 : rax;

extern kernel32:ExitProcess(
	exitCode->u32 : rcx
); 


i32 main() {
	u64 h = GetStdHandle(0xFFFFFFF5);

	u8[] msg = "Hello, world!\n";
	u64  written;

	WriteFile(h, msg, msg.length, &written, null);
}
