extern b:rax kernel32:WriteFile(u64:rcx handle, *u8:rdx buffer, u32:r8 bytesToWrite, *u32:r9 bytesWritten, *u8 overlapped);
extern u64:rax kernel32:GetStdHandle(i32:rcx stdHandleId);

i32 farewell() {
    u64 h = GetStdHandle(0xFFFFFFF5);
    u8[] msg = "Farewell from xmc!\n";
    u64 written;
    WriteFile(h, msg, msg.length, &written, null);
}
