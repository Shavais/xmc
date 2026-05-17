extern kernel32:ExitProcess(u32:rcx exitCode);

i32 main() {
    greet();
    farewell();
    ExitProcess(0);
}
