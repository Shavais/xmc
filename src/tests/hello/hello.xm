// tests/hello/hello.xm

extern b:rax kernel32:WriteFile(
    u64:rcx  handle,          // opaque 64-bit handle (Windows HANDLE)
    *u8:rdx  buffer,          // pointer to the data buffer
    u32:r8   bytesToWrite,    // u32 value passed in R8
    *u32:r9  bytesWritten,    // pointer to a u32 passed in R9
    *u8      overlapped       // on the stack — no register clause
);

extern u64:rax kernel32:GetStdHandle(
    i32:rcx stdHandleId
);

extern kernel32:ExitProcess(
    u32:rcx exitCode
);


i32 main() {
    u64 h = GetStdHandle(0xFFFFFFF5);

    u8[] msg = "Hello, world!\n";
    u64  written;

    WriteFile(h, msg, msg.length, &written, null);
}
